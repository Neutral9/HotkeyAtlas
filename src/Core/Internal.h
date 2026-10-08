#pragma once

// Shared internals of the core (everything below HotkeyAtlas.h). Not for the menu pages,
// which see the core through HotkeyAtlas.h plus the few helpers UI/UI.h pulls in.

#include "HotkeyAtlas.h"

namespace HA
{
    inline constexpr std::string_view kBom = "\xEF\xBB\xBF";
    inline constexpr auto             npos = std::string::npos;

    // ---------------------------------------------------------------- text (Text.cpp)

    std::string                                     Trim(std::string_view s);
    std::string                                     Lower(std::string s);
    std::string                                     Humanize(std::string name);  // "iToggleHotkey:Main" -> "Toggle Hotkey"
    bool                                            IsKeyLike(const std::string& name);
    bool                                            IsGamepadSetting(const std::string& name);
    std::optional<std::uint32_t>                    ParseUInt(std::string_view s);
    std::optional<std::uint32_t>                    ParseKeyName(std::string_view v);  // "F1", "PageUp" -> DIK
    std::optional<std::uint32_t>                    ParseKey(std::string v);           // number or key name -> DIK
    std::optional<std::pair<Device, std::uint32_t>> ParseSkseButton(std::string v);    // SKSE mouse / gamepad codes
    std::optional<std::uint32_t>                    VkToDik(std::uint32_t vk);
    std::optional<std::uint32_t>                    DikToVk(std::uint32_t dik);
    bool                                            IsPhantomKey(std::uint32_t dik);  // F13-F24...: a switched-off mod key
    std::uint8_t                                    ModBitForDik(std::uint32_t dik);
    bool                                            UsesVirtualKeys(const std::string& owner);
    std::string                                     ModsText(std::uint8_t mods);  // "Ctrl+Shift+"
    std::string                                     Utf8(const fs::path& p);

    // ---------------------------------------------------------------- input contexts (Contexts.cpp)

    // Input contexts in AE order (RE::UserEvents::INPUT_CONTEXT_ID). SE has no
    // Marketplace, so its Favor context sits at 16.
    enum Ctx : int
    {
        kCtxGameplay,
        kCtxMenu,
        kCtxConsole,
        kCtxItemMenu,
        kCtxInventory,
        kCtxDebugText,
        kCtxFavorites,
        kCtxMap,
        kCtxStats,
        kCtxCursor,
        kCtxBook,
        kCtxDebugOverlay,
        kCtxJournal,
        kCtxTFC,
        kCtxMapDebug,
        kCtxLockpicking,
        kCtxMarketplace,
        kCtxFavor,
        kCtxCount,
        kCtxAny = -1
    };

    struct CtxInfo
    {
        const char* name;   // short, for the Context column
        const char* where;  // when the game uses it
    };

    inline constexpr CtxInfo kCtxInfo[kCtxCount] = {
        { N_("Gameplay"), N_("In the world, no menu open") },
        { N_("Menu"), N_("Any menu: Inventory, Magic, Container, Barter, Dialogue, Main menu, message boxes") },
        { N_("Console"), N_("Developer console (~)") },
        { N_("Item menus"), N_("Item lists: Inventory, Container, Barter, Gift") },
        { N_("Inventory"), N_("Inventory menu") },
        { N_("Debug text"), N_("Developer debug text overlay") },
        { N_("Favorites"), N_("Favorites menu (Q)") },
        { N_("Map"), N_("World map and local map") },
        { N_("Skills"), N_("Skills menu / perk constellations") },
        { N_("Cursor"), N_("Menus that show a mouse cursor") },
        { N_("Book"), N_("Reading a book or a note") },
        { N_("Debug overlay"), N_("Developer debug overlay") },
        { N_("Journal"), N_("Journal / system menu: quests, stats, saves, settings") },
        { N_("Free camera"), N_("Free-fly camera (console command tfc)") },
        { N_("Map debug"), N_("Developer world map debug mode") },
        { N_("Lockpicking"), N_("Lockpicking minigame") },
        { N_("Creation Club"), N_("Creation Club / Marketplace menu") },
        { N_("Follower cmd"), N_("Commanding a follower (look at a follower and hold Activate)") },
    };

    int         ContextCount();         // 18 on 1.6.1130+, 17 before it and on VR
    int         LogicalContext(int c);  // game index -> Ctx, kCtxAny if unknown
    std::string ContextName(int c);
    std::string ContextHint(int c);
    std::string VanillaOwner(int c);  // "Skyrim", "Skyrim - Creation Club" or "Skyrim - Debug"

    // ---------------------------------------------------------------- built-in notes (BuiltInNotes.cpp)
    // Data/SKSE/Plugins/HotkeyAtlas/Notes.json: what each Skyrim control and mod hotkey does.

