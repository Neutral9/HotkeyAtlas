// Finds every hotkey: Skyrim controls plus the key settings in mods' ini and json files.

#include "Internal.h"
#include "Json.h"

#include <Psapi.h>

namespace HA
{
    namespace
    {
        std::mutex                   g_lock;
        std::shared_ptr<const Model> g_model = std::make_shared<const Model>();
        std::string                  g_status;
        std::atomic<bool>            g_busy{ false };
        std::atomic<bool>            g_again{ false };
    }

    void SetStatus(std::string s)
    {
        std::lock_guard l(g_lock);
        g_status = std::move(s);
    }

    namespace
    {
        std::atomic<bool> g_restoredFiles{ false };

        // Applies our remap record to a scanned mod binding. The mod's file keeps its own key
        // (`defaultKey`); the binding shows the key the user picked, which the input hook
        // translates at runtime.
        void MarkFileEdit(Binding& b)
        {
            std::optional<Override> rec;
            {
                std::lock_guard l(g_ovLock);
                if (const auto it = g_fileEdits.find(FileEditId(b)); it != g_fileEdits.end()) rec = it->second;
            }
            if (!rec) return;
            const auto inFile = CurrentCode(b);

            if (b.kind == Kind::Yaml) {
                // we wrote the key into the file: it is there unless changed since (SkyrimNet's
                // own dashboard), then the record no longer applies
                if (rec->key != inFile) return;
                b.overridden = true;
                b.defaultKey = rec->original;
                return;
            }
            // only ini / json files were ever written (not OMO's KeyConfiguration.txt)
            const bool writable = b.origin.ends_with(".ini") || b.origin.ends_with(".json");
            if (writable && b.device == Device::Keyboard && rec->key == inFile && rec->original != inFile) {
                // written into the mod's file by an older Hotkey Atlas: put the file back
                std::string err;
                if (!RestoreFileValue(b, rec->original, err)) {
                    logger::warn("could not restore {} {}: {}", b.origin, b.iniKey, err);
                    return;
                }
                logger::info("restored {} {} to its original key", b.origin, b.iniKey);
                g_restoredFiles = true;  // other offsets in this file are stale now: scan again
            } else if (rec->original != inFile) {
                return;  // the mod's own key changed since: this remap no longer applies
            }
            b.overridden = true;
            b.defaultKey = rec->original;
            if (rec->key == kUnbound) {
                b.key  = kUnbound;
                b.mods = 0;
            } else if (CodeDevice(rec->key) == Device::Keyboard) {
                b.key  = ComboKey(rec->key);
                b.mods = ComboMods(rec->key);
            } else {
                b.device = CodeDevice(rec->key);  // a key moved to the mouse is shown there
                b.key    = CodeId(rec->key);
                b.mods   = 0;
            }
            b.hold    = HoldOf(rec->key);
            b.trigger = TriggerOf(rec->key);
        }

        // ---------------------------------------------------------------- ini scanning

        // Path relative to Data/. Lexical on purpose: fs::relative resolves through MO2's
        // virtual file system to the real mod folder and yields "../../MO2/mods/...".
        fs::path RelToData(const fs::path& file, const fs::path& dataDir)
        {
            auto rel = file.lexically_relative(dataDir);
            if (rel.empty() || *rel.begin() == "..") return file.filename();
            return rel;
        }

        std::string OwnerFromPath(const fs::path& file, const fs::path& dataDir)
        {
            const auto               rel = RelToData(file, dataDir);
            std::vector<std::string> parts;
            for (const auto& p : rel) parts.push_back(p.string());
            // SKSE/Plugins/<Mod>/something.ini -> <Mod>;  SKSE/Plugins/<Mod>.ini and MCM/Settings/<Mod>.ini -> stem
            if (parts.size() > 3 && Lower(parts[0]) == "skse" && Lower(parts[1]) == "plugins") return parts[2];
            return file.stem().string();
        }

        // ---------------------------------------------------------------- which mods are running

        // "OpenAnimationReplacer_ImGui" -> "openanimationreplacerimgui": names compared loosely
        std::string Norm(std::string_view s)
        {
            std::string out;
            for (const char c : s)
                if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        // The SKSE plugins (dll) and game plugins (esp / esm / esl) actually loaded, by Norm()
        // name. A mod switched off in the mod manager can leave its config visible (kept in
        // another MO2 folder or in Overwrite); its hotkeys do nothing and are left out.
        struct LoadedMods
        {
            std::vector<std::string> dlls, plugins;
            std::vector<fs::path>    dllPaths;      // same order as dlls
            std::set<std::string>    pluginsExact;  // lower case, with extension
            fs::path                 skse;          // skse64_1_x_y.dll

            // The loaded dll installed in the same mod manager folder as `file` (MO2: mods\<Mod>\...),
            // for configs named unlike their dll (SKSE/Plugins/IED/... of ImmersiveEquipmentDisplays.dll).
            std::optional<fs::path> DllInSameMod(const fs::path& file) const
            {
                // "...\mods\<Mod>\" in lower case, without the "\\?\" prefix; empty outside a mods folder
                const auto root = [](const std::wstring& real) -> std::wstring {
                    auto l = real.starts_with(L"\\\\?\\") ? real.substr(4) : real;
                    std::ranges::transform(l, l.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    const auto p   = l.find(L"\\mods\\");
                    if (p == std::wstring::npos) return {};
                    const auto end = l.find(L'\\', p + 6);
                    return end == std::wstring::npos ? std::wstring() : l.substr(0, end + 1);
                };
                HANDLE h = CreateFileW(file.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (h == INVALID_HANDLE_VALUE) return std::nullopt;
                wchar_t    buf[MAX_PATH * 2];
                const auto len = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)), FILE_NAME_NORMALIZED);
                CloseHandle(h);
                if (!len || len >= std::size(buf)) return std::nullopt;
                const auto mod = root(std::wstring(buf, len));
                if (mod.empty()) return std::nullopt;
                for (const auto& dll : dllPaths)
                    if (root(dll.wstring()) == mod) return dll;
                return std::nullopt;
            }

            // Who reads the keys of a mod with Papyrus scripts: SKSE (RegisterForKey), plus the
            // mod's own dll if it has one (its script may pass the key on to it).
            std::vector<fs::path> PapyrusReaders(std::initializer_list<std::string_view> names) const
            {
                std::vector<fs::path> out;
                if (!skse.empty()) out.push_back(skse);
                if (const auto dll = DllFor(names)) out.push_back(*dll);
                return out;
            }

            bool Known() const { return !dlls.empty() && !plugins.empty(); }

            // Whether one of `names` (a config's folder or file name) is a loaded dll or plugin: the
            // same name, or starting with it ("OpenAnimationReplacer_ImGui.ini" belongs to
            // OpenAnimationReplacer.dll). Not the other way round: OpenAnimationReplacer-RaySense.dll
            // being loaded says nothing about OpenAnimationReplacer.ini.
            bool AnyLoaded(std::initializer_list<std::string_view> names) const
            {
                for (const auto raw : names) {
                    const auto n = Norm(raw);
                    if (n.size() < 3) continue;
                    for (const auto* list : { &dlls, &plugins })
                        for (const auto& l : *list)
                            if (n == l || (l.size() >= 5 && n.starts_with(l))) return true;
                }
                return false;
            }

            // The loaded dll a config belongs to (see AnyLoaded), if any.
            std::optional<fs::path> DllFor(std::initializer_list<std::string_view> names) const
            {
                for (const auto raw : names) {
                    const auto n = Norm(raw);
                    if (n.size() < 3) continue;
                    for (std::size_t i = 0; i < dlls.size(); ++i)
                        if (n == dlls[i] || (dlls[i].size() >= 5 && n.starts_with(dlls[i]))) return dllPaths[i];
                }
                return std::nullopt;
            }

            bool PluginLoaded(const std::string& stem) const
            {
                const auto l = Lower(stem);
                return pluginsExact.contains(l + ".esp") || pluginsExact.contains(l + ".esm") || pluginsExact.contains(l + ".esl");
            }
        };

