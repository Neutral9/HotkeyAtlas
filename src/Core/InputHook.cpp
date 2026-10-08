// Input hook: combos, keys moved to mouse / gamepad buttons and mod key remaps.

#include "Internal.h"

namespace HA
{
    // ---------------------------------------------------------------- combo input hook
    // Game thread only: rebuilt whenever overrides change, read by the input hook.

    namespace
    {
        struct ComboBind
        {
            int               ctx;
            std::uint32_t     key;
            std::uint8_t      mods;
            std::uint32_t     hold;  // input that must be held too (see HoldOf), 0 = none
            RE::BSFixedString event;
        };

        // The dlls a mod remap is shown to (see Binding::readers). Everything else that reads the
        // game's input (the game, other mods) keeps seeing the real keys. Null: every reader.
        using Readers = std::shared_ptr<const std::vector<HMODULE>>;

        // A mod's key moved by the user: pressing `from` makes the mod see `to` (its own key).
        struct Remap
        {
            std::uint32_t from;  // combo the user presses now
            std::uint32_t to;    // combo written in the mod's config
            Readers       readers;
        };

        // A mod's original combo, hidden from it (the user moved it elsewhere).
        struct Blocked
        {
            std::uint32_t code;
            Readers       readers;
        };

        // What a held key / button is being shown as, until it is released.
        struct Shown
        {
            std::uint32_t to;  // combo, or kHiddenKey
            Readers       readers;
        };

        std::map<std::string, std::vector<fs::path>> g_remapReaders;  // FileEditId() -> dlls, from the scan (g_ovLock)
        std::set<HMODULE>                            g_readerModules;  // every dll of a remap's readers (game thread)

        // A bind used by a double tap or by holding its input (see Trigger), of a Skyrim control
        // (`event`, in context `ctx`) or of a mod key (`to`, the combo in the mod's config).
        struct TriggerBind
        {
            std::uint32_t     code;  // input without its Trigger; may carry a held input
            Trigger           trigger;
            int               ctx;  // -1: a mod key, any context
            RE::BSFixedString event;
            std::uint32_t     to = kUnbound;
        };
        std::vector<TriggerBind> g_triggers;
        std::atomic<bool>        g_triggersPaused{ false };  // a bind is being captured in the menu

        std::vector<ComboBind>                               g_combos;
        // Skyrim controls moved from a key to a mouse button, or given an extra gamepad button:
        // the button press gets the event
        struct ButtonBind
        {
            int               ctx;
            std::uint32_t     code;  // mouse or gamepad code, see MakeCode; may carry a held input
            RE::BSFixedString event;
        };
        std::vector<ButtonBind>                              g_buttonBinds;
        std::vector<ButtonBind>                              g_stickBinds;  // same, on a stick used as a button
        bool                                                 g_stickUsed = false;  // any stick bind or remap from a stick
        std::unordered_map<std::uint32_t, RE::BSFixedString> g_activeButtons;  // mouse code held -> event
        std::unordered_map<std::uint32_t, RE::BSFixedString> g_activeCombos;  // key held as part of a combo -> event
        std::vector<Remap>                                   g_remaps;
        std::vector<Blocked>                                 g_blocked;       // mods' original combos, hidden from them
        std::unordered_map<std::uint32_t, Shown>             g_activeRemaps;  // physical key held -> combo shown to mods

        constexpr std::uint32_t kHiddenKey = 0xFF;  // idCode given to a blocked key press
    }

    // Caller holds g_ovLock.
    void RebuildComboTableLocked()
    {
        g_combos.clear();
        g_buttonBinds.clear();
        g_stickBinds.clear();
        g_triggers.clear();
        // <ctx>|<event>|<original key> -> ctx, event
        const auto parse = [](const std::string& id) -> std::optional<std::pair<int, RE::BSFixedString>> {
            const auto p1 = id.find('|');
            const auto p2 = id.rfind('|');
            if (p1 == npos || p2 == p1) return std::nullopt;
            const auto ctx = ParseUInt(std::string_view(id).substr(0, p1));
            if (!ctx) return std::nullopt;
            return std::pair{ static_cast<int>(*ctx), RE::BSFixedString(id.substr(p1 + 1, p2 - p1 - 1)) };
        };
        // a double tap / hold is told apart from a plain press by the hook (see ProcessInput)
        const auto addTrigger = [](std::uint32_t key, int ctx, RE::BSFixedString event, std::uint32_t to) {
            g_triggers.push_back({ WithTrigger(key, Trigger::Press), TriggerOf(key), ctx, std::move(event), to });
        };
        for (const auto& [id, ov] : g_overrides) {
            if (TriggerOf(ov.key) != Trigger::Press) {
                if (const auto p = parse(id)) addTrigger(ov.key, p->first, p->second, kUnbound);
                continue;
            }
            const bool held    = HoldOf(ov.key) != 0;
            const bool combo   = CodeDevice(ov.key) == Device::Keyboard && ov.key != kUnbound && (ComboMods(ov.key) || held);
            const bool toMouse = CodeDevice(ov.original) == Device::Keyboard && CodeDevice(ov.key) == Device::Mouse;
            const bool toStick = IsStick(ov.key) && !IsStick(ov.original);
            const bool padHold = held && CodeDevice(ov.key) == Device::Gamepad;  // gamepad combo: fired by the hook too
            if (!combo && !toMouse && !toStick && !padHold) continue;
            const auto p = parse(id);
            if (!p) continue;
            const auto& [ctx, event] = *p;
            if (combo)
                g_combos.push_back({ ctx, ComboKey(ov.key), ComboMods(ov.key), HoldOf(ov.key), event });
            else
                (toStick ? g_stickBinds : g_buttonBinds).push_back({ ctx, ov.key, event });
        }
        for (const auto& [id, ov] : g_padControls)
            if (const auto p = parse(id)) {
                if (TriggerOf(ov.key) != Trigger::Press)
                    addTrigger(ov.key, p->first, p->second, kUnbound);
                else
                    (IsStick(ov.key) ? g_stickBinds : g_buttonBinds).push_back({ p->first, ov.key, p->second });
            }

        g_remaps.clear();
        g_blocked.clear();
        g_readerModules.clear();
        // the loaded dlls reading a mod key, by its FileEditId()
        const auto readersOf = [](const std::string& id) -> Readers {
            const auto it = g_remapReaders.find(id);
            if (it == g_remapReaders.end()) return nullptr;
            std::vector<HMODULE> mods;
            for (const auto& p : it->second)
                if (auto* m = GetModuleHandleW(p.c_str())) mods.push_back(m);
            if (mods.empty()) return nullptr;
            g_readerModules.insert(mods.begin(), mods.end());
            return std::make_shared<const std::vector<HMODULE>>(std::move(mods));
        };
        const auto addRemap = [&](const std::string& id, const Override& ov) {
            if (TriggerOf(ov.key) != Trigger::Press)
                addTrigger(ov.key, -1, {}, ov.original);  // double tap / hold: shown to every reader
            else
                g_remaps.push_back({ ov.key, ov.original, readersOf(id) });
        };
        // an unbound key has no new combo: nothing to translate, only the old one to hide
        // (keys written into a mod's YAML file need no translating: the file has them)
        for (const auto& [id, ov] : g_fileEdits)
            if (ov.key != ov.original && ov.key != kUnbound && !IsYamlEditId(id)) addRemap(id, ov);
        // an added gamepad button shows the mod its own key; the key itself keeps working
        for (const auto& [id, ov] : g_padMods) addRemap(id, ov);
        g_stickUsed = !g_stickBinds.empty() || std::ranges::any_of(g_remaps, [](const Remap& r) { return IsStick(r.from); });
        // the old combo stops working for the mod, unless it is also some remap's new combo
        for (const auto& [id, ov] : g_fileEdits)
            if (ov.key != ov.original && !IsYamlEditId(id) && std::ranges::none_of(g_remaps, [&](const Remap& o) { return o.from == ov.original; }))
                g_blocked.push_back({ ov.original, readersOf(id) });
    }