    // English text, looked up in the translation files, or its own translations
    struct LocText
    {
        std::string                                      en;
        std::vector<std::pair<std::string, std::string>> tr;  // language (lower case) -> text
    };

    struct ControlNote
    {
        int         ctx;    // Ctx, or kCtxAny
        std::string event;  // NormEvent()
        LocText     text;
    };

    struct DeviceNote
    {
        Device      device;
        std::string event;  // NormEvent()
        LocText     text;
    };

    struct ModNote
    {
        std::string mod, file, section, setting, action;  // lower case; empty = any
        LocText     text;
    };

    struct BuiltInNotes
    {
        std::vector<ControlNote> controls;    // a context's own entries first, then kCtxAny
        std::vector<DeviceNote>  devices;
        std::vector<ControlNote> directions;  // ctx unused
        LocText                  quickSlot, quickSlotFavorites;
        std::vector<ModNote>     mods;
    };

    std::shared_ptr<const BuiltInNotes> BuiltIn();
    void                                LoadBuiltInNotes();  // keeps the notes read last when the file is missing or broken
    std::string                         NormEvent(std::string_view s);  // "Left Equip" == "LeftEquip" == "left-equip"
    std::string                         Resolve(const LocText& t, std::initializer_list<std::string_view> args = {});
    std::string                         DescribeControl(int c, std::string_view event, Device device);
    std::string                         DescribeModBinding(const Binding& b, const BuiltInNotes& notes);

    // ---------------------------------------------------------------- settings (Settings.cpp)
    // Skyrim controls changed through Hotkey Atlas live only in our own ini and are applied to
    // RE::ControlMap in memory; mod keys are remapped by the input hook. No other file is written.

    struct Override
    {
        std::uint32_t key;       // key we force
        std::uint32_t original;  // key the game had before the first override
    };
    using Overrides = std::map<std::string, Override>;

    extern std::mutex g_ovLock;       // guards the four maps below and g_preset
    extern Overrides  g_overrides;    // OverrideId() -> Skyrim control moved (applied in memory)
    extern Overrides  g_fileEdits;    // FileEditId() -> mod key remapped by the input hook
    extern Overrides  g_padControls;  // OverrideId(): gamepad button added to a key / mouse control
    extern Overrides  g_padMods;      // FileEditId(): gamepad button added to a mod key
    extern std::string g_preset;      // active preset

    // Every bind change made with Hotkey Atlas: what HotkeyAtlas.ini and a preset store.
    struct BindSet
    {
        Overrides controls;     // g_overrides
        Overrides files;        // g_fileEdits
        Overrides padControls;  // g_padControls
        Overrides padMods;      // g_padMods
    };

    extern const fs::path kPresetDir;
    fs::path              PresetPath(std::string_view name);
    bool                  WritePresetFile(std::string_view name, const std::string& binds, std::string& err);

    std::string OverrideId(int ctx, std::string_view action, std::uint32_t originalKey);
    std::string FileEditId(const Binding& b);
    void        RecordChange(Overrides& map, const std::string& id, std::uint32_t oldKey, std::uint32_t newKey);  // caller holds g_ovLock
    bool        ParseBindLine(const std::string& section, const std::string& lhs, const std::string& rhs, BindSet& out);
    std::string BindSetText(const Overrides& controls, const Overrides& files, const Overrides& padControls, const Overrides& padMods);
    bool        SaveConfigFile(std::string& err);
    void        SaveConfigQuiet();                          // in the background, errors go to the status line
    void        SaveConfigAsync(std::string okStatus = {});  // in the background, then rescans
    void        EnsureActivePreset();

    // ---------------------------------------------------------------- YAML hotkey files (YamlKeys.cpp)

    struct YamlEntry
    {
        std::string path;   // parent mappings, "a.b"; empty at the top
        std::string name;
        std::string value;  // as written, comment and quotes included
        std::size_t valueOffset = 0, valueLength = 0;  // byte range of the value in the file
        int         line        = -1;
    };
    std::vector<YamlEntry>       ParseYamlScalars(const std::string& text);  // every "name: value" line
    std::optional<std::uint32_t> YamlCode(long vk);                          // VK in the file -> input code
    std::optional<long>          YamlValue(std::uint32_t code);              // input code -> VK, -1 = unset
    bool                         IsYamlEditId(const std::string& id);        // a g_fileEdits entry written into its file
    bool                         WriteYamlKey(const std::string& id, std::uint32_t code, std::string& err);
    void                         SyncYamlFiles(const Overrides& before, const Overrides& after);  // files follow the change set

    // ---------------------------------------------------------------- SkyUI MCM menus (PapyrusMcm.cpp)
    // Plain SkyUI menus (not MCM Helper) keep their keys in script variables, saved with the game.
    // The menu's compiled script tells which variable each AddKeyMapOption shows; the values are
    // read from the running scripts.