        // Whether a dll asks Windows for key states (GetAsyncKeyState / GetKeyState). Alone that
        // says little (many mods check a modifier that way and still read the game's input), so
        // it only counts for keys their config writes as Windows key names (VK_F2), see markOwnInput.
        bool PollsWindowsKeys(const fs::path& dll)
        {
            static std::mutex                       lock;
            static std::map<fs::path, bool>         cache;
            std::lock_guard                         l(lock);
            if (const auto it = cache.find(dll); it != cache.end()) return it->second;
            bool          result = false;
            std::ifstream in(dll, std::ios::binary);
            if (in) {
                const std::string bytes(std::istreambuf_iterator<char>(in), {});
                result = bytes.find("GetAsyncKeyState") != npos || bytes.find("GetKeyState") != npos;
            }
            cache[dll] = result;
            return result;
        }

        LoadedMods FindLoadedMods()
        {
            LoadedMods out;
            HMODULE    mods[1024];
            DWORD      needed = 0;
            if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
                const auto count = (std::min)(static_cast<std::size_t>(needed / sizeof(HMODULE)), std::size(mods));
                for (std::size_t i = 0; i < count; ++i) {
                    wchar_t path[MAX_PATH * 2];
                    const auto len = GetModuleFileNameW(mods[i], path, static_cast<DWORD>(std::size(path)));
                    if (!len) continue;
                    const fs::path p(std::wstring_view(path, len));
                    if (Lower(Utf8(p.filename())).starts_with("skse64_1")) out.skse = p;  // its RegisterForKey serves Papyrus mods
                    // SKSE plugins only (under MO2 the real path is mods\<mod>\SKSE\Plugins\x.dll)
                    if (Lower(Utf8(p.parent_path())).find("skse\\plugins") == npos) continue;
                    out.dlls.push_back(Norm(Utf8(p.stem())));
                    out.dllPaths.push_back(p);
                }
            }
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                for (const auto* file : dh->files) {
                    if (!file || file->GetCompileIndex() == 0xFF) continue;  // in Data but not active
                    const auto name = std::string(file->GetFilename());
                    out.pluginsExact.insert(Lower(name));
                    // the game's own masters name no mod ("Skyrim" would claim SkyrimNet's config)
                    const auto stem = Norm(fs::path(name).stem().string());
                    if (stem != "skyrim" && stem != "update" && stem != "dawnguard" && stem != "hearthfires" && stem != "dragonborn")
                        out.plugins.push_back(stem);
                }
            }
            return out;
        }

        struct IniEntry
        {
            std::string section;
            std::string name;
            std::string base;   // BaseName(name)
            std::string value;  // without trailing comment
            int         line;
        };

        // "uToggleUIKey" -> "toggleuikey": no Hungarian prefix, lower case
        std::string BaseName(const std::string& name)
        {
            if (name.size() > 2 && std::strchr("ibfsu", name[0]) && std::isupper(static_cast<unsigned char>(name[1]))) return Lower(name.substr(1));
            return Lower(name);
        }

        bool ParseBool(const std::string& v)
        {
            const auto l = Lower(v);
            if (l == "true" || l == "on" || l == "yes") return true;
            const auto n = ParseUInt(l);
            return n && *n != 0;
        }

        // Index of a setting in the same section whose base name is one of `names`, or npos.
        std::size_t FindSibling(const std::vector<IniEntry>& entries, std::size_t self, const std::vector<std::string>& names)
        {
            for (std::size_t j = 0; j < entries.size(); ++j) {
                if (j == self || entries[j].section != entries[self].section) continue;
                for (const auto& n : names)
                    if (entries[j].base == n) return j;
            }
            return npos;
        }

        // Settings an MCM Helper menu declares as "keymap": bound keys by definition, whatever
        // their name. Key: lower-case "section|name"; value: label shown in the MCM.
        struct McmKeymaps
        {
            std::string                        owner;
            std::map<std::string, std::string> labels;
            std::set<std::string>*             seen       = nullptr;  // keymaps already found in another file
            bool                               onlyUnseen = false;    // defaults file: fill in what the user file lacks
        };

        std::string McmKeyId(const std::string& section, const std::string& name)
        {
            return Lower(section) + '|' + Lower(name);
        }

        enum class FileResult
        {
            Read,
            TooBig,
            Unreadable,
            BadJson
        };

        constexpr std::uintmax_t kMaxFileSize = 2u * 1024 * 1024;

        FileResult ScanIni(const fs::path& file, const fs::path& dataDir, std::vector<Binding>& out, const McmKeymaps* mcm = nullptr)
        {
            std::error_code ec;
            const auto      size = fs::file_size(file, ec);
            if (ec) return FileResult::Unreadable;
            if (size > kMaxFileSize) return FileResult::TooBig;
            std::ifstream in(file, std::ios::binary);
            if (!in) return FileResult::Unreadable;

            const auto owner  = mcm ? mcm->owner : OwnerFromPath(file, dataDir);
            const auto origin = RelToData(file, dataDir).generic_string();

            // pass 1: every setting, so modifiers stored in sibling settings can be matched
            std::vector<IniEntry> entries;
            std::string           section, line;
            int                   lineNo = -1;
            while (std::getline(in, line)) {
                ++lineNo;
                if (lineNo == 0 && line.starts_with(kBom)) line.erase(0, kBom.size());
                const auto t = Trim(line);
                if (t.empty() || t[0] == ';' || t[0] == '#' || t.starts_with("//")) continue;
                if (t.front() == '[') {
                    if (const auto r = t.find(']'); r != npos) section = t.substr(1, r - 1);
                    continue;
                }
                const auto eq = t.find('=');
                if (eq == npos) continue;
                auto value = t.substr(eq + 1);
                if (const auto p = value.find_first_of(";#"); p != npos) value.erase(p);
                const auto name = Trim(t.substr(0, eq));
                entries.push_back({ section, name, BaseName(name), Trim(value), lineNo });
            }

            // pass 2: key settings plus their modifiers; settings used as a modifier are not listed on their own
            std::vector<bool> consumed(entries.size());
            std::vector<std::pair<std::size_t, Binding>> found;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto& e     = entries[i];
                std::string label = Humanize(e.name);
                if (mcm) {
                    const auto id = McmKeyId(e.section, e.name);
                    const auto it = mcm->labels.find(id);
                    if (it == mcm->labels.end()) {
                        if (mcm->onlyUnseen || !IsKeyLike(e.name)) continue;
                    } else {
                        if (mcm->onlyUnseen && mcm->seen->contains(id)) continue;
                        if (!it->second.empty()) label = it->second;
                        mcm->seen->insert(id);
                    }
                } else if (!IsKeyLike(e.name)) {
                    continue;
                }
                if (IsGamepadSetting(e.name) && ParseKeyName(e.value)) continue;  // "LB", "A": gamepad buttons
                if (const auto button = ParseSkseButton(e.value)) {
                    // mouse / gamepad: remapped by the input hook like keys, no modifiers
                    Binding b;
                    b.device     = button->first;
                    b.key        = button->second;
                    b.action     = label;
                    b.owner      = owner;
                    b.context    = e.section;
                    b.origin     = origin;
                    b.kind       = Kind::Ini;
                    b.editable   = true;
                    b.file       = file;
                    b.iniKey     = e.name;
                    b.line       = e.line;
                    b.defaultKey = CurrentCode(b);
                    MarkFileEdit(b);
                    found.emplace_back(i, std::move(b));
                    continue;
                }
                const auto val = ParseKey(e.value);
                if (!val) {
                    // A keymap the MCM declares but nobody set (-1): listed, so the action is known.
                    // Read-only: with no key of its own the mod listens to nothing we could remap.
                    if (mcm && mcm->labels.contains(McmKeyId(e.section, e.name)) && (e.value.empty() || e.value == "-1" || e.value == "0")) {
                        Binding u;
                        u.key        = kUnbound;
                        u.action     = label;
                        u.owner      = owner;
                        u.context    = e.section;
                        u.origin     = origin;
                        u.kind       = Kind::Ini;
                        u.file       = file;
                        u.iniKey     = e.name;
                        u.line       = e.line;
                        u.defaultKey = kUnbound;
                        found.emplace_back(i, std::move(u));
                    }
                    continue;
                }

                Binding b;
                b.key      = *val;
                b.action   = label;
                b.owner    = owner;
                b.context  = e.section;
                b.origin   = origin;
                b.kind     = Kind::Ini;
                b.editable = true;
                b.file     = file;
                b.iniKey   = e.name;
                b.line     = e.line;

                // uToggleUIKey + uToggleUIKeyShift / Ctrl / Alt
                static constexpr std::pair<const char*, int> kFlagNames[] = {
                    { "shift", 0 }, { "ctrl", 1 }, { "control", 1 }, { "alt", 2 }
                };
                for (const auto& [suffix, bit] : kFlagNames) {
                    const auto j = FindSibling(entries, i, { e.base + suffix, e.base + '_' + suffix });
                    if (j == npos) continue;
                    b.modStyle        = ModStyle::Flags;
                    b.flags[bit].line = entries[j].line;
                    b.flags[bit].name = entries[j].name;
                    if (ParseBool(entries[j].value)) b.mods |= kModBits[bit];
                    consumed[j] = true;
                }

                // iHotkey + iHotkeyModifier, iToggleKey + iToggleModifierKey, ...
                if (b.modStyle == ModStyle::None) {
                    std::vector<std::string> names = { e.base + "modifier", e.base + "_modifier", e.base + "mod", e.base + "modifierkey", e.base + "modkey" };
                    if (e.base.ends_with("key")) {
                        const auto stem = e.base.substr(0, e.base.size() - 3);
                        names.push_back(stem + "modifierkey");
                        names.push_back(stem + "modkey");
                        names.push_back(stem + "modifier");
                    }
                    if (const auto j = FindSibling(entries, i, names); j != npos) {
                        b.modStyle    = ModStyle::ModKey;
                        b.modKey.line = entries[j].line;
                        b.modKey.name = entries[j].name;
                        if (const auto m = ParseKey(entries[j].value)) b.mods = ModBitForDik(*m);
                        consumed[j] = true;
                    }
                }

                b.defaultKey = Combo(b.key, b.mods);
                MarkFileEdit(b);
                found.emplace_back(i, std::move(b));
            }
            for (auto& [i, b] : found)
                if (!consumed[i]) out.push_back(std::move(b));
            return FileResult::Read;
        }

        // ---------------------------------------------------------------- json scanning

        // Minimal JSON walker: finds integer values under key-like names, including small
        // arrays such as "Effects11ToggleKey": [16, 106] (Shift + Num*).
        class JsonScanner
        {
        public:
            JsonScanner(const std::string& text, const Binding& proto, std::vector<Binding>& out) :
                _s(text), _proto(proto), _out(out) {}

            void Run()
            {
                if (_s.compare(0, kBom.size(), kBom) == 0) _i = kBom.size();
                Value("", "", "", 0);
            }

        private:
            struct Number
            {
                std::size_t   offset, length;
                std::uint32_t value;
            };

            const std::string&    _s;
            const Binding&        _proto;
            std::vector<Binding>& _out;
            std::size_t           _i = 0;

            [[noreturn]] static void Fail() { throw std::runtime_error("bad json"); }

            void Ws()
            {
                while (_i < _s.size() && std::strchr(" \t\r\n", _s[_i]) && _s[_i]) ++_i;
            }

            char Peek()
            {
                Ws();
                return _i < _s.size() ? _s[_i] : '\0';
            }

            void Expect(char c)
            {
                if (Peek() != c) Fail();
                ++_i;
            }

            std::string String()
            {
                Expect('"');
                std::string r;
                while (_i < _s.size() && _s[_i] != '"') {
                    if (_s[_i] == '\\') ++_i;
                    if (_i < _s.size()) r += _s[_i++];
                }
                if (_i >= _s.size()) Fail();
                ++_i;
                return r;
            }

            // Skips a scalar; returns it if it is a non-negative integer.
            std::optional<Number> Scalar()
            {
                Ws();
                const auto start = _i;
                while (_i < _s.size() && !std::strchr(",]} \t\r\n", _s[_i])) ++_i;
                if (_i == start) Fail();
                unsigned   v = 0;
                const auto [p, ec] = std::from_chars(_s.data() + start, _s.data() + _i, v);
                if (ec != std::errc{} || p != _s.data() + _i) return std::nullopt;
                return Number{ start, _i - start, v };
            }

            std::optional<std::uint32_t> ToDik(std::uint32_t v) const
            {
                if (_proto.virtualKey) return VkToDik(v);
                if (v < 2 || v > 255) return std::nullopt;
                return v;
            }

            Binding Make(const std::string& path, const std::string& ctx, const std::string& name, const Number& n, std::uint32_t dik, const std::string& suffix)
            {
                Binding b     = _proto;
                b.key         = dik;
                b.action      = Humanize(name) + suffix;
                b.context     = ctx;
                b.iniKey      = path;
                b.valueOffset = n.offset;
                b.valueLength = n.length;
                b.fileValue   = n.value;
                return b;
            }

            void Push(Binding b)
            {
                b.defaultKey = Combo(b.key, b.mods);
                MarkFileEdit(b);
                _out.push_back(std::move(b));
            }

            void Emit(const std::string& path, const std::string& ctx, const std::string& name, const Number& n, std::uint32_t dik, const std::string& suffix)
            {
                Push(Make(path, ctx, name, n, dik, suffix));
            }

            void Value(const std::string& path, const std::string& ctx, const std::string& name, int depth)
            {
                if (depth > 64) Fail();
                const char c = Peek();
                if (c == '{') {
                    ++_i;
                    if (Peek() == '}') {
                        ++_i;
                        return;
                    }
                    for (;;) {
                        const auto key = String();
                        Expect(':');
                        Value(path.empty() ? key : path + '.' + key, path, key, depth + 1);
                        if (Peek() == ',') {
                            ++_i;
                            continue;
                        }
                        Expect('}');
                        return;
                    }
                }
                if (c == '[') {
                    const auto arrayStart = _i;
                    ++_i;
                    const bool          keyLike = IsKeyLike(name);
                    std::vector<Number> nums;
                    bool                plain = true;  // only integers inside
                    if (Peek() != ']') {
                        for (int idx = 0;; ++idx) {
                            const char e = Peek();
                            if (e == '{' || e == '[' || e == '"') {
                                plain = false;
                                Value(path + '[' + std::to_string(idx) + ']', ctx, name, depth + 1);
                            } else if (auto n = Scalar()) {
                                nums.push_back(*n);
                            } else {
                                plain = false;
                            }
                            if (Peek() == ',') {
                                ++_i;
                                continue;
                            }
                            break;
                        }
                    }
                    Expect(']');
                    if (keyLike && plain && !nums.empty()) EmitCombo(path, ctx, name, nums, arrayStart);
                    return;
                }
                if (c == '"') {
                    String();
                    return;
                }
                if (const auto n = Scalar(); n && IsKeyLike(name))
                    if (const auto dik = ToDik(n->value)) Emit(path, ctx, name, *n, *dik, "");
            }

            // [mod, mod, key] -> one binding on `key` with modifiers, owning the whole array;
            // otherwise one binding per element
            void EmitCombo(const std::string& path, const std::string& ctx, const std::string& name, const std::vector<Number>& nums, std::size_t arrayStart)
            {
                std::vector<std::optional<std::uint32_t>> diks;
                for (const auto& n : nums) diks.push_back(ToDik(n.value));

                int          main  = -1;
                std::uint8_t mods  = 0;
                bool         combo = true;
                for (std::size_t k = 0; k < nums.size(); ++k) {
                    if (!diks[k]) {
                        combo = false;
                        break;
                    }
                    if (const auto bit = ModBitForDik(*diks[k]); bit && nums.size() > 1) {
                        mods |= bit;
                    } else if (main < 0) {
                        main = static_cast<int>(k);
                    } else {
                        combo = false;
                    }
                }
                if (combo && main >= 0) {
                    auto b        = Make(path, ctx, name, nums[main], *diks[main], "");
                    b.mods        = mods;
                    b.modStyle    = ModStyle::JsonArray;
                    b.arrayOffset = arrayStart;
                    b.arrayText   = _s.substr(arrayStart, _i - arrayStart);
                    Push(std::move(b));
                    return;
                }
                for (std::size_t k = 0; k < nums.size(); ++k)
                    if (diks[k]) Emit(path + '[' + std::to_string(k) + ']', ctx, name, nums[k], *diks[k], " #" + std::to_string(k + 1));
            }
        };

        FileResult ScanJson(const fs::path& file, const fs::path& dataDir, std::vector<Binding>& out)
        {
            std::error_code ec;
            const auto      size = fs::file_size(file, ec);
            if (ec) return FileResult::Unreadable;
            if (size > kMaxFileSize) return FileResult::TooBig;
            std::ifstream in(file, std::ios::binary);
            if (!in) return FileResult::Unreadable;
            const std::string text(std::istreambuf_iterator<char>(in), {});

            Binding proto;
            proto.owner      = OwnerFromPath(file, dataDir);
            proto.origin     = file.filename().string();
            proto.origin     = RelToData(file, dataDir).generic_string();
            proto.kind       = Kind::Json;
            proto.editable   = true;
            proto.file       = file;
            proto.virtualKey = UsesVirtualKeys(proto.owner);

            std::vector<Binding> found;
            try {
                JsonScanner(text, proto, found).Run();
            } catch (...) {
                return FileResult::BadJson;  // ignore the whole file
            }
            out.insert(out.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
            return FileResult::Read;
        }

        // A YAML hotkey file (SkyrimNet's Hotkey.yaml): every number in it is a key, as a Windows
        // VK code, -1 = unset. Unset keys are listed too: they can be given one here.
        FileResult ScanYaml(const fs::path& file, const fs::path& dataDir, std::vector<Binding>& out)
        {
            std::error_code ec;
            const auto      size = fs::file_size(file, ec);
            if (ec) return FileResult::Unreadable;
            if (size > kMaxFileSize) return FileResult::TooBig;
            std::ifstream in(file, std::ios::binary);
            if (!in) return FileResult::Unreadable;
            const std::string text(std::istreambuf_iterator<char>(in), {});

            const auto owner  = OwnerFromPath(file, dataDir);
            const auto origin = RelToData(file, dataDir).generic_string();
            for (const auto& e : ParseYamlScalars(text)) {
                auto v = e.value;
                if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
                long n = 0;
                const auto [p, err] = std::from_chars(v.data(), v.data() + v.size(), n);
                if (err != std::errc{} || p != v.data() + v.size()) continue;  // not a key code
                const auto code = YamlCode(n);
                if (!code) continue;

                Binding b;
                if (*code == kUnbound) {
                    b.key = kUnbound;
                } else {
                    b.device = CodeDevice(*code);
                    b.key    = b.device == Device::Keyboard ? *code : CodeId(*code);
                }
                b.action = Humanize(e.name);  // "recordSpeech" -> "Record Speech"
                if (!b.action.empty()) b.action[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(b.action[0])));
                b.owner      = owner;
                b.context    = e.path;
                b.origin     = origin;
                b.kind       = Kind::Yaml;
                b.editable   = true;
                b.file       = file;
                b.iniKey     = e.name;
                b.line       = e.line;
                b.virtualKey = true;
                b.defaultKey = CurrentCode(b);
                MarkFileEdit(b);
                out.push_back(std::move(b));
            }
            return FileResult::Read;
        }

        // Only settings-like json files: data files (translations, form lists...) are noise.
        bool IsSettingsJson(const fs::path& p)
        {
            const auto name = Lower(p.filename().string());
            if (name.find("default") != npos || name.find("test") != npos) return false;  // CS SettingsDefault/SettingsTest
            for (auto w : { "setting", "config", "user", "hotkey", "keybind", "control", "input" })
                if (name.find(w) != npos) return true;
            return false;
        }

        // Interface/Translations/<mod>_english.txt: UTF-16 "$KEY<tab>text" lines.
        std::unordered_map<std::string, std::string> LoadTranslations(const fs::path& data, const std::string& mod)
        {
            std::unordered_map<std::string, std::string> map;
            std::string                                  raw;
            if (std::ifstream in(data / "Interface/Translations" / (mod + "_english.txt"), std::ios::binary); in) {
                raw.assign(std::istreambuf_iterator<char>(in), {});
            } else {
                // packed in the mod's BSA (Follower Live Package): the game's file system finds it
                RE::BSResourceNiBinaryStream bsa("Interface\\Translations\\" + mod + "_english.txt");
                if (!bsa.good() || !bsa.stream || !bsa.stream->totalSize || bsa.stream->totalSize > kMaxFileSize) return map;
                raw.resize(bsa.stream->totalSize);
                if (!bsa.read(raw.data(), static_cast<std::uint32_t>(raw.size()))) return map;
            }
            if (raw.size() < 2 || static_cast<unsigned char>(raw[0]) != 0xFF || static_cast<unsigned char>(raw[1]) != 0xFE) return map;

            const auto* w   = reinterpret_cast<const wchar_t*>(raw.data() + 2);
            const int   len = static_cast<int>((raw.size() - 2) / sizeof(wchar_t));
            std::string text(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, len, text.data(), static_cast<int>(text.size()), nullptr, nullptr);

            for (std::size_t pos = 0; pos < text.size();) {
                auto       nl   = text.find('\n', pos);
                const auto line = Trim(std::string_view(text).substr(pos, nl == npos ? npos : nl - pos));
                pos             = nl == npos ? text.size() : nl + 1;
                if (const auto tab = line.find('\t'); tab != npos) map[line.substr(0, tab)] = Trim(line.substr(tab + 1));
            }
            return map;
        }

        void CollectKeymaps(const JNode& n, const std::unordered_map<std::string, std::string>& tr, std::map<std::string, std::string>& out)
        {
            if (n.t == JNode::T::Obj && n.Str("type") == "keymap") {
                // "id": "iKeybind:Controls" = setting:section
                const auto id    = n.Str("id");
                const auto colon = id.find(':');
                if (colon != npos) {
                    auto label = n.Str("text");
                    if (label.starts_with('$')) {
                        const auto it = tr.find(label);
                        label         = it != tr.end() ? it->second : label.substr(1);
                    }
                    out[McmKeyId(id.substr(colon + 1), id.substr(0, colon))] = label;
                }
            }
            for (const auto& [k, v] : n.obj) CollectKeymaps(v, tr, out);
            for (const auto& v : n.arr) CollectKeymaps(v, tr, out);
        }

        // A "keymap" entry of a dMenu settings json.
        struct DMenuKey
        {
            std::string section, name, label, def;  // def: default SKSE key code, as written
        };

        void CollectDMenuKeymaps(const JNode& n, std::vector<DMenuKey>& out)
        {
            if (n.t == JNode::T::Obj && n.Str("type") == "keymap") {
                if (const auto* ini = n.Get("ini")) {
                    DMenuKey k{ ini->Str("section"), ini->Str("id") };
                    if (const auto* text = n.Get("text")) k.label = text->Str("name");
                    if (const auto* d = n.Get("default"); d && d->t == JNode::T::Other) k.def = d->s;
                    if (!k.name.empty()) out.push_back(std::move(k));
                }
            }
            for (const auto& [k, v] : n.obj) CollectDMenuKeymaps(v, out);
            for (const auto& v : n.arr) CollectDMenuKeymaps(v, out);
        }

        // What a scan looked at, for the log.
        struct ScanStats
        {
            std::size_t files = 0, withKeys = 0, skipped = 0;
            void        Add(std::size_t hotkeys, bool skip)
            {
                ++files;
                withKeys += hotkeys > 0;
                skipped += skip;
            }
        };

        // Scans one ini/json file.
        void ScanCounted(const fs::path& file, const fs::path& data, std::vector<Binding>& out, ScanStats& stats, bool json,
            const McmKeymaps* mcm = nullptr)
        {
            const auto before = out.size();
            FileResult r;
            try {
                r = json ? ScanJson(file, data, out) : ScanIni(file, data, out, mcm);
            } catch (...) {
                r = FileResult::Unreadable;  // odd file names / encodings
            }
            stats.Add(out.size() - before, r != FileResult::Read);
        }

        // MCM Helper mods: MCM/Config/<Mod>/config.json declares the keymaps, the values are in
        // MCM/Settings/<Mod>.ini (only once the user changed something) or else in the
        // defaults, MCM/Config/<Mod>/settings.ini. Returns the MCM/Settings files handled here.
        std::set<std::string> ScanMcmHelper(const fs::path& data, std::vector<Binding>& out, ScanStats& stats, const LoadedMods& loaded)
        {
            std::set<std::string> handled;
            std::error_code       ec;
            for (fs::directory_iterator it(data / "MCM/Config", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e2;
                if (!it->is_directory(e2)) continue;
                const auto mod    = it->path().filename().string();
                const auto config = it->path() / "config.json";
                try {
                    std::ifstream in(config, std::ios::binary);
                    if (!in) continue;  // not an MCM Helper menu
                    const std::string json(std::istreambuf_iterator<char>(in), {});

                    McmKeymaps keys;
                    keys.owner = mod;
                    CollectKeymaps(JsonDom(json).Parse(), LoadTranslations(data, mod), keys.labels);
                    stats.Add(keys.labels.size(), false);
                    if (keys.labels.empty()) continue;  // its MCM/Settings ini, if any, is still read by the generic scan

                    // MCM Helper names the folder after the mod's plugin: no plugin loaded, no menu
                    if (loaded.Known() && !loaded.PluginLoaded(mod) && !loaded.AnyLoaded({ mod })) {
                        logger::info("scan: {} is not loaded, its MCM hotkeys are left out", mod);
                        handled.insert(Lower(mod + ".ini"));  // its MCM/Settings file too
                        continue;
                    }

                    std::set<std::string> seen;
                    keys.seen       = &seen;
                    const auto from = out.size();
                    const auto user = data / "MCM/Settings" / (mod + ".ini");
                    if (fs::exists(user, e2)) {
                        ScanCounted(user, data, out, stats, false, &keys);
                        handled.insert(Lower(user.filename().string()));
                    }
                    keys.onlyUnseen    = true;
                    const auto defaults = it->path() / "settings.ini";
                    if (fs::exists(defaults, e2)) ScanCounted(defaults, data, out, stats, false, &keys);
                    // MCM Helper's own keybinds, or the mod's scripts / dll
                    auto readers = loaded.PapyrusReaders({ mod });
                    if (const auto helper = loaded.DllFor({ "MCMHelper" })) readers.push_back(*helper);
                    for (auto i = from; i < out.size(); ++i) out[i].readers = readers;
                } catch (...) {
                    stats.Add(0, true);  // broken config.json
                }
            }
            return handled;
        }

        // dMenu mod settings: SKSE/Plugins/dmenu/customSettings/<Mod>.json declares them; its
        // "keymap" entries are keys whatever their name (Wheeler: toggleWheel, nextItem...), as
        // SKSE key codes in the ini the json names (by default customSettings/ini/<Mod>.ini), or
        // the json's default while nobody changed them. Returns the ini files handled here
        // (lower case, relative to Data/), which the generic scan then skips.
        std::set<std::string> ScanDMenu(const fs::path& data, std::vector<Binding>& out, ScanStats& stats, const LoadedMods& loaded)
        {
            std::set<std::string> handled;
            const auto            dir = data / "SKSE/Plugins/dmenu/customSettings";
            std::error_code       ec;
            for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e2;
                if (!it->is_regular_file(e2) || Lower(it->path().extension().string()) != ".json") continue;
                const auto mod = Utf8(it->path().stem());
                try {
                    std::ifstream in(it->path(), std::ios::binary);
                    if (!in) continue;
                    const std::string json(std::istreambuf_iterator<char>(in), {});
                    const auto        root = JsonDom(json).Parse();

                    std::vector<DMenuKey> keys;
                    if (const auto* list = root.Get("data")) CollectDMenuKeymaps(*list, keys);
                    stats.Add(keys.size(), false);
                    if (keys.empty()) continue;

                    // the ini path is relative to the game folder ("Data\SKSE\Plugins\wheeler\Controls.ini")
                    const auto iniText = root.Str("ini");
                    const auto ini     = iniText.empty() ? dir / "ini" / (mod + ".ini") : fs::path(std::u8string(iniText.begin(), iniText.end()));
                    handled.insert(Lower(RelToData(ini, data).generic_string()));
                    if (loaded.Known() && !loaded.AnyLoaded({ mod })) {
                        logger::info("scan: {} is not loaded, its dMenu hotkeys are left out", mod);
                        continue;
                    }

                    McmKeymaps            map;
                    std::set<std::string> seen;
                    map.owner = mod;
                    map.seen  = &seen;
                    for (const auto& k : keys) map.labels[McmKeyId(k.section, k.name)] = k.label;
                    const auto from = out.size();
                    if (fs::exists(ini, e2)) ScanCounted(ini, data, out, stats, false, &map);

                    // never changed in dMenu, so not in the ini yet: the json's default key
                    for (const auto& k : keys) {
                        if (seen.contains(McmKeyId(k.section, k.name))) continue;
                        Binding b;
                        if (const auto button = ParseSkseButton(k.def)) {
                            b.device = button->first;
                            b.key    = button->second;
                        } else if (const auto key = ParseKey(k.def)) {
                            b.key = *key;
                        } else {
                            b.key = kUnbound;  // 0 = unmapped: listed read-only, like MCM Helper's
                        }
                        b.action     = k.label.empty() ? Humanize(k.name) : k.label;
                        b.owner      = mod;
                        b.context    = k.section;
                        b.origin     = RelToData(ini, data).generic_string();
                        b.kind       = Kind::Ini;
                        b.editable   = b.key != kUnbound;
                        b.file       = ini;
                        b.iniKey     = k.name;
                        b.defaultKey = CurrentCode(b);
                        MarkFileEdit(b);
                        out.push_back(std::move(b));
                    }
                    if (const auto dll = loaded.DllFor({ mod }))
                        for (auto i = from; i < out.size(); ++i) out[i].readers = { *dll };
                } catch (...) {
                    stats.Add(0, true);  // broken json
                }
            }
            return handled;
        }

        // ENB: enblocal.ini in the game folder, [INPUT], Windows VK codes. Most of its keys work
        // together with KeyCombination (Shift: Shift+Enter opens the editor); the FPS display,
        // the screenshot and the reload key work alone (enbdev.com/doc_skyrim_input_en.htm). ENB reads the keyboard itself, past the game's input,
        // so its keys can't be remapped: listed read-only.
        void ScanEnb(std::vector<Binding>& out, ScanStats& stats)
        {
            const fs::path  file = "enblocal.ini";  // cwd: the game folder
            std::error_code ec;
            if (!fs::exists(file, ec)) return;
            std::ifstream in(file, std::ios::binary);
            if (!in) return;

            std::map<std::string, std::string> keys;  // lower-case name -> value, [INPUT] only
            std::map<std::string, std::string> names;  // lower-case name -> name as written
            std::map<std::string, int>         lines;
            std::string                        section, line;
            for (int n = 0; std::getline(in, line); ++n) {
                auto t = Trim(line);
                if (t.empty() || t[0] == ';' || t[0] == '#') continue;
                if (t.front() == '[') {
                    section = Lower(t.substr(1, t.find(']') - 1));
                    continue;
                }
                const auto eq = t.find('=');
                if (section != "input" || eq == npos) continue;
                auto value = t.substr(eq + 1);
                if (const auto p = value.find_first_of(";#"); p != npos) value.erase(p);
                const auto name = Trim(t.substr(0, eq));
                keys[Lower(name)]  = Trim(value);
                names[Lower(name)] = name;
                lines[Lower(name)] = n;
            }

            // the combination key: Shift / Ctrl / Alt as a modifier, any other key as a held key
            std::uint8_t  mods = 0;
            std::uint32_t hold = 0;
            if (const auto it = keys.find("keycombination"); it != keys.end())
                if (const auto vk = ParseUInt(it->second); vk && *vk) {
                    if (*vk == VK_SHIFT || *vk == VK_LSHIFT || *vk == VK_RSHIFT) mods = kShift;
                    else if (*vk == VK_CONTROL || *vk == VK_LCONTROL || *vk == VK_RCONTROL) mods = kCtrl;
                    else if (*vk == VK_MENU || *vk == VK_LMENU || *vk == VK_RMENU) mods = kAlt;
                    else if (const auto dik = VkToDik(*vk)) hold = *dik;
                }

            static constexpr struct
            {
                const char* name;   // lower case
                const char* label;
                bool        combo;  // pressed with KeyCombination
            } kEnbKeys[] = {
                { "keyuseeffect", "Toggle ENB", true }, { "keyeditor", "Open ENB editor", true }, { "keyfpslimit", "Toggle FPS limiter", true },
                { "keyshowfps", "Show FPS", false }, { "keyscreenshot", "Screenshot", false }, { "keyreadconfig", "Reload ENB settings", false },
                { "keyfreevram", "Free video memory", true }, { "keybruteforce", "Brute force mode", true }, { "keydof", "Depth of field focus", true },
            };
            std::size_t found = 0;
            for (const auto& k : kEnbKeys) {
                const auto it = keys.find(k.name);
                if (it == keys.end()) continue;
                const auto vk  = ParseUInt(it->second);
                const auto dik = vk && *vk ? VkToDik(*vk) : std::nullopt;
                Binding    b;
                if (!dik) {
                    b.key = kUnbound;
                } else {
                    b.key  = *dik;
                    b.mods = k.combo ? mods : 0;
                    b.hold = k.combo ? hold : 0;
                }
                b.action     = k.label;
                b.owner      = "ENB";
                b.context    = "INPUT";
                b.origin     = "enblocal.ini";
                b.kind       = Kind::Ini;
                b.editable   = false;
                b.file       = file;
                b.iniKey     = names[k.name];
                b.line       = lines[k.name];
                b.defaultKey = CurrentCode(b);
                out.push_back(std::move(b));
                ++found;
            }
            stats.Add(found, false);
        }

        // Object Manipulation Overhaul: Data/Object Manipulation Overhaul/KeyConfiguration.txt,
        // one "Action, Device, Key" line per key (an action may have several), key names as in
        // Skyrim's controlmap, lower-cased by the mod. It reads the game's input events, so its
        // keys are remapped by the input hook like any mod key.
        void ScanOmo(const fs::path& data, std::vector<Binding>& out, ScanStats& stats, const LoadedMods& loaded)
        {
            const auto      file = data / "Object Manipulation Overhaul" / "KeyConfiguration.txt";
            std::error_code ec;
            if (!fs::exists(file, ec)) return;
            if (loaded.Known() && !loaded.AnyLoaded({ "ObjectManipulationOverhaul" })) {
                logger::info("scan: {} belongs to no loaded mod, left out", RelToData(file, data).generic_string());
                return;
            }
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                stats.Add(0, true);
                return;
            }

            // names the mod knows beyond ParseKeyName's (its keyboard list is DirectInput's order)
            static const std::map<std::string, std::uint32_t, std::less<>> kKeys = {
                { "bracketleft", 26 }, { "bracketright", 27 }, { "kp_multiply", 55 }, { "kp_7", 71 }, { "kp_8", 72 }, { "kp_9", 73 },
                { "kp_subtract", 74 }, { "kp_4", 75 }, { "kp_5", 76 }, { "kp_6", 77 }, { "kp_plus", 78 }, { "kp_1", 79 }, { "kp_2", 80 },
                { "kp_3", 81 }, { "kp_0", 82 }, { "kp_decimal", 83 }, { "kp_enter", 156 }, { "kp_divide", 181 }, { "leftwin", 219 },
                { "rightwin", 220 }
            };
            static const std::map<std::string, std::uint32_t, std::less<>> kMouse = {
                { "leftbutton", kMouseLeft }, { "rightbutton", kMouseRight }, { "middlebutton", kMouseMiddle }, { "button3", kMouse4 },
                { "button4", kMouse5 }, { "button5", kMouse6 }, { "button6", kMouse7 }, { "button7", kMouse8 },
                { "wheelup", kMouseWheelUp }, { "wheeldown", kMouseWheelDown }
            };
            static const std::map<std::string, std::uint32_t, std::less<>> kPad = {
                { "up", kPadUp }, { "down", kPadDown }, { "left", kPadLeft }, { "right", kPadRight }, { "start", kPadStart },
                { "back", kPadBack }, { "leftthumb", kPadL3 }, { "rightthumb", kPadR3 }, { "leftshoulder", kPadLB },
                { "rightshoulder", kPadRB }, { "a", kPadA }, { "b", kPadB }, { "x", kPadX }, { "y", kPadY },
                { "lefttrigger", kPadLT }, { "righttrigger", kPadRT }
            };

            const auto            origin = RelToData(file, data).generic_string();
            std::map<std::string, int> count;  // per action: "Rotate", "Rotate 2"
            std::size_t           found = 0;
            std::string           line;
            for (int n = 0; std::getline(in, line); ++n) {
                if (n == 0 && line.starts_with(kBom)) line.erase(0, kBom.size());
                const auto t = Trim(line);
                if (t.empty() || t[0] == '#') continue;
                std::vector<std::string> cols;
                for (std::size_t pos = 0; pos <= t.size();) {
                    const auto c = t.find(',', pos);
                    cols.push_back(Trim(t.substr(pos, c == npos ? npos : c - pos)));
                    if (c == npos) break;
                    pos = c + 1;
                }
                if (cols.size() < 3 || cols[0].empty()) continue;
                const auto device = Lower(cols[1]);
                const auto name   = Lower(cols[2]);

                Binding b;
                if (device == "keyboard") {
                    if (const auto it = kKeys.find(name); it != kKeys.end()) {
                        b.key = it->second;
                    } else if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
                        b.key = name[0] == '0' ? 11 : 2 + (name[0] - '1');
                    } else if (const auto k = ParseKeyName(name)) {
                        b.key = *k;
                    } else {
                        continue;
                    }
                } else if (device == "mouse" || device == "gamepad") {
                    const auto& table = device == "mouse" ? kMouse : kPad;
                    const auto  it    = table.find(name);
                    if (it == table.end()) continue;
                    b.device = device == "mouse" ? Device::Mouse : Device::Gamepad;
                    b.key    = it->second;
                } else {
                    continue;
                }
                const int nth = ++count[Lower(cols[0])];
                b.action      = Humanize(cols[0]);  // "ToggleAdvancedMode" -> "Toggle Advanced Mode"
                b.owner       = "Object Manipulation Overhaul";
                b.context     = "Keys";
                b.origin      = origin;
                b.kind        = Kind::Ini;
                b.editable    = true;
                b.file        = file;
                b.iniKey      = nth == 1 ? cols[0] : std::format("{} {}", cols[0], nth);
                if (const auto dll = loaded.DllFor({ "ObjectManipulationOverhaul" })) b.readers = { *dll };
                b.line        = n;
                b.defaultKey  = CurrentCode(b);
                MarkFileEdit(b);
                out.push_back(std::move(b));
                ++found;
            }
            stats.Add(found, false);
        }

        // Plain SkyUI menus: the keys their keymap options show live in script variables (saved
        // with the game), found through the menu's compiled script. Such mods listen with
        // RegisterForKey, which sees the game's input: remapped by the input hook.
        void ScanPapyrusMcm(const fs::path& data, const std::vector<McmMenu>& menus, std::vector<Binding>& out, ScanStats& stats,
            const LoadedMods& loaded)
        {
            for (const auto& m : menus) {
                const auto stem    = fs::path(m.plugin).stem().string();
                const auto readers = loaded.PapyrusReaders({ stem });
                const auto tr   = stem.empty() ? std::unordered_map<std::string, std::string>{} : LoadTranslations(data, stem);
                const auto text = [&](const std::string& s) {
                    if (!s.starts_with('$')) return s;
                    const auto it = tr.find(s);
                    return it != tr.end() ? it->second : s.substr(1);
                };
                const auto owner = text(m.name);

                std::size_t found = 0;
                for (const auto& script : m.scripts) {
                    for (const auto& k : ReadPexKeymaps(script)) {
                        // "::PKEY_var" (an auto property) -> "PKEY"
                        auto name = k.var;
                        if (name.starts_with("::")) name.erase(0, 2);
                        if (name.ends_with("_var")) name.erase(name.size() - 4);
                        const auto var = Lower(k.var);

                        const auto add = [&](int value, int index, const std::string& label) {
                            Binding b;
                            if (const auto button = ParseSkseButton(std::to_string(value))) {
                                b.device = button->first;
                                b.key    = button->second;
                            } else if (value >= 2 && value <= 255) {
                                b.key = static_cast<std::uint32_t>(value);
                            } else {
                                b.key = kUnbound;  // -1 / 0: no key set, the mod listens to nothing we could remap
                            }
                            b.action   = label.empty() ? Humanize(name) : text(label);
                            b.owner    = owner;
                            b.context  = "MCM";
                            b.origin   = "Scripts/" + script + ".pex";
                            b.kind     = Kind::Ini;
                            b.editable = b.key != kUnbound;
                            b.file     = data / "Scripts" / (script + ".pex");
                            b.iniKey   = index < 0 ? name : std::format("{}[{}]", name, index);
                            b.readers  = readers;
                            b.defaultKey = CurrentCode(b);
                            MarkFileEdit(b);
                            out.push_back(std::move(b));
                            ++found;
                        };

                        if (k.global) {
                            if (const auto it = m.globals.find(var); it != m.globals.end()) add(it->second, -1, k.label);
                            continue;
                        }
                        if (k.index < 0 && !k.all) {
                            if (const auto it = m.ints.find(var); it != m.ints.end()) add(it->second, -1, k.label);
                            continue;
                        }
                        const auto arr = m.intArrays.find(var);
                        if (arr == m.intArrays.end()) continue;
                        if (!k.all) {
                            if (k.index < static_cast<int>(arr->second.size())) add(arr->second[k.index], k.index, k.label);
                            continue;
                        }
                        const auto labels = m.strArrays.find(Lower(k.labelArray));
                        for (std::size_t i = 0; i < arr->second.size(); ++i) {
                            const bool named = labels != m.strArrays.end() && i < labels->second.size() && !labels->second[i].empty();
                            if (!named && labels != m.strArrays.end()) continue;  // a spare slot no option shows
                            add(arr->second[i], static_cast<int>(i), named ? labels->second[i] : std::string());
                        }
                    }
                }
                stats.Add(found, false);
                if (found) logger::info("scan: MCM {} ({}): {} hotkey(s) in its scripts", owner, m.plugin, found);
            }
        }

        std::vector<Binding> ScanFiles(ScanStats& stats, const std::vector<McmMenu>& menus)
        {
            std::vector<Binding> out;
            const fs::path       data       = "Data";  // process cwd is the game folder; MO2's VFS hooks these calls
            const auto           loaded     = FindLoadedMods();
            const auto           mcmHandled = ScanMcmHelper(data, out, stats, loaded);
            const auto           dmenuIni   = ScanDMenu(data, out, stats, loaded);
            ScanEnb(out, stats);
            ScanOmo(data, out, stats, loaded);
            ScanPapyrusMcm(data, menus, out, stats, loaded);
            if (!loaded.Known())
                logger::warn("scan: could not list the loaded mods, configs of switched-off mods may show");
            else
                logger::info("scan: {} SKSE plugins and {} game plugins loaded", loaded.dlls.size(), loaded.plugins.size());

            // A config whose folder / file name matches no loaded dll or plugin belongs to a mod
            // that is switched off: SKSE/Plugins/<Mod>/... or SKSE/Plugins/<Mod>.ini, MCM/Settings/<Mod>.ini
            const auto modOff = [&](const fs::path& file, std::string_view sub) {
                if (!loaded.Known()) return false;
                const auto rel = file.lexically_relative(data / sub);
                if (rel.empty() || *rel.begin() == "..") return false;
                const auto stem = Utf8(file.stem());
                const auto top  = Utf8(*rel.begin());
                const bool off  = std::distance(rel.begin(), rel.end()) > 1 ? !loaded.AnyLoaded({ top, stem }) : !loaded.AnyLoaded({ stem }) && !loaded.PluginLoaded(stem);
                if (off) logger::info("scan: {} belongs to no loaded mod, left out", RelToData(file, data).generic_string());
                return off;
            };

            // A mod that reads the keyboard itself: its keys are listed read-only (the input hook
            // only changes what the game's input events say, which it never looks at). Told by
            // two signs together: the key written as a Windows key name (VK_F2: Windows' codes,
            // the game has its own) and its dll asking Windows for key states.
            const auto markOwnInput = [&](const fs::path& file, std::string_view sub, std::vector<Binding>& binds) {
                const auto rel = file.lexically_relative(data / sub);
                if (rel.empty() || *rel.begin() == "..") return;
                std::vector<std::string> lines;
                {
                    std::ifstream in(file, std::ios::binary);
                    for (std::string line; std::getline(in, line);) lines.push_back(std::move(line));
                }
                const auto vkName = [&](const Binding& b) {
                    if (b.kind != Kind::Ini || b.line < 0 || b.line >= static_cast<int>(lines.size())) return false;
                    const auto& line = lines[b.line];
                    const auto  eq   = line.find('=');
                    return eq != npos && Lower(Trim(line.substr(eq + 1))).starts_with("vk_");
                };
                if (std::ranges::none_of(binds, vkName)) return;
                const auto dll = loaded.DllFor({ Utf8(*rel.begin()), Utf8(file.stem()) });
                if (!dll || !PollsWindowsKeys(*dll)) return;
                logger::info("scan: {} reads the keyboard itself, its keys in {} are read-only", Utf8(dll->filename()), RelToData(file, data).generic_string());
                for (auto& b : binds)
                    if (vkName(b)) {
                        b.ownInput = true;
                        b.editable = false;
                    }
            };

            for (const auto* sub : { "SKSE/Plugins", "MCM/Settings" }) {
                std::error_code ec;
                for (fs::recursive_directory_iterator it(data / sub, fs::directory_options::skip_permission_denied, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    std::error_code e2;
                    if (!it->is_regular_file(e2)) continue;
                    std::string ext, name;
                    try {
                        ext  = Lower(it->path().extension().string());
                        name = Lower(it->path().filename().string());
                    } catch (...) {
                        continue;  // name not representable
                    }
                    if (ext == ".yaml" || ext == ".yml") {
                        // only hotkey files: a mod's other YAML configs (SkyrimNet has dozens) hold no keys
                        if (name.find("hotkey") == npos && name.find("keybind") == npos) continue;
                        std::vector<Binding> got;
                        FileResult           r;
                        try {
                            r = ScanYaml(it->path(), data, got);
                        } catch (...) {
                            r = FileResult::Unreadable;
                        }
                        stats.Add(got.size(), r != FileResult::Read);
                        if (!got.empty() && !modOff(it->path(), sub))
                            out.insert(out.end(), std::make_move_iterator(got.begin()), std::make_move_iterator(got.end()));
                        continue;
                    }
                    if (ext != ".ini" && ext != ".json") continue;  // dlls, logs, textures...
                    if (std::string_view(sub) == "MCM/Settings" && mcmHandled.contains(name)) continue;  // read by ScanMcmHelper
                    if (dmenuIni.contains(Lower(RelToData(it->path(), data).generic_string()))) continue;  // read by ScanDMenu
                    // skipped: Hotkey Atlas's own files, json whose name is not a settings name
                    if (name.starts_with("hotkeyatlas") || (ext == ".json" && !IsSettingsJson(it->path()))) {
                        stats.Add(0, true);
                        continue;
                    }
                    std::vector<Binding> got;
                    ScanCounted(it->path(), data, got, stats, ext == ".json");
                    if (!got.empty() && modOff(it->path(), sub)) continue;  // hotkeys of a switched-off mod
                    if (!got.empty()) {
                        markOwnInput(it->path(), sub, got);
                        // the dll named like the file or its folder under SKSE/Plugins reads it
                        const auto rel  = it->path().lexically_relative(data / sub);
                        const auto top  = rel.empty() ? std::string() : Utf8(*rel.begin());
                        const auto stem = Utf8(it->path().stem());
                        std::vector<fs::path> readers;
                        if (std::string_view(sub) == "MCM/Settings")
                            readers = loaded.PapyrusReaders({ stem });
                        else if (const auto dll = loaded.DllFor({ top, stem }))
                            readers = { *dll };
                        else if (const auto same = loaded.DllInSameMod(it->path()))
                            readers = { *same };
                        for (auto& b : got) b.readers = readers;
                    }
                    out.insert(out.end(), std::make_move_iterator(got.begin()), std::make_move_iterator(got.end()));
                }
            }
            // a mod key parked on a key no keyboard has is switched off (see IsPhantomKey);
            // once the user moved it to a real key it is a bind again
            const auto off = std::erase_if(out, [](const Binding& b) { return b.device == Device::Keyboard && IsPhantomKey(b.key); });
            if (off) logger::info("scan: {} mod hotkey(s) on F13-F24 left out (switched off, e.g. by a menu launcher)", off);
            return out;
        }

        void Publish(std::vector<Binding> cm, std::vector<Binding> files, const ScanStats& stats)
        {
            logger::info("scan: {} files, {} with hotkeys, {} skipped, {} hotkeys in total ({} Skyrim controls)", stats.files, stats.withKeys,
                stats.skipped, cm.size() + files.size(), cm.size());
            auto model = std::make_shared<Model>();
            model->all = std::move(cm);
            model->all.insert(model->all.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
            if (const auto notes = BuiltIn(); !notes->mods.empty())
                for (auto& b : model->all)
                    if (b.kind != Kind::ControlMap) b.description = DescribeModBinding(b, *notes);
            {
                // gamepad buttons added to key / mouse bindings (dropped silently when the mod's key changed since)
                std::lock_guard l(g_ovLock);
                for (auto& b : model->all) {
                    if (CodeDevice(b.defaultKey) == Device::Gamepad) continue;
                    const bool  control = b.kind == Kind::ControlMap;
                    const auto& map     = control ? g_padControls : g_padMods;
                    const auto  it      = map.find(control ? OverrideId(b.ctx, b.action, b.defaultKey) : FileEditId(b));
                    if (it != map.end() && it->second.original == b.defaultKey) b.padKey = it->second.key;
                }
            }
            std::sort(model->all.begin(), model->all.end(), [](const Binding& a, const Binding& b) {
                return std::tie(a.device, a.key, a.mods, a.owner, a.action) < std::tie(b.device, b.key, b.mods, b.owner, b.action);
            });
            for (std::size_t i = 0; i < model->all.size(); ++i)
            {
                // every keyboard key a binding takes: its key, the key held first, its modifiers
                const auto& b    = model->all[i];
                const auto  code = CurrentCode(b);
                if (code == kUnbound) continue;
                std::uint32_t keys[8]{};
                std::size_t   n = 0;
                if (b.device == Device::Keyboard) keys[n++] = b.key;
                if (b.hold && CodeDevice(b.hold) == Device::Keyboard) keys[n++] = CodeId(b.hold);
                for (const auto dik : { 42u, 54u, 29u, 157u, 56u, 184u })
                    if (dik != b.key && CodeUses(code, Device::Keyboard, dik)) keys[n++] = dik;
                for (std::size_t k = 0; k < n; ++k)
                    if (std::find(keys, keys + k, keys[k]) == keys + k) model->byKey[keys[k]].push_back(i);
            }

            {
                // which dlls each remap is shown to; the hook's table is rebuilt on the game thread
                std::map<std::string, std::vector<fs::path>> readers;
                for (const auto& b : model->all)
                    if (b.kind != Kind::ControlMap && !b.readers.empty()) readers[FileEditId(b)] = b.readers;
                std::lock_guard l(g_ovLock);
                SetRemapReadersLocked(std::move(readers));
            }
            SKSE::GetTaskInterface()->AddTask([] {
                std::lock_guard l(g_ovLock);
                RebuildComboTableLocked();
            });

            std::lock_guard l(g_lock);
            g_model = std::move(model);
        }
    }

    void Rescan()
    {
        if (g_busy.exchange(true)) {
            g_again = true;
            return;
        }
        SKSE::GetTaskInterface()->AddTask([] {
            ApplyOverrides();            // game thread
            LoadBuiltInNotes();          // Notes.json edits show after a Rescan
            auto cm    = ReadControlMap();     // game thread
            auto menus = SnapshotMcmMenus();   // game thread
            std::thread([cm = std::move(cm), menus = std::move(menus)]() mutable {
                ScanStats stats;
                auto      files = ScanFiles(stats, menus);
                Publish(std::move(cm), std::move(files), stats);
                g_busy = false;
                if (g_again.exchange(false) | g_restoredFiles.exchange(false)) Rescan();
            }).detach();
        });
    }

    void ApplyOverridesLater()
    {
        SKSE::GetTaskInterface()->AddTask([] { ApplyOverrides(); });
    }

    std::shared_ptr<const Model> GetModel()
    {
        std::lock_guard l(g_lock);
        return g_model;
    }

    bool IsBusy() { return g_busy.load(); }

    std::string GetStatus()
    {
        std::lock_guard l(g_lock);
        return g_status;
    }
}