    void SetRemapReadersLocked(std::map<std::string, std::vector<fs::path>> readers) { g_remapReaders = std::move(readers); }

    // Physical modifier state straight from Windows: works regardless of which UI has focus.
    std::uint8_t HeldModsOS()
    {
        std::uint8_t m = 0;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= kShift;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= kCtrl;
        if (GetAsyncKeyState(VK_MENU) & 0x8000) m |= kAlt;
        return m;
    }

    namespace
    {
        int ActiveContext()
        {
            auto* cm = RE::ControlMap::GetSingleton();
            if (!cm) return 0;
            const auto& stack = cm->GetRuntimeData().contextPriorityStack;
            return stack.empty() ? 0 : static_cast<int>(stack.back());
        }

        // Synthetic modifier events, allocated once from the game heap and reused every frame.
        // They are linked into the event list only for the duration of one dispatch.
        class EventPool
        {
        public:
            void Reset() { _used = 0; }

            // Keyboard key. down: IsDown() (first frame of a press); up: IsUp()
            RE::InputEvent* Button(std::uint32_t dik, bool down)
            {
                return Button(RE::INPUT_DEVICE::kKeyboard, dik, down ? 1.0f : 0.0f, down ? 0.0f : 0.1f, ""sv);  // no event: no Skyrim action
            }

            RE::InputEvent* Button(RE::INPUT_DEVICE device, std::uint32_t id, float value, float held, const RE::BSFixedString& event)
            {
                if (_used == _pool.size()) {
                    auto* e = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, ""sv, id, 0.0f, 0.0f);
                    if (!e) return nullptr;
                    _pool.push_back(e);
                }
                auto* e   = _pool[_used++];
                e->device = device;
                e->SetIDCode(id);
                e->SetUserEvent(event);
                e->GetRuntimeData().value        = value;
                e->GetRuntimeData().heldDownSecs = held;
                e->next                          = nullptr;
                return e;
            }

        private:
            std::vector<RE::ButtonEvent*> _pool;
            std::size_t                   _used = 0;
        };
        EventPool g_eventPool;

        // Synthetic modifier presses/releases taking what mods see from state `from` to `to`.
        void ModifierTransition(std::uint8_t from, std::uint8_t to, std::vector<RE::InputEvent*>& out)
        {
            for (int bit = 0; bit < 3; ++bit) {
                const bool f = from & kModBits[bit], t = to & kModBits[bit];
                if (f == t) continue;
                if (auto* e = g_eventPool.Button(kModDik[bit], t)) out.push_back(e);
            }
        }

        // Keys whose "down" is sent one frame late, after the synthetic modifiers (see present()).
        std::vector<std::pair<std::uint32_t, Readers>> g_pendingDown;

        // Synthetic key releases for mouse wheel ticks shown to mods as a key: the wheel has
        // no "up", so the key is let go a couple of frames later.
        struct PendingUp
        {
            std::uint32_t combo;
            int           frames;
            Readers       readers;
        };
        std::vector<PendingUp> g_pendingUp;

        // ---- gamepad state straight from XInput: the menu does not pass gamepad buttons on,
        // and a stick used as a button needs its deflection

        using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

        XInputGetStateFn XInputGetStatePtr()
        {
            static const auto fn = []() -> XInputGetStateFn {
                for (auto dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
                    if (auto* h = LoadLibraryW(dll)) return reinterpret_cast<XInputGetStateFn>(GetProcAddress(h, "XInputGetState"));
                return nullptr;
            }();
            return fn;
        }
    }