    // One AddKeyMapOption(ST) call of a script, traced back to the script variable it shows.
    struct PexKeymap
    {
        std::string var;            // script variable (an auto property's is "::Name_var")
        int         index = -1;     // int array element; -1 = plain variable
        bool        all   = false;  // a loop over the array (keys[i]): every element
        bool        global = false; // the variable holds a GlobalVariable, the key is its value (Key.GetValueInt())
        std::string label;          // literal option text ("$P_KEYMAP"), empty = unknown
        std::string labelArray;     // string array holding the texts (loop form)
    };
    std::vector<PexKeymap> ReadPexKeymaps(const std::string& script);  // Scripts/<script>.pex, loose or in a BSA; cached

    // A registered MCM menu as its script holds it right now.
    struct McmMenu
    {
        std::string                                               name;     // as registered, may be "$..."
        std::string                                               plugin;   // its quest's plugin, "3BBB.esp"
        std::vector<std::string>                                  scripts;  // most derived first, without SkyUI's bases
        std::unordered_map<std::string, int>                      ints;     // lower-case variable -> value
        std::unordered_map<std::string, std::vector<int>>         intArrays;
        std::unordered_map<std::string, std::vector<std::string>> strArrays;
        std::unordered_map<std::string, int>                      globals;  // variable holding a GlobalVariable -> its value
    };
    std::vector<McmMenu> SnapshotMcmMenus();  // game thread

    // ---------------------------------------------------------------- translation (Translation.cpp)

    extern std::mutex  g_trLock;
    extern std::string g_language;  // from HotkeyAtlas.ini, guarded by g_trLock

    // ---------------------------------------------------------------- control map (ControlMap.cpp)

    using Mappings = RE::BSTArray<RE::ControlMap::UserEventMapping>;

    inline constexpr Device kDevices[] = { Device::Keyboard, Device::Mouse, Device::Gamepad };

    void                 SortByKey(Mappings& maps);
    std::uint16_t        PhysicalKey(std::uint32_t code, Device in, std::uint32_t original);
    inline std::uint16_t PhysicalKey(std::uint32_t code, Device in = Device::Keyboard) { return PhysicalKey(code, in, code); }
    Mappings*            DeviceMappings(RE::ControlMap* cm, int c, Device d);
    const Override*      AppliedOverride(int c, const RE::ControlMap::UserEventMapping& m, Device d = Device::Keyboard);  // caller holds g_ovLock
    void                 ApplyOverrides();                                                                                   // game thread
    std::vector<Binding> ReadControlMap();                                                                                   // game thread
    std::size_t          RestoreControls(std::optional<Device> only);                                                        // game thread

    // ---------------------------------------------------------------- input hook (InputHook.cpp)

    void RebuildComboTableLocked();  // caller holds g_ovLock
    // FileEditId() -> dlls reading that mod key (Binding::readers), from the latest scan. Caller holds g_ovLock.
    void SetRemapReadersLocked(std::map<std::string, std::vector<fs::path>> readers);
    void ClearActiveInputs();        // game thread
    void PauseTriggers(bool paused);  // a bind is being captured: no new double taps / holds, gamepad buttons reach no one; any thread

    // ---------------------------------------------------------------- input block while capturing (InputBlock.cpp)

    struct BlockedInput
    {
        Device        device;
        std::uint32_t id;  // DIK or MouseButton
        bool          down;
    };
    void                      StartInputBlock();  // keyboard, middle / side mouse buttons and wheel go to the capture only
    void                      StopInputBlock();
    bool                      InputBlockBusy();  // on, or keys it took still held: the input hook must look
    bool                      BlockInputEvent(Device d, std::uint32_t id, bool down, bool up, bool canHold);  // input hook: true = hide it
    std::vector<BlockedInput> TakeBlockedInput();  // presses and releases since the last call
    bool                      BlockedHeld(Device d, std::uint32_t id);
    bool                      AnyBlockedHeld();

    // Input state straight from Windows / XInput: works whichever UI has focus.
    struct PadState
    {
        std::uint32_t buttons  = 0;   // GamepadButton bits; LT in bit 16, RT in bit 17 (analog in XInput)
        float         stick[2] = {};  // how far the left / right stick is pushed, 0..1
    };

    std::uint8_t HeldModsOS();                // Shift / Ctrl / Alt held now
    PadState     ReadPads();                  // every connected pad merged
    bool         IsHeld(std::uint32_t hold);  // the held part of a combo is down right now

    inline bool IsWheel(std::uint32_t code) { return code == MakeCode(Device::Mouse, kMouseWheelUp) || code == MakeCode(Device::Mouse, kMouseWheelDown); }

    // ---------------------------------------------------------------- scanning (Scanner.cpp)

    void SetStatus(std::string s);

    // ---------------------------------------------------------------- old versions (LegacyEdits.cpp)

    bool RestoreFileValue(const Binding& b, std::uint32_t combo, std::string& err);
}
