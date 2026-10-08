#pragma once

// Hotkey Atlas: the bind model and the API the menu pages use.

namespace HA
{
    enum class Kind : std::uint8_t
    {
        ControlMap,  // vanilla Skyrim controls, read live from RE::ControlMap
        Ini,         // a key value found in a mod's .ini file
        Json,        // a key value found in a mod's .json settings file
        Yaml         // a key in a mod's YAML hotkey file (SkyrimNet): rebinding writes the file
    };

    // Modifier bits. A "combo" packs a key and its modifiers: key | mods << 8.
    enum Mod : std::uint8_t
    {
        kShift = 1,
        kCtrl  = 2,
        kAlt   = 4
    };

    constexpr std::uint32_t Combo(std::uint32_t key, std::uint8_t mods) { return (key & 0xFF) | (static_cast<std::uint32_t>(mods & 7) << 8); }
    constexpr std::uint32_t ComboKey(std::uint32_t combo) { return combo & 0xFF; }
    constexpr std::uint8_t  ComboMods(std::uint32_t combo) { return static_cast<std::uint8_t>((combo >> 8) & 7); }

    // Combo of a binding the user removed: no key at all. Same value the game uses for an
    // unbound control, so it never matches a real key press.
    constexpr std::uint32_t kUnbound = 0xFF;

    // Per modifier bit: the bit, the left-hand key used for it, its name.
    constexpr std::uint8_t  kModBits[3]  = { kShift, kCtrl, kAlt };
    constexpr std::uint32_t kModDik[3]   = { 42, 29, 56 };  // LShift, LCtrl, LAlt
    constexpr const char*   kModNames[3] = { "Shift", "Ctrl", "Alt" };

    // How a setting stores its modifiers.
    enum class ModStyle : std::uint8_t
    {
        None,      // plain key, no modifier support
        Flags,     // sibling bool settings: uToggleUIKey + uToggleUIKeyShift/Ctrl/Alt (OAR)
        ModKey,    // sibling setting holding the modifier's key code: iHotkey + iHotkeyModifier
        JsonArray  // json array of keys: [16, 106] = Shift + Num* (Community Shaders)
    };

    enum class Device : std::uint8_t
    {
        Keyboard,
        Mouse,
        Gamepad
    };

    // Button ids for Device::Mouse and Device::Gamepad, the same values Skyrim's control map
    // uses. Mods store them as SKSE codes (256+ mouse, 266+ gamepad), converted on scan.
    enum MouseButton : std::uint32_t
    {
        kMouseLeft,
        kMouseRight,
        kMouseMiddle,
        kMouse4,
        kMouse5,
        kMouse6,
        kMouse7,
        kMouse8,
        kMouseWheelUp,
        kMouseWheelDown,
        kMouseMove
    };

    enum GamepadButton : std::uint32_t
    {
        kPadUp        = 0x0001,
        kPadDown      = 0x0002,
        kPadLeft      = 0x0004,
        kPadRight     = 0x0008,
        kPadStart     = 0x0010,
        kPadBack      = 0x0020,
        kPadL3        = 0x0040,
        kPadR3        = 0x0080,
        kPadLB        = 0x0100,
        kPadRB        = 0x0200,
        kPadA         = 0x1000,
        kPadB         = 0x2000,
        kPadX         = 0x4000,
        kPadY         = 0x8000,
        kPadLT        = 0x0009,
        kPadRT        = 0x000A,
        kPadLeftStick = 0x000B,
        kPadRightStick = 0x000C
    };

    // An input "code" names a key or button on any device, as stored in overrides and remaps:
    // keyboard combos stay below 0x800 (key | mods << 8), mouse is 0x10000 | MouseButton,
    // gamepad 0x20000 | GamepadButton. kUnbound (0xFF) means no input at all.
    // A combo of any two inputs ("hold G, press F") also packs the held one in bits 20-29,
    // see WithHold; keyboard modifiers stay in the key's mods (mods' own files store those).
    // Bits 30-31 say how the input is used: pressed, tapped twice or held, see Trigger.
    constexpr std::uint32_t kMouseCode = 0x10000, kPadCode = 0x20000;