    // Every connected pad merged. Asking XInput about an empty slot is slow, so empty slots
    // are probed only once a second (this runs every frame while a stick is in use).
    PadState ReadPads()
    {
        static std::atomic<DWORD>     connected{ 0 };
        static std::atomic<ULONGLONG> nextProbe{ 0 };
        PadState                      out;
        auto*                         get = XInputGetStatePtr();
        if (!get) return out;
        const auto now   = GetTickCount64();
        const bool probe = now >= nextProbe.load();
        if (probe) nextProbe = now + 1000;
        const auto push = [](SHORT x, SHORT y) {
            return (std::min)(1.0f, std::sqrt(static_cast<float>(x) * x + static_cast<float>(y) * y) / 32767.0f);
        };
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            const DWORD bit = 1u << i;
            if (!(connected & bit) && !probe) continue;
            XINPUT_STATE st{};
            if (get(i, &st) != ERROR_SUCCESS) {
                connected &= ~bit;
                continue;
            }
            connected |= bit;
            out.buttons |= st.Gamepad.wButtons;
            if (st.Gamepad.bLeftTrigger > 128) out.buttons |= 1u << 16;
            if (st.Gamepad.bRightTrigger > 128) out.buttons |= 1u << 17;
            out.stick[0] = (std::max)(out.stick[0], push(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY));
            out.stick[1] = (std::max)(out.stick[1], push(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY));
        }
        return out;
    }

    namespace
    {
        // A stick used as a button: pushed past the threshold = pressed, back to the centre = released.
        struct StickPress
        {
            bool              down    = false;
            ULONGLONG         since   = 0;         // GetTickCount64() at the press
            std::uint32_t     remapTo = kUnbound;  // mod key shown while it is pushed
            Readers           readers;             // ... to these dlls
            RE::BSFixedString event;               // or the Skyrim control it fires
        };
        StickPress g_stickPress[2];  // left, right

        // ---- double tap / hold

        constexpr ULONGLONG kDoubleTapMs = 300;  // longest gap between the two taps, and longest tap
        constexpr ULONGLONG kHoldMs      = 400;  // how long an input is held for a hold

        // A press of an input that has double tap / hold binds, until it is clear what it was.
        // Its events are held back meanwhile; a plain tap is replayed afterwards, so whatever
        // the input does on its own keeps working, a bit later.
        struct TriggerPress
        {
            enum class Stage
            {
                Down,      // pressed, not yet a hold
                Released,  // tapped once, waiting for a second tap
                Fired,     // a double tap / hold: its events go to that bind until release
                Passing    // held too long for a tap: an ordinary press
            };
            Stage                      stage;
            std::uint32_t              phys;  // MakeCode of the key / button
            RE::INPUT_DEVICE           device;
            std::uint32_t              id;
            RE::BSFixedString          event;  // what the game made of the press, for the replay
            ULONGLONG                  since;  // press (Down), release (Released), firing (Fired)
            std::optional<TriggerBind> dbl, hold;
            TriggerBind                fired{};
            float                      heldOffset = 0.0f;  // secs held before a hold fired
        };
        std::vector<TriggerPress> g_presses;

        // Replayed taps: their release comes one frame after their press.
        struct ReplayUp
        {
            RE::INPUT_DEVICE  device;
            std::uint32_t     id;
            RE::BSFixedString event;
        };
        std::vector<ReplayUp> g_replayUps;

        // No double tap / hold for mod keys while text is typed: letters would come late.
        bool Typing()
        {
            if (auto* cm = RE::ControlMap::GetSingleton(); cm && cm->GetRuntimeData().textEntryCount > 0) return true;
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::Console::MENU_NAME);
        }
    }

    void PauseTriggers(bool paused) { g_triggersPaused = paused; }

    // Whether the held part of a combo is down right now, read from Windows / XInput: the
    // event stream only tells about changes.
    bool IsHeld(std::uint32_t hold)
    {
        const auto id = CodeId(hold);
        switch (CodeDevice(hold)) {
        case Device::Keyboard:
            if (const auto bit = ModBitForDik(id)) return HeldModsOS() & bit;  // either Shift, Ctrl, Alt
            if (const auto vk = DikToVk(id)) return GetAsyncKeyState(static_cast<int>(*vk)) & 0x8000;
            return false;
        case Device::Mouse:
            {
                static constexpr int kVk[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
                return id < std::size(kVk) && (GetAsyncKeyState(kVk[id]) & 0x8000);
            }
        default:
            {
                const auto pads = ReadPads();
                if (id == kPadLeftStick || id == kPadRightStick) return pads.stick[id == kPadRightStick] > 0.6f;
                return pads.buttons & (id == kPadLT ? 1u << 16 : id == kPadRT ? 1u << 17 : id);
            }
        }
    }

    namespace
    {
        bool HoldOk(std::uint32_t code)
        {
            const auto h = HoldOf(code);
            return !h || IsHeld(h);
        }
    }

    // Of the entries whose code (without the held part) is `base` and whose held input is
    // down, the most specific one: a combo with a held input wins over the plain key.
    template <class T, class Code>
    T* BestMatch(std::vector<T>& v, std::uint32_t base, Code code, int ctx = -1)
    {
        T* best = nullptr;
        for (auto& e : v) {
            if constexpr (requires { e.ctx; })
                if (e.ctx != ctx) continue;
            const auto c = code(e);
            if (BaseCode(c) != base || !HoldOk(c)) continue;
            if (!best || (HoldOf(c) && !HoldOf(code(*best)))) best = &e;
        }
        return best;
    }

    namespace
    {
        bool IsKeyCombo(std::uint32_t to) { return to != kUnbound && to != kHiddenKey && CodeDevice(to) == Device::Keyboard; }

        // Whether mods are being shown a key of theirs, with its modifiers, in place of what is
        // held; if so, to which dlls (null: everyone).
        std::optional<Readers> PresentingCombo()
        {
            std::optional<Readers> out;
            bool                   everyone = false;
            const auto             add      = [&](const Readers& r) {
                everyone |= !r;
                if (!out) out = r;
            };
            for (const auto& [key, r] : g_pendingDown) add(r);
            for (const auto& p : g_pendingUp) add(p.readers);
            for (const auto& [code, s] : g_activeRemaps)
                if (IsKeyCombo(s.to)) add(s.readers);
            for (const auto& s : g_stickPress)
                if (s.down && IsKeyCombo(s.remapTo)) add(s.readers);
            for (const auto& p : g_presses)
                if (p.stage == TriggerPress::Stage::Fired && IsKeyCombo(p.fired.to)) add(nullptr);
            if (out && everyone) out = Readers{};
            return out;
        }

        // ---- one frame's input seen two ways: by the dlls a mod remap is for, and by everyone
        // else (the game, other mods), who keep the real keys. The engine below builds the
        // readers' view; what it changed for them only is recorded here and undone while the
        // list goes to everyone else.
        struct Split
        {
            struct Edit
            {
                RE::ButtonEvent* e;
                std::uint32_t    id[2];  // [0] everyone, [1] the readers
                RE::INPUT_DEVICE dev[2];
                Readers          readers;
            };
            std::vector<RE::InputEvent*>                     order;  // the readers' list
            std::vector<std::pair<RE::InputEvent*, Readers>> only;   // made up for some readers only
            std::vector<Edit>                                edits;
            RE::InputEvent*                                  everyone = nullptr;
            bool                                             on       = false;

            void Clear()
            {
                order.clear();
                only.clear();
                edits.clear();
                everyone = nullptr;
                on       = false;
            }
            bool Empty() const { return only.empty() && edits.empty(); }

            // Sets the events up as `mod` sees them (nullptr: everyone else) and links its list.
            RE::InputEvent* View(HMODULE mod)
            {
                const auto reads = [&](const Readers& r) { return mod && r && std::ranges::find(*r, mod) != r->end(); };
                for (auto& x : edits) {
                    const int v = reads(x.readers);
                    x.e->SetIDCode(x.id[v]);
                    x.e->device = x.dev[v];
                }
                RE::InputEvent* head = nullptr;
                RE::InputEvent* tail = nullptr;
                for (auto* e : order) {
                    const auto it = std::ranges::find_if(only, [&](const auto& o) { return o.first == e; });
                    if (it != only.end() && !reads(it->second)) continue;
                    (tail ? tail->next : head) = e;
                    tail = e;
                }
                if (tail) tail->next = nullptr;
                return head;
            }
        };
        Split g_split;

        // ---- input sinks (BSTEventSink<InputEvent*>) of the reader dlls: their ProcessEvent is
        // wrapped so they get their own view of each frame.
        using ProcessEventFn = RE::BSEventNotifyControl (*)(RE::BSTEventSink<RE::InputEvent*>*, RE::InputEvent* const*,
            RE::BSTEventSource<RE::InputEvent*>*);
        struct SinkClass
        {
            HMODULE        module = nullptr;
            ProcessEventFn orig   = nullptr;  // set once wrapped
        };
        std::unordered_map<void**, SinkClass> g_sinkClasses;   // by vtable; game thread
        std::set<HMODULE>                     g_sinkModules;   // reader dlls whose sinks are wrapped
        bool                                  g_listReplaced = false;

        RE::BSEventNotifyControl SinkThunk(RE::BSTEventSink<RE::InputEvent*>* self, RE::InputEvent* const* ev, RE::BSTEventSource<RE::InputEvent*>* src)
        {
            const auto it = g_sinkClasses.find(*reinterpret_cast<void***>(self));
            if (it == g_sinkClasses.end() || !it->second.orig) return RE::BSEventNotifyControl::kContinue;
            const auto orig = it->second.orig;
            if (!g_split.on || !ev) return orig(self, ev, src);
            if (*ev != g_split.everyone) {
                // a mod hooked in below us handed the sinks a list of its own: no view to give
                if (!std::exchange(g_listReplaced, true))
                    logger::warn("input hook: another mod replaced the input list, mod remaps can't reach their mod this frame");
                return orig(self, ev, src);
            }
            RE::InputEvent* mine   = g_split.View(it->second.module);
            const auto      result = orig(self, &mine, src);
            g_split.View(nullptr);
            return result;
        }

        // Wraps the sinks of reader dlls registered since the last frame. Game thread, before dispatch.
        void RefreshSinks(RE::BSTEventSource<RE::InputEvent*>* source)
        {
            g_sinkModules.clear();
            if (g_readerModules.empty()) return;
            // the source being dispatched: BSInputDeviceManager
            if (!source) return;
            RE::BSSpinLockGuard l(source->lock);
            for (auto* sink : source->sinks) {
                if (!sink) continue;
                auto** vtbl = *reinterpret_cast<void***>(sink);
                auto   it   = g_sinkClasses.find(vtbl);
                if (it == g_sinkClasses.end()) {
                    HMODULE mod = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(vtbl[1]), &mod);
                    it = g_sinkClasses.emplace(vtbl, SinkClass{ mod, nullptr }).first;
                }
                auto& c = it->second;
                if (!c.module || !g_readerModules.contains(c.module)) continue;
                if (!c.orig) {
                    DWORD old = 0;
                    if (!VirtualProtect(&vtbl[1], sizeof(void*), PAGE_READWRITE, &old)) continue;
                    c.orig  = reinterpret_cast<ProcessEventFn>(vtbl[1]);
                    vtbl[1] = reinterpret_cast<void*>(&SinkThunk);
                    VirtualProtect(&vtbl[1], sizeof(void*), old, &old);
                    wchar_t name[MAX_PATH]{};
                    GetModuleFileNameW(c.module, name, MAX_PATH);
                    logger::info("input hook: {} gets its remapped keys alone, other mods keep the real ones", Utf8(fs::path(name).filename()));
                }
                g_sinkModules.insert(c.module);
            }
        }

        // A remap's readers if it can be shown to them alone, else null (shown to everyone).
        Readers Scoped(const Readers& r)
        {
            if (!r) return nullptr;
            return std::ranges::any_of(*r, [](HMODULE m) { return g_sinkModules.contains(m); }) ? r : nullptr;
        }

        // Rewrites keyboard button events before any sink sees them:
        //  - vanilla combos get the user event of their control;
        //  - a remapped mod key is shown to mods as the key in their own config (idCode),
        //    with fake modifier presses when that key needs different modifiers;
        //  - the mod's original key is hidden (idCode 0xFF), Skyrim's own action on it stays.
        // Returns the (possibly new) head of the list; `restore` undoes the relinking.
        RE::InputEvent* ProcessInput(RE::InputEvent* head, std::vector<std::pair<RE::InputEvent*, RE::InputEvent*>>& restore)
        {
            if (g_combos.empty() && g_activeCombos.empty() && g_remaps.empty() && g_activeRemaps.empty() && g_pendingDown.empty() &&
                g_buttonBinds.empty() && g_activeButtons.empty() && g_pendingUp.empty() && !g_stickUsed && !g_stickPress[0].down &&
                !g_stickPress[1].down && g_triggers.empty() && g_presses.empty() && g_replayUps.empty() && !g_triggersPaused && !InputBlockBusy())
                return head;
            g_eventPool.Reset();

            std::vector<RE::InputEvent*> seq;
            bool                         relinked = false;
            int                          ctx      = -1;
            const auto                   held     = HeldModsOS();
            std::optional<bool>          typingNow;  // a text field or the console has the keyboard (asked once)

            // For a remap shown to some dlls only (see Split): an event made up for them, and a
            // real one changed for them (recorded before the change).
            const auto only = [&](RE::InputEvent* e, const Readers& r) {
                if (e && r) g_split.only.emplace_back(e, r);
            };
            const auto edit = [&](RE::ButtonEvent* b, const Readers& r) {
                if (!r || std::ranges::any_of(g_split.edits, [&](const Split::Edit& x) { return x.e == b; })) return;
                g_split.edits.push_back({ b, { b->GetIDCode(), 0 }, { b->GetDevice(), b->GetDevice() }, r });
            };
            const auto modifiers = [&](std::uint8_t from, std::uint8_t to, const Readers& r, std::vector<RE::InputEvent*>& out) {
                const auto start = out.size();
                ModifierTransition(from, to, out);
                for (auto i = start; i < out.size(); ++i) only(out[i], r);
            };

            // A key press made up for a mod whose key was moved to a mouse button, with the
            // modifiers it needs (the key itself one frame later when they change, see present()).
            const auto keyDown = [&](std::uint32_t combo, const Readers& r) {
                std::vector<RE::InputEvent*> mods;
                modifiers(held, ComboMods(combo), r, mods);
                seq.insert(seq.end(), mods.begin(), mods.end());
                if (!mods.empty()) {
                    g_pendingDown.emplace_back(ComboKey(combo), r);
                } else if (auto* d = g_eventPool.Button(ComboKey(combo), true)) {
                    only(d, r);
                    seq.push_back(d);
                }
                relinked = true;
            };
            const auto keyUp = [&](std::uint32_t combo, const Readers& r) {
                if (auto* u = g_eventPool.Button(ComboKey(combo), false)) {
                    only(u, r);
                    seq.push_back(u);
                }
                modifiers(ComboMods(combo), held, r, seq);
                relinked = true;
            };

            // the delayed key downs go first, before this frame's events (a quick tap's "up" may follow)
            for (const auto& [key, r] : std::exchange(g_pendingDown, {}))
                if (auto* d = g_eventPool.Button(key, true)) {
                    only(d, r);
                    seq.push_back(d);
                    relinked = true;
                }
            for (auto it = g_pendingUp.begin(); it != g_pendingUp.end();) {
                if (--it->frames > 0) {
                    ++it;
                    continue;
                }
                keyUp(it->combo, it->readers);
                it = g_pendingUp.erase(it);
            }

            // Sticks used as buttons. Read from XInput every frame: thumbstick events are not
            // sent while a stick stays put. A held press repeats every frame like a real button.
            if (g_stickUsed || g_stickPress[0].down || g_stickPress[1].down) {
                const auto pads = ReadPads();
                const auto now  = GetTickCount64();
                for (int s = 0; s < 2; ++s) {
                    auto&      st = g_stickPress[s];
                    const auto id = s ? kPadRightStick : kPadLeftStick;
                    const bool on = pads.stick[s] > (st.down ? 0.35f : 0.6f);  // hysteresis: no flicker at the edge
                    if (!on && !st.down) continue;
                    const bool press = !st.down;
                    if (press) {
                        const auto code = MakeCode(Device::Gamepad, id);
                        st              = {};
                        if (const auto r = std::ranges::find(g_remaps, code, &Remap::from); r != g_remaps.end()) {
                            st.remapTo = r->to;
                            st.readers = Scoped(r->readers);
                        } else if (!g_stickBinds.empty()) {
                            if (ctx < 0) ctx = ActiveContext();
                            const auto b = std::ranges::find_if(g_stickBinds, [&](const ButtonBind& b) { return b.code == code && b.ctx == ctx; });
                            if (b != g_stickBinds.end()) st.event = b->event;
                        }
                        if (st.remapTo == kUnbound && st.event.empty()) continue;  // nothing on this stick here
                        st.down  = true;
                        st.since = now;
                    }
                    const bool release = !on;
                    if (release) st.down = false;
                    if (st.remapTo != kUnbound && CodeDevice(st.remapTo) == Device::Keyboard) {
                        if (press)
                            keyDown(st.remapTo, st.readers);
                        else if (release)
                            keyUp(st.remapTo, st.readers);
                        continue;
                    }
                    const float     secs = press ? 0.0f : (std::max)(0.001f, static_cast<float>(now - st.since) / 1000.0f);
                    RE::InputEvent* e    = nullptr;
                    if (st.remapTo != kUnbound)  // a mod's mouse or gamepad button
                        e = g_eventPool.Button(CodeDevice(st.remapTo) == Device::Mouse ? RE::INPUT_DEVICE::kMouse : RE::INPUT_DEVICE::kGamepad,
                            CodeId(st.remapTo), release ? 0.0f : 1.0f, secs, ""sv);
                    else
                        e = g_eventPool.Button(RE::INPUT_DEVICE::kGamepad, id, release ? 0.0f : 1.0f, secs, st.event);
                    if (e) {
                        if (st.remapTo != kUnbound) only(e, st.readers);
                        seq.push_back(e);
                        relinked = true;
                    }
                }
            }

            // ---- double tap / hold. Presses of inputs with such binds are taken out of the list
            // until it is clear what they are; everything else, and the replayed taps, go on
            // through the rest below (`input`).
            std::vector<RE::InputEvent*> input;
            const auto                   nowMs = GetTickCount64();

            // the press of a plain tap, shown again (with the game's own event for it)
            const auto replay = [&](const TriggerPress& p, bool down) {
                const auto e = g_eventPool.Button(p.device, p.id, down ? 1.0f : 0.0f, down ? 0.0f : 0.05f, p.event);
                if (e) input.push_back(e);
                relinked = true;
            };
            const auto hideButton = [&](RE::ButtonEvent* btn) {
                btn->SetIDCode(kHiddenKey);
                btn->SetUserEvent(""sv);
                seq.push_back(btn);
            };
            // (double tap / hold of mod keys: shown to every reader)
            const auto modButton = [&](std::uint32_t to, bool down, float secs) {
                const auto dev = CodeDevice(to) == Device::Mouse ? RE::INPUT_DEVICE::kMouse : RE::INPUT_DEVICE::kGamepad;
                if (auto* e = g_eventPool.Button(dev, CodeId(to), down ? 1.0f : 0.0f, secs, ""sv)) seq.push_back(e);
            };
            // The double tap / hold happened: its bind goes down. `btn`: the event of this frame
            // (the second tap), none when a hold fires on time alone.
            const auto fire = [&](TriggerPress& p, const TriggerBind& bind, RE::ButtonEvent* btn) {
                p.stage      = TriggerPress::Stage::Fired;
                p.fired      = bind;
                p.heldOffset = btn ? 0.0f : static_cast<float>(nowMs - p.since) / 1000.0f;
                p.since      = nowMs;
                relinked     = true;
                if (bind.to == kUnbound) {  // a Skyrim control: the press carries its event
                    if (btn) {
                        btn->SetUserEvent(bind.event);
                        seq.push_back(btn);
                    } else if (auto* e = g_eventPool.Button(p.device, p.id, 1.0f, 0.0f, bind.event)) {
                        seq.push_back(e);
                    }
                    return;
                }
                if (btn) hideButton(btn);  // a mod key: the mod sees its own key
                if (CodeDevice(bind.to) == Device::Keyboard)
                    keyDown(bind.to, nullptr);
                else
                    modButton(bind.to, true, 0.0f);
            };
            // An event of an input whose double tap / hold fired, until it is let go.
            const auto firedEvent = [&](TriggerPress& p, RE::ButtonEvent* btn) {
                const bool up = !btn->IsPressed();
                if (p.fired.to == kUnbound) {
                    auto& data        = btn->GetRuntimeData();  // held from when the hold fired
                    data.heldDownSecs = (std::max)(0.001f, data.heldDownSecs - p.heldOffset);
                    btn->SetUserEvent(p.fired.event);
                    seq.push_back(btn);
                    return;
                }
                hideButton(btn);
                if (!up) return;
                relinked = true;
                if (CodeDevice(p.fired.to) == Device::Keyboard)
                    keyUp(p.fired.to, nullptr);
                else
                    modButton(p.fired.to, false, (std::max)(0.001f, static_cast<float>(nowMs - p.since) / 1000.0f));
            };

            // releases of taps replayed last frame, then what time alone decides
            for (const auto& r : std::exchange(g_replayUps, {}))
                if (auto* e = g_eventPool.Button(r.device, r.id, 0.0f, 0.05f, r.event)) {
                    input.push_back(e);
                    relinked = true;
                }
            for (auto it = g_presses.begin(); it != g_presses.end();) {
                auto&      p   = *it;
                const auto age = nowMs - p.since;
                if (p.stage == TriggerPress::Stage::Down && p.hold && age >= kHoldMs) {
                    fire(p, *p.hold, nullptr);
                } else if (p.stage == TriggerPress::Stage::Down && !p.hold && age >= kDoubleTapMs) {
                    replay(p, true);  // too long for a tap: an ordinary press from here on
                    p.stage = TriggerPress::Stage::Passing;
                } else if (p.stage == TriggerPress::Stage::Released && age >= kDoubleTapMs) {
                    replay(p, true);  // no second tap: a plain tap
                    g_replayUps.push_back({ p.device, p.id, p.event });
                    it = g_presses.erase(it);
                    continue;
                }
                ++it;
            }

            // Takes `e` if it belongs to a double tap / hold (hidden, rewritten or held back).
            const auto takeTrigger = [&](RE::InputEvent* e) -> bool {
                if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) return false;
                const auto dev = e->GetDevice();
                if (dev != RE::INPUT_DEVICE::kKeyboard && dev != RE::INPUT_DEVICE::kMouse && dev != RE::INPUT_DEVICE::kGamepad) return false;
                const auto d    = dev == RE::INPUT_DEVICE::kKeyboard ? Device::Keyboard : dev == RE::INPUT_DEVICE::kMouse ? Device::Mouse : Device::Gamepad;
                auto*      btn  = e->AsButtonEvent();
                const auto id   = btn->GetIDCode();
                const auto phys = MakeCode(d, id);
                if (IsWheel(phys)) return false;

                if (const auto it = std::ranges::find(g_presses, phys, &TriggerPress::phys); it != g_presses.end()) {
                    auto&      p  = *it;
                    const bool up = !btn->IsPressed();
                    switch (p.stage) {
                    case TriggerPress::Stage::Down:
                        if (!up) {
                            hideButton(btn);
                            return true;
                        }
                        if (p.dbl) {  // first tap: wait for the second
                            p.stage = TriggerPress::Stage::Released;
                            p.since = nowMs;
                            hideButton(btn);
                            return true;
                        }
                        replay(p, true);  // a plain tap: its press, then this release
                        g_presses.erase(it);
                        return false;
                    case TriggerPress::Stage::Released:
                        if (!btn->IsDown()) {
                            hideButton(btn);
                            return true;
                        }
                        fire(p, *p.dbl, btn);  // the second tap
                        return true;
                    case TriggerPress::Stage::Fired:
                        firedEvent(p, btn);
                        if (up) g_presses.erase(it);
                        return true;
                    case TriggerPress::Stage::Passing:
                        if (up) g_presses.erase(it);
                        return false;
                    }
                }

                if (!btn->IsDown() || g_triggers.empty() || g_triggersPaused) return false;
                // the binds this press may turn into: the most specific one of each kind
                const auto base = d == Device::Keyboard ? Combo(id, static_cast<std::uint8_t>(held & ~ModBitForDik(id))) : phys;
                if (ctx < 0) ctx = ActiveContext();
                std::optional<bool>        typing;
                std::optional<TriggerBind> dbl, hold;
                for (const auto& t : g_triggers) {
                    if (BaseCode(t.code) != base || !HoldOk(t.code)) continue;
                    if (t.ctx >= 0 ? t.ctx != ctx : (typing ? *typing : *(typing = Typing()))) continue;
                    auto& slot = t.trigger == Trigger::DoubleTap ? dbl : hold;
                    if (!slot || (HoldOf(t.code) && !HoldOf(slot->code))) slot = t;
                }
                if (!dbl && !hold) return false;
                g_presses.push_back({ TriggerPress::Stage::Down, phys, dev, id, btn->QUserEvent(), nowMs, dbl, hold });
                hideButton(btn);
                relinked = true;
                return true;
            };

            for (auto* e = head; e; e = e->next) {
                restore.emplace_back(e, e->next);
                // A bind is being captured: what is pressed goes to the capture and reaches no one
                // else, so the key picked doesn't also open some mod's menu (see InputBlock).
                // Keyboard and the middle / side mouse buttons and wheel are handed over; gamepad
                // buttons only hidden (the capture reads XInput); left / right click the menu.
                if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
                    auto*      btn = e->AsButtonEvent();
                    const auto dev = e->GetDevice();
                    const auto id  = btn->GetIDCode();
                    if (dev == RE::INPUT_DEVICE::kGamepad && g_triggersPaused) {
                        hideButton(btn);
                        continue;
                    }
                    const bool kb    = dev == RE::INPUT_DEVICE::kKeyboard;
                    const bool mouse = dev == RE::INPUT_DEVICE::kMouse && id >= kMouseMiddle && id != kMouseMove;
                    if ((kb || mouse) && InputBlockBusy() &&
                        BlockInputEvent(kb ? Device::Keyboard : Device::Mouse, id, btn->IsDown(), !btn->IsPressed(), !IsWheel(MakeCode(Device::Mouse, id)) || kb)) {
                        hideButton(btn);
                        continue;
                    }
                }
                if (!takeTrigger(e)) input.push_back(e);
            }

            for (auto* e : input) {
                // mouse / gamepad buttons:
                //  - a mod's button moved to another button of the same device is shown to it as
                //    its own button, its old button is hidden;
                //  - a mod's key moved to a mouse button: the button is hidden and the key pressed;
                //  - a Skyrim control moved from a key to a mouse button gets the button's press.
                if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton &&
                    (e->GetDevice() == RE::INPUT_DEVICE::kMouse || e->GetDevice() == RE::INPUT_DEVICE::kGamepad)) {
                    auto*      btn  = e->AsButtonEvent();
                    const auto code = MakeCode(e->GetDevice() == RE::INPUT_DEVICE::kMouse ? Device::Mouse : Device::Gamepad, btn->GetIDCode());
                    const auto hide = [&] {
                        btn->SetIDCode(kHiddenKey);
                        btn->SetUserEvent(""sv);
                    };
                    const auto show = [&](std::uint32_t to) {
                        btn->SetIDCode(to == kHiddenKey ? kHiddenKey : CodeId(to));
                        if (to == kHiddenKey) return;
                        btn->SetUserEvent(""sv);  // the button now belongs to the mod action
                        // a gamepad button added to a mod's mouse button is shown as that mouse button
                        btn->device = CodeDevice(to) == Device::Mouse ? RE::INPUT_DEVICE::kMouse : RE::INPUT_DEVICE::kGamepad;
                    };
                    seq.push_back(e);  // synthetic keys go after it

                    if (const auto it = g_activeRemaps.find(code); it != g_activeRemaps.end()) {
                        const auto [to, r]  = it->second;
                        const bool released = !btn->IsPressed();
                        edit(btn, r);
                        if (to != kHiddenKey && CodeDevice(to) == Device::Keyboard) {
                            hide();
                            if (released) keyUp(to, r);
                        } else {
                            show(to);
                        }
                        if (released) g_activeRemaps.erase(code);
                        continue;
                    }
                    if (const auto it = g_activeButtons.find(code); it != g_activeButtons.end()) {
                        btn->SetUserEvent(it->second);
                        if (!btn->IsPressed()) g_activeButtons.erase(it);
                        continue;
                    }
                    if (!btn->IsDown()) continue;

                    if (const auto* m = BestMatch(g_remaps, code, [](const Remap& r) { return r.from; })) {
                        const auto r = Scoped(m->readers);
                        edit(btn, r);
                        if (CodeDevice(m->to) == Device::Keyboard) {
                            hide();
                            keyDown(m->to, r);
                            if (IsWheel(code))
                                g_pendingUp.push_back({ m->to, 3, r });
                            else
                                g_activeRemaps[code] = { m->to, r };
                        } else {
                            show(m->to);
                            if (!IsWheel(code)) g_activeRemaps[code] = { m->to, r };
                        }
                        continue;
                    }
                    if (!g_buttonBinds.empty()) {
                        if (ctx < 0) ctx = ActiveContext();
                        if (const auto* b = BestMatch(g_buttonBinds, code, [](const ButtonBind& b) { return b.code; }, ctx)) {
                            btn->SetUserEvent(b->event);
                            if (!IsWheel(code)) g_activeButtons[code] = b->event;
                            continue;
                        }
                    }
                    if (const auto b = std::ranges::find(g_blocked, code, &Blocked::code); b != g_blocked.end()) {
                        const auto r = Scoped(b->readers);
                        edit(btn, r);
                        show(kHiddenKey);
                        if (!IsWheel(code)) g_activeRemaps[code] = { kHiddenKey, r };
                    }
                    continue;
                }
                if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || e->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                    seq.push_back(e);
                    continue;
                }
                auto*      btn = e->AsButtonEvent();
                const auto key = btn->GetIDCode();

                // Show the mods `combo` instead of this key. Modifiers change like a real key
                // press: switched to the combo's before the key goes down, switched back after
                // it comes up, untouched while it is held. Doing it every frame floods ImGui
                // based mods (OAR) with modifier events: lag and a stuck queue.
                // (the key's Skyrim action is dropped for everyone: the key now belongs to the mod)
                const auto present = [&](std::uint32_t combo, const Readers& r) {
                    edit(btn, r);
                    btn->SetIDCode(ComboKey(combo));
                    btn->SetUserEvent(""sv);  // the key now belongs to the mod action
                    std::vector<RE::InputEvent*> before, after;
                    if (btn->IsDown()) {
                        modifiers(held, ComboMods(combo), r, before);
                        if (!before.empty()) {
                            // ImGui applies modifier events at the start of the next frame (OAR
                            // closes its menu on io.KeyShift): modifiers this frame, key next frame
                            btn->SetIDCode(kHiddenKey);
                            g_pendingDown.emplace_back(ComboKey(combo), r);
                        }
                    } else if (!btn->IsPressed()) {
                        modifiers(ComboMods(combo), held, r, after);
                    }
                    relinked |= !before.empty() || !after.empty();
                    seq.insert(seq.end(), before.begin(), before.end());
                    seq.push_back(e);
                    seq.insert(seq.end(), after.begin(), after.end());
                };

                // a press already being translated: keep it consistent until release
                if (const auto it = g_activeRemaps.find(key); it != g_activeRemaps.end()) {
                    const auto [combo, r] = it->second;
                    if (!btn->IsPressed()) g_activeRemaps.erase(it);
                    if (combo == kHiddenKey) {
                        edit(btn, r);
                        btn->SetIDCode(kHiddenKey);
                        seq.push_back(e);
                    } else {
                        present(combo, r);
                    }
                    continue;
                }
                // While mods are shown a combo, a physical modifier stays hidden from them: Skyrim
                // repeats a held key's event every frame, and each repeat would undo the faked
                // modifiers (OAR toggles its menu only on the exact Ctrl / Shift / Alt state).
                // Its Skyrim action stays; on release the modifiers go back to the real ones.
                if (ModBitForDik(key)) {
                    if (const auto r = PresentingCombo()) {
                        edit(btn, *r);
                        btn->SetIDCode(kHiddenKey);
                        seq.push_back(e);
                        continue;
                    }
                }
                if (const auto it = g_activeCombos.find(key); it != g_activeCombos.end()) {
                    btn->SetUserEvent(it->second);
                    if (!btn->IsPressed()) g_activeCombos.erase(it);
                    seq.push_back(e);
                    continue;
                }
                if (!btn->IsDown()) {
                    seq.push_back(e);
                    continue;
                }

                // a modifier pressed as the key itself doesn't count as its own modifier
                const auto pressed = Combo(key, static_cast<std::uint8_t>(held & ~ModBitForDik(key)));
                // A remap shown to every reader would also change what is typed (a moved
                // Backspace stopped erasing): while a text field or the console has the
                // keyboard, such remaps and blocks leave the keys alone.
                if (!typingNow) typingNow = Typing();
                if (const auto* m = BestMatch(g_remaps, pressed, [](const Remap& r) { return r.from; }); m && (Scoped(m->readers) || !*typingNow)) {
                    const auto r        = Scoped(m->readers);
                    g_activeRemaps[key] = { m->to, r };
                    present(m->to, r);
                    continue;
                }
                if (!g_combos.empty()) {
                    if (ctx < 0) ctx = ActiveContext();
                    const auto* c = BestMatch(g_combos, pressed, [](const ComboBind& c) { return WithHold(Combo(c.key, c.mods), c.hold); }, ctx);
                    if (c) {
                        btn->SetUserEvent(c->event);
                        g_activeCombos[key] = c->event;
                        seq.push_back(e);
                        continue;
                    }
                }
                if (const auto b = std::ranges::find(g_blocked, pressed, &Blocked::code); b != g_blocked.end() && (Scoped(b->readers) || !*typingNow)) {
                    const auto r = Scoped(b->readers);
                    edit(btn, r);
                    btn->SetIDCode(kHiddenKey);
                    g_activeRemaps[key] = { kHiddenKey, r };
                }
                seq.push_back(e);
            }

            if (!relinked || seq.empty()) {
                if (g_split.Empty()) restore.clear();  // same list, only fields changed (a split relinks it)
                return head;
            }
            for (std::size_t i = 0; i < seq.size(); ++i) seq[i]->next = i + 1 < seq.size() ? seq[i + 1] : nullptr;
            return seq.front();
        }

        // Debug: gamepad presses and releases as they come in and as they go out to the sinks.
        constexpr bool kLogPad = false;

        void LogPadEvents(const char* stage, RE::InputEvent* head)
        {
            for (auto* e = head; e; e = e->next) {
                if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || e->GetDevice() != RE::INPUT_DEVICE::kGamepad) continue;
                auto* btn = e->AsButtonEvent();
                if (!btn->IsDown() && btn->IsPressed()) continue;  // repeats while held
                logger::info("pad {}: id 0x{:04X} {} event '{}' value {:.2f} held {:.3f} ctx {}", stage, btn->GetIDCode(),
                    btn->IsDown() ? "down" : "up", btn->QUserEvent().c_str(), btn->Value(), btn->HeldDuration(), ActiveContext());
            }
        }

        // Other mods (OAR, ...) hook the same call to read input. Whoever hooks last runs first,
        // so we re-hook on top of them after they load. Each layer needs its own thunk and
        // saved original; only the outermost one processes, inner ones just pass through.
        bool g_inDispatch = false;  // game thread only
    }

    template <int N>
    struct InputHook
    {
        static void Thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent* const* a_events)
        {
            if (g_inDispatch || !a_events) return func(a_source, a_events);
            g_inDispatch = true;
            g_split.Clear();
            RefreshSinks(a_source);
            std::vector<std::pair<RE::InputEvent*, RE::InputEvent*>> restore;
            if (kLogPad) LogPadEvents("in ", *a_events);
            RE::InputEvent*                                         head = ProcessInput(*a_events, restore);
            if (g_split.Empty()) {
                if (kLogPad) LogPadEvents("out", head);
                func(a_source, &head);
            } else {
                // `head` is the reader dlls' view; everyone else gets the real keys (see Split)
                for (auto* e = head; e; e = e->next) g_split.order.push_back(e);
                for (auto& x : g_split.edits) {
                    x.id[1]  = x.e->GetIDCode();
                    x.dev[1] = x.e->GetDevice();
                }
                RE::InputEvent* everyone = g_split.View(nullptr);
                g_split.everyone         = everyone;
                g_split.on               = true;
                if (kLogPad) LogPadEvents("out", everyone);
                func(a_source, &everyone);
                g_split.on = false;
            }
            for (auto& [e, next] : restore) e->next = next;  // hand the game its own list back
            g_inDispatch = false;
        }
        static inline REL::Relocation<decltype(Thunk)> func;
    };

    namespace
    {
        std::atomic<bool> g_hookInstalled{ false };
        std::uintptr_t    g_hookSite   = 0;
        std::uintptr_t    g_ourTarget  = 0;  // call target we wrote most recently
        int               g_hookLayers = 0;

        std::uintptr_t CallTarget(std::uintptr_t site)
        {
            return site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1);
        }

        // The DLL a call target leads to, through an SKSE trampoline jump (jmp [rip+disp]).
        std::string TargetOwner(std::uintptr_t target)
        {
            const auto* code = reinterpret_cast<const std::uint8_t*>(target);
            if (code[0] == 0xFF && code[1] == 0x25)
                target = *reinterpret_cast<const std::uintptr_t*>(target + 6 + *reinterpret_cast<const std::int32_t*>(target + 2));
            HMODULE mod = nullptr;
            wchar_t name[MAX_PATH]{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(target), &mod) ||
                !GetModuleFileNameW(mod, name, MAX_PATH))
                return "unknown";
            return Utf8(fs::path(name).filename());
        }

        template <int N>
        void WriteLayer()
        {
            InputHook<N>::func = SKSE::GetTrampoline().write_call<5>(g_hookSite, InputHook<N>::Thunk);
        }

        bool AddHookLayer()
        {
            switch (g_hookLayers) {
            case 0: WriteLayer<0>(); break;
            case 1: WriteLayer<1>(); break;
            case 2: WriteLayer<2>(); break;
            case 3: WriteLayer<3>(); break;
            case 4: WriteLayer<4>(); break;
            case 5: WriteLayer<5>(); break;
            case 6: WriteLayer<6>(); break;
            case 7: WriteLayer<7>(); break;
            default: return false;
            }
            ++g_hookLayers;
            g_ourTarget = CallTarget(g_hookSite);
            return true;
        }
    }

    bool InputHookInstalled() { return g_hookInstalled.load(); }

    // Hooks the call that hands each frame's input events to their sinks
    // (BSInputDeviceManager poll -> BSTEventSource<InputEvent*>::SendEvent).
    void InstallInputHook()
    {
        REL::Relocation<std::uintptr_t> func{ RELOCATION_ID(67315, 68617) };
        const auto                      site = func.address() + REL::VariantOffset(0x7B, 0x7B, 0x81).offset();
        // must be a rel32 call; anything else means this game version differs: don't patch
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
            logger::error("input hook: unexpected code at {:X}, combos and mod remaps are disabled", site);
            return;
        }
        g_hookSite = site;
        AddHookLayer();
        g_hookInstalled = true;
        logger::info("input hook installed");
    }

    // Game thread. If a mod loaded after us hooked the same call, it now sees input before
    // we translate it: wrap it once more so we are outermost again.
    void EnsureInputHookOnTop()
    {
        if (!g_hookInstalled) return;
        const auto target = CallTarget(g_hookSite);
        if (target == g_ourTarget) return;
        const auto owner = TargetOwner(target);
        if (AddHookLayer())
            logger::info("input hook: {} hooked input after us, re-hooked on top (layer {})", owner, g_hookLayers);
        else
            logger::warn("input hook: {} hooked input after us, still not first after {} layers, remaps may not reach it", owner, kInputHookLayers);
    }

    // hook state of inputs held under the old tables. Game thread.
    void ClearActiveInputs()
    {
        g_activeCombos.clear();
        g_activeRemaps.clear();
        g_activeButtons.clear();
        g_presses.clear();
        g_replayUps.clear();
    }
}