    enum class Trigger : std::uint8_t
    {
        Press,      // a plain press
        DoubleTap,  // pressed twice in quick succession
        Hold        // held down for a moment
    };

    constexpr Trigger TriggerOf(std::uint32_t code) { return code == kUnbound ? Trigger::Press : static_cast<Trigger>((code >> 30) & 3); }
    constexpr std::uint32_t WithTrigger(std::uint32_t code, Trigger t)
    {
        return code == kUnbound ? code : (code & 0x3FFFFFFF) | static_cast<std::uint32_t>(t) << 30;
    }

    constexpr std::uint32_t MakeCode(Device d, std::uint32_t id)
    {
        return d == Device::Mouse ? kMouseCode | id : d == Device::Gamepad ? kPadCode | id : id;
    }
    constexpr std::uint32_t BaseCode(std::uint32_t code) { return code & 0xFFFFF; }  // without the held input
    constexpr Device        CodeDevice(std::uint32_t code)
    {
        code = BaseCode(code);
        return code >= kPadCode ? Device::Gamepad : code >= kMouseCode ? Device::Mouse : Device::Keyboard;
    }
    constexpr std::uint32_t CodeId(std::uint32_t code) { return code & 0xFFFF; }
    constexpr bool          IsStick(std::uint32_t code) { return code == MakeCode(Device::Gamepad, kPadLeftStick) || code == MakeCode(Device::Gamepad, kPadRightStick); }

    // Gamepad buttons in a fixed order: a held pad button is stored by its index here.
    constexpr std::uint32_t kPadIds[] = { kPadUp, kPadDown, kPadLeft, kPadRight, kPadStart, kPadBack, kPadL3, kPadR3, kPadLB, kPadRB,
        kPadA, kPadB, kPadX, kPadY, kPadLT, kPadRT, kPadLeftStick, kPadRightStick };

    // Held input of a combo: 0 = none, else its code (no mods, no hold of its own).
    constexpr std::uint32_t HoldOf(std::uint32_t code)
    {
        if (code == kUnbound) return 0;
        const auto h = (code >> 20) & 0x3FF, idx = h & 0xFF;
        switch (h >> 8) {
        case 1: return idx;                                        // keyboard DIK
        case 2: return MakeCode(Device::Mouse, idx);               // MouseButton
        case 3: return idx < std::size(kPadIds) ? MakeCode(Device::Gamepad, kPadIds[idx]) : 0;
        default: return 0;
        }
    }

    // `code` pressed while `hold` is held; hold 0 = a plain `code`. Its Trigger stays.
    constexpr std::uint32_t WithHold(std::uint32_t code, std::uint32_t hold)
    {
        code = code == kUnbound ? code : BaseCode(code) | (code & 0xC0000000);
        if (!hold) return code;
        std::uint32_t h = 0;
        switch (CodeDevice(hold)) {
        case Device::Keyboard: h = 1u << 8 | (hold & 0xFF); break;
        case Device::Mouse: h = 2u << 8 | (CodeId(hold) & 0xFF); break;
        case Device::Gamepad:
            for (std::uint32_t i = 0; i < std::size(kPadIds); ++i)
                if (kPadIds[i] == CodeId(hold)) h = 3u << 8 | i;
            break;
        }
        return h ? code | h << 20 : code;
    }

    struct Binding
    {
        Device        device = Device::Keyboard;
        std::uint32_t key  = 0;  // keyboard: DirectInput scancode; mouse / gamepad: MouseButton / GamepadButton
        std::uint8_t  mods = 0;  // Mod bits that must be held with `key`
        std::uint32_t hold = 0;  // input code held before `key` is pressed (see HoldOf), 0 = none
        Trigger       trigger = Trigger::Press;  // double tap / hold instead of a plain press
        std::string   action;    // human readable action name
        std::string   owner;    // "Skyrim", "Skyrim - Creation Club", "Skyrim - Debug" (see VanillaOwner) or the mod that owns the setting
        std::string   context;  // input context (ControlMap) or ini section
        std::string   origin;   // "ControlMap" or path relative to Data/
        std::string   description;  // built-in explanation, from Notes.json
        std::string   contextHint;  // where the context is active (Skyrim controls only)
        Kind          kind     = Kind::ControlMap;
        bool          editable = false;
        bool          ownInput = false;  // the mod reads the keyboard itself (not the game's input): read-only

        // handle for Kind::ControlMap
        int           ctx        = -1;
        int           index      = -1;
        // set when Hotkey Atlas changed this binding (recorded in HotkeyAtlas.ini)
        bool          overridden = false;
        std::uint32_t defaultKey = 0;  // input code (see MakeCode) before our first change
        // key or mouse binding: gamepad code (see MakeCode) of a button or stick that also fires
        // it, the key stays; kUnbound = none
        std::uint32_t padKey = kUnbound;

        // handle for Kind::Ini / Kind::Json / Kind::Yaml
        fs::path    file;
        std::string iniKey;  // ini / yaml: setting name; json: full path, e.g. "Menu.ToggleKey"
        int         line = -1;
        // dlls that read this mod key from the game's input: a remap is shown to them only, other
        // mods keep seeing the real keys. Empty = not known: the remap is shown to every reader.
        std::vector<fs::path> readers;

        // modifier storage (Kind::Ini / Kind::Json)
        ModStyle modStyle = ModStyle::None;
        struct IniRef
        {
            int         line = -1;
            std::string name;
        };
        IniRef flags[3];  // ModStyle::Flags: Shift, Ctrl, Alt settings (line -1 = absent)
        IniRef modKey;    // ModStyle::ModKey

        // handle for Kind::Json
        std::size_t   valueOffset = 0;  // byte range of the number in the file
        std::size_t   valueLength = 0;
        std::uint32_t fileValue   = 0;      // number as written in the file
        bool          virtualKey  = false;  // file stores Windows VK codes instead of DIK
        std::size_t   arrayOffset = 0;      // ModStyle::JsonArray: byte range of the whole array
        std::string   arrayText;
    };

    // The input code a binding has now.
    inline std::uint32_t CurrentCode(const Binding& b)
    {
        if (b.key == kUnbound) return kUnbound;
        return WithTrigger(WithHold(b.device == Device::Keyboard ? Combo(b.key, b.mods) : MakeCode(b.device, b.key), b.hold), b.trigger);
    }

    // Whether pressing `id` on `device` is part of input `code`: its key or button, its held
    // input, or (keyboard) one of its Shift / Ctrl / Alt keys, either side.
    constexpr bool CodeUses(std::uint32_t code, Device device, std::uint32_t id)
    {
        if (code == kUnbound) return false;
        if (CodeDevice(code) == device && (device == Device::Keyboard ? ComboKey(code) : CodeId(code)) == id) return true;
        if (const auto h = HoldOf(code); h && CodeDevice(h) == device && CodeId(h) == id) return true;
        if (device != Device::Keyboard || CodeDevice(code) != Device::Keyboard) return false;
        const auto mods = ComboMods(code);
        return ((mods & kShift) && (id == 42 || id == 54)) || ((mods & kCtrl) && (id == 29 || id == 157)) || ((mods & kAlt) && (id == 56 || id == 184));
    }

    // The input of `b` (its own code, or the gamepad button added to it) that `id` on `device`
    // is part of; kUnbound = none. A combo is listed on every key and button it takes.
    inline std::uint32_t CodeUsing(const Binding& b, Device device, std::uint32_t id)
    {
        if (const auto own = CurrentCode(b); CodeUses(own, device, id)) return own;
        if (CodeUses(b.padKey, device, id)) return b.padKey;
        return kUnbound;
    }

    struct Model
    {
        std::vector<Binding>                                        all;    // sorted by key, owner, action
        std::unordered_map<std::uint32_t, std::vector<std::size_t>> byKey;  // keyboard key -> indices into `all`
    };

    void                         Rescan();
    std::shared_ptr<const Model> GetModel();
    bool                         IsBusy();
    std::string                  GetStatus();
    // swap: the Skyrim controls already on that key in the same context get the key `binding`
    // leaves. Without it such a rebind waits for the user's answer (see PendingSwap).
    void                         Rebind(const Binding& binding, std::uint32_t newCombo, bool swap = false);

    // A Skyrim control put on a key another control of the same context already has.
    struct SwapRequest
    {
        Binding               binding;
        std::uint32_t         code = kUnbound;
        std::vector<Binding> taken;  // the controls on that key now
    };
    std::vector<Binding>       ControlsOnKey(const Binding& b, std::uint32_t newCombo);
    std::optional<SwapRequest> PendingSwap();
    bool                       CanSwap(const SwapRequest& r);  // the others can all take the key it leaves
    void                       AnswerSwap(bool swap);          // false: the rebind is dropped
    void                         BindGamepad(const Binding& binding, std::uint32_t padCode);  // kUnbound removes it
    void                         ResetAll(std::optional<Device> only = std::nullopt);  // only: that device's changes

    // Presets: sets of bind changes; every change goes into the active one. Names are UTF-8.
    std::vector<std::string> Presets();
    std::string              PresetName(std::string_view text);  // typed text -> usable file name, empty = none
    std::string              ActivePreset();
    bool                     NewPreset(std::string_view name, std::string& err);     // the current binds under a new name, made active
    bool                     SwitchPreset(std::string_view name, std::string& err);  // its binds replace the current ones
    bool                     DeletePreset(std::string_view name, std::string& err);  // the active one: switches to another first

    void                         LoadConfig();
    inline constexpr int         kInputHookLayers         = 8;                      // re-hooks on top of other mods (see EnsureInputHookOnTop)
    inline constexpr std::size_t kInputHookTrampolineSize = 14 * kInputHookLayers;  // one 14-byte jump per layer
    void                         InstallInputHook();  // needs kInputHookTrampolineSize from SKSE::Init
    void                         EnsureInputHookOnTop();
    bool                         InputHookInstalled();
    void                         ApplyOverridesLater();
    void                         WatchMapMenu();  // keeps SkyUI's map button hints on the current binds
    bool                         HideVanilla();
    void                         SetHideVanilla(bool hide);

    // Table columns the user can hide (Key and Edit always stay).
    struct OptionalColumn
    {
        std::uint32_t bit;
        const char*   name;
    };
    constexpr OptionalColumn kOptionalColumns[] = {
        { 1, N_("Action") }, { 2, N_("Note") }, { 4, N_("Mod") }, { 8, N_("Context") }, { 16, N_("Source") }
    };
    bool          PlayStationLabels();  // gamepad drawn with PlayStation names and symbols
    void          SetPlayStationLabels(bool ps);
    bool          ShowCrossDevice();  // key column shows a key's gamepad duplicate and the other way round
    void          SetShowCrossDevice(bool show);
    std::uint32_t HiddenColumns();  // OptionalColumn::bit mask
    void          SetHiddenColumns(std::uint32_t mask);

    // Translation. Text is looked up by its English form in
    // Data/SKSE/Plugins/HotkeyAtlas/Translations/<language>.txt; missing text stays English.
    const char*           TL(const char* en);
    const std::string&       TL(const std::string& en);
    std::string              TLF(std::string_view en, std::initializer_list<std::string_view> args);  // {0}, {1}, ...
    std::vector<std::string> Languages();       // translation files found
    std::string              Language();        // chosen in HotkeyAtlas.ini; empty = the game's language
    std::string              ActiveLanguage();  // the language in use
    void                     SetLanguage(std::string lang);
    void                     LoadTranslation();

    // lower-case mod (owner) names whose bindings are hidden
    std::shared_ptr<const std::set<std::string>> GetBlacklist();
    void                                         SetBlacklisted(const std::vector<std::string>& owners, bool hidden);

    // User notes on bindings, keyed by NoteId(). An empty text removes the note.
    using Notes = std::map<std::string, std::string>;
    std::string                  NoteId(const Binding& b);
    std::shared_ptr<const Notes> GetNotes();
    void                         SetNote(const std::string& id, std::string text);
}

namespace HA::UI
{
    void Register();  // adds the pages to SKSE Menu Framework
}
