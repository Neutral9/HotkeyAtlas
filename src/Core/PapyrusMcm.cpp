// SkyUI MCM menus: which script variables their keymap options show (read from the compiled
// .pex), and the menus registered in the running game with those variables' values.

#include "Internal.h"

#undef GetObject  // windows.h: Variable::GetObject would become GetObjectA

namespace HA
{
    namespace
    {
        // ---------------------------------------------------------------- .pex reading
        // Skyrim's compiled Papyrus, big-endian. Layout: github.com/Orvid/Champollion (PexReader).

        struct Arg
        {
            enum class T : std::uint8_t
            {
                None,
                Ident,
                String,
                Int,
                Float,
                Bool
            } t = T::None;
            std::uint16_t s = 0;  // Ident / String: string table index
            std::int32_t  i = 0;  // Int
        };

        struct Instr
        {
            std::uint8_t     op = 0;
            std::vector<Arg> args;  // fixed arguments, then the variadic ones (the count left out)
        };

        struct Func
        {
            std::vector<Instr> code;
        };

        struct Property
        {
            std::uint16_t       autoVar = 0xFFFF;
            std::optional<Func> getter;
        };

        enum Op : std::uint8_t
        {
            kAssign      = 0x0D,
            kCast        = 0x0E,
            kCallMethod  = 0x17,
            kCallParent  = 0x18,
            kCallStatic  = 0x19,
            kReturn      = 0x1A,
            kPropGet     = 0x1C,
            kArrayGetElt = 0x20,
        };

        // fixed argument count of each Skyrim opcode (0x00 nop .. 0x23 array_rfindelement)
        constexpr std::uint8_t kArgCount[] = { 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3,
            1, 2, 2, 3, 2, 3, 1, 3, 3, 3, 2, 2, 3, 3, 4, 4 };

        class PexReader
        {
        public:
            explicit PexReader(const std::string& bytes) : _b(bytes) {}

            std::uint8_t  U8() { return static_cast<std::uint8_t>(Need(1)[0]); }
            std::uint16_t U16()
            {
                const auto* p = Need(2);
                return static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[0]) << 8 | static_cast<std::uint8_t>(p[1]));
            }
            std::uint32_t U32()
            {
                const std::uint32_t hi = U16();
                return hi << 16 | U16();
            }
            void        Skip(std::size_t n) { Need(n); }
            std::string WStr()
            {
                const auto n = U16();
                return std::string(Need(n), n);
            }
            Arg Data()
            {
                Arg a;
                switch (U8()) {
                case 0: a.t = Arg::T::None; break;
                case 1: a.t = Arg::T::Ident, a.s = U16(); break;
                case 2: a.t = Arg::T::String, a.s = U16(); break;
                case 3: a.t = Arg::T::Int, a.i = static_cast<std::int32_t>(U32()); break;
                case 4: a.t = Arg::T::Float, Skip(4); break;
                case 5: a.t = Arg::T::Bool, a.i = U8(); break;
                default: throw std::runtime_error("bad pex value");
                }
                return a;
            }
            Func Function()
            {
                Skip(2 + 2 + 4 + 1);           // return type, doc string, user flags, flags
                Skip(4u * U16());              // parameters: name, type
                Skip(4u * U16());              // locals: name, type
                Func f;
                const auto count = U16();
                f.code.reserve(count);
                for (std::uint16_t i = 0; i < count; ++i) {
                    Instr in;
                    in.op = U8();
                    if (in.op >= std::size(kArgCount)) throw std::runtime_error("bad pex opcode");
                    for (int a = 0; a < kArgCount[in.op]; ++a) in.args.push_back(Data());
                    if (in.op == kCallMethod || in.op == kCallParent || in.op == kCallStatic) {
                        const auto n = Data();
                        for (std::int32_t a = 0; a < n.i; ++a) in.args.push_back(Data());
                    }
                    f.code.push_back(std::move(in));
                }
                return f;
            }

        private:
            const char* Need(std::size_t n)
            {
                if (_pos + n > _b.size()) throw std::runtime_error("pex truncated");
                const auto* p = _b.data() + _pos;
                _pos += n;
                return p;
            }

            const std::string& _b;
            std::size_t        _pos = 0;
        };

        // One script, as far as keymap options need it.
        struct Script
        {
            std::vector<std::string>                  strings;  // string table
            std::set<std::string>                     vars;     // lower case
            std::unordered_map<std::string, Property> props;    // lower case
            std::vector<Func>                         funcs;    // every state's
        };

        Script ParsePex(const std::string& bytes)
        {
            PexReader r(bytes);
            if (r.U32() != 0xFA57C0DE) throw std::runtime_error("not a pex file");
            r.Skip(1 + 1 + 2 + 8);  // version, game id, compile time
            r.WStr(), r.WStr(), r.WStr();  // source, user, machine

            Script s;
            const auto count = r.U16();
            s.strings.reserve(count);
            for (std::uint16_t i = 0; i < count; ++i) s.strings.push_back(r.WStr());
            const auto str = [&](std::uint16_t i) { return i < s.strings.size() ? Lower(s.strings[i]) : std::string(); };

            if (r.U8()) {  // debug info
                r.Skip(8);
                for (auto n = r.U16(); n; --n) {
                    r.Skip(2 + 2 + 2 + 1);
                    r.Skip(2u * r.U16());
                }
            }
            for (auto n = r.U16(); n; --n) r.Skip(2 + 1);  // user flags

            for (auto objects = r.U16(); objects; --objects) {
                r.Skip(2 + 4);          // name, size
                r.Skip(2 + 2 + 4 + 2);  // parent, doc string, user flags, auto state
                for (auto n = r.U16(); n; --n) {
                    s.vars.insert(str(r.U16()));
                    r.Skip(2 + 4);  // type, user flags
                    r.Data();       // initial value
                }
                for (auto n = r.U16(); n; --n) {
                    const auto name = str(r.U16());
                    r.Skip(2 + 2 + 4);  // type, doc string, user flags
                    const auto flags = r.U8();
                    Property   p;
                    if (flags & 4) {
                        p.autoVar = r.U16();
                    } else {
                        if (flags & 1) p.getter = r.Function();
                        if (flags & 2) r.Function();
                    }
                    s.props[name] = std::move(p);
                }
                for (auto states = r.U16(); states; --states) {
                    r.U16();  // state name
                    for (auto n = r.U16(); n; --n) {
                        r.U16();  // function name
                        s.funcs.push_back(r.Function());
                    }
                }
            }
            return s;
        }

        // ---------------------------------------------------------------- tracing an argument

        struct Source
        {
            std::string var;  // empty: not a script variable (a constant, a function result...)
            int         index = -1;
            bool        all    = false;
            bool        global = false;  // var holds a GlobalVariable whose value this is
            std::string text;            // string literal
        };

        // The instruction argument a write goes to, if `in` writes a variable.
        const Arg* Dest(const Instr& in)
        {
            switch (in.op) {
            case 0x14: case 0x15: case 0x16: case kReturn: case 0x1D: case 0x21: return nullptr;  // jumps, return, propset, setelement
            case kCallMethod: case kCallStatic: case kPropGet: return in.args.size() > 2 ? &in.args[2] : nullptr;
            case kCallParent: return in.args.size() > 1 ? &in.args[1] : nullptr;
            case 0x22: case 0x23: return &in.args[1];  // array_(r)findelement: array, dest, value, start
            default: return in.args.empty() ? nullptr : &in.args[0];
            }
        }

        class Tracer
        {
        public:
            explicit Tracer(const Script& s) : _s(s) {}

            // What `a`, used by instruction `at` of `f`, holds: followed back through temporaries,
            // locals, self's properties and array reads.
            Source Trace(const Func& f, std::size_t at, const Arg& a, int depth = 0) const
            {
                Source out;
                if (depth > 8) return out;
                if (a.t == Arg::T::String) {
                    out.text = Str(a.s);
                    return out;
                }
                if (a.t != Arg::T::Ident) return out;
                const auto name = Lower(Str(a.s));
                if (_s.vars.contains(name)) {
                    out.var = Str(a.s);
                    return out;
                }
                for (std::size_t j = at; j-- > 0;) {
                    const auto& in = f.code[j];
                    const auto* d  = Dest(in);
                    if (!d || d->t != Arg::T::Ident || Lower(Str(d->s)) != name) continue;
                    switch (in.op) {
                    case kAssign:
                    case kCast:
                        return Trace(f, j, in.args[1], depth + 1);
                    case kPropGet:
                        if (in.args[1].t != Arg::T::Ident || Lower(Str(in.args[1].s)) != "self") return out;
                        return Prop(Lower(Str(in.args[0].s)), depth);
                    case kCallMethod: {
                        // Key.GetValueInt(): the key kept in a GlobalVariable (Follower Live Package)
                        if (in.args[0].t != Arg::T::Ident) return out;
                        const auto method = Lower(Str(in.args[0].s));
                        if (method != "getvalueint" && method != "getvalue") return out;
                        auto obj = Trace(f, j, in.args[1], depth + 1);
                        if (obj.var.empty() || obj.index >= 0 || obj.all || obj.global) return out;
                        obj.global = true;
                        return obj;
                    }
                    case kArrayGetElt: {
                        auto arr = Trace(f, j, in.args[1], depth + 1);
                        if (arr.var.empty() || arr.index >= 0 || arr.all || arr.global) return out;
                        if (in.args[2].t == Arg::T::Int)
                            arr.index = in.args[2].i;
                        else
                            arr.all = true;  // keys[i] in a loop
                        return arr;
                    }
                    default:
                        return out;  // computed
                    }
                }
                return out;  // a parameter
            }

            std::string Str(std::uint16_t i) const { return i < _s.strings.size() ? _s.strings[i] : std::string(); }

        private:
            // self.Prop: an auto property's variable, or the variable a plain getter returns
            Source Prop(const std::string& name, int depth) const
            {
                const auto it = _s.props.find(name);
                if (it == _s.props.end()) return {};
                if (it->second.autoVar != 0xFFFF) {
                    Source out;
                    out.var = Str(it->second.autoVar);
                    return out;
                }
                if (const auto& g = it->second.getter)
                    for (std::size_t j = 0; j < g->code.size(); ++j)
                        if (g->code[j].op == kReturn && !g->code[j].args.empty()) return Trace(*g, j, g->code[j].args[0], depth + 1);
                return {};
            }

            const Script& _s;
        };

        std::vector<PexKeymap> FindKeymaps(const Script& s)
        {
            std::vector<PexKeymap> out;
            const Tracer           t(s);
            for (const auto& f : s.funcs) {
                for (std::size_t i = 0; i < f.code.size(); ++i) {
                    const auto& in = f.code[i];
                    if (in.op != kCallMethod || in.args.size() < 3 || in.args[0].t != Arg::T::Ident) continue;
                    const auto method = Lower(t.Str(in.args[0].s));
                    const bool st     = method == "addkeymapoptionst";
                    if (!st && method != "addkeymapoption") continue;
                    // AddKeyMapOption(text, keyCode, flags), AddKeyMapOptionST(state, text, keyCode, flags)
                    const std::size_t text = st ? 4 : 3;
                    if (in.args.size() <= text + 1) continue;
                    const auto key = t.Trace(f, i, in.args[text + 1]);
                    if (key.var.empty()) continue;  // a constant or computed key: nothing to read
                    const auto label = t.Trace(f, i, in.args[text]);

                    PexKeymap k;
                    k.var   = key.var;
                    k.index = key.index;
                    k.all    = key.all;
                    k.global = key.global;
                    if (!label.text.empty())
                        k.label = label.text;
                    else if (label.all && key.all)
                        k.labelArray = label.var;  // names[i] next to keys[i]
                    const auto same = [&](const PexKeymap& o) {
                        return Lower(o.var) == Lower(k.var) && o.index == k.index && o.all == k.all && o.global == k.global;
                    };
                    if (std::ranges::none_of(out, same)) out.push_back(std::move(k));
                }
            }
            return out;
        }

        // Scripts/<script>.pex through the game's file system: loose files and BSAs alike.
        std::optional<std::string> ReadScriptFile(const std::string& script)
        {
            RE::BSResourceNiBinaryStream in("Scripts\\" + script + ".pex");
            if (!in.good()) return std::nullopt;
            const auto size = in.stream->totalSize;
            if (!size || size > 8u * 1024 * 1024) return std::nullopt;
            std::string bytes(size, '\0');
            if (!in.read(bytes.data(), size)) return std::nullopt;
            return bytes;
        }
    }

    std::vector<PexKeymap> ReadPexKeymaps(const std::string& script)
    {
        static std::mutex                                     lock;
        static std::map<std::string, std::vector<PexKeymap>> cache;  // scripts don't change while the game runs
        const auto                                            id = Lower(script);
        {
            std::lock_guard l(lock);
            if (const auto it = cache.find(id); it != cache.end()) return it->second;
        }
        std::vector<PexKeymap> found;
        try {
            if (const auto bytes = ReadScriptFile(script)) found = FindKeymaps(ParsePex(*bytes));
            else logger::info("mcm: Scripts/{}.pex not found", script);
        } catch (const std::exception& e) {
            logger::warn("mcm: Scripts/{}.pex: {}", script, e.what());
        }
        std::lock_guard l(lock);
        cache[id] = found;
        return found;
    }

    namespace
    {
        // SkyUI's menu registry: the SKI_ConfigManager script on one of its quests.
        RE::BSTSmartPointer<RE::BSScript::Object> FindConfigManager(RE::BSScript::IVirtualMachine* vm, RE::BSScript::IObjectHandlePolicy* policy)
        {
            static RE::VMHandle                       cached = 0;
            RE::BSTSmartPointer<RE::BSScript::Object> obj;
            if (cached && vm->FindBoundObject(cached, "SKI_ConfigManager", obj) && obj) return obj;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return nullptr;
            for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
                if (!quest) continue;
                const auto handle = policy->GetHandleForObject(RE::TESQuest::FORMTYPE, quest);
                if (handle == policy->EmptyHandle()) continue;
                if (vm->FindBoundObject(handle, "SKI_ConfigManager", obj) && obj) {
                    cached = handle;
                    return obj;
                }
            }
            return nullptr;
        }

        void ReadValue(const RE::BSScript::Variable& v, const std::string& name, McmMenu& m, RE::BSScript::IObjectHandlePolicy* policy)
        {
            if (v.IsInt()) {
                m.ints[name] = v.GetSInt();
            } else if (v.IsObject()) {
                const auto obj = v.GetObject();
                if (!obj) return;
                if (auto* form = policy->GetObjectForHandle(RE::TESGlobal::FORMTYPE, obj->GetHandle()))
                    if (const auto* global = form->As<RE::TESGlobal>()) m.globals[name] = static_cast<int>(global->value);
            } else if (v.IsLiteralArray()) {
                const auto arr = v.GetArray();
                if (!arr || arr->size() == 0) return;  // not empty(): CommonLib's returns size() > 0
                if ((*arr)[0].IsInt()) {
                    auto& out = m.intArrays[name];
                    for (const auto& e : *arr) out.push_back(e.IsInt() ? e.GetSInt() : -1);
                } else if ((*arr)[0].IsString()) {
                    auto& out = m.strArrays[name];
                    for (const auto& e : *arr) out.emplace_back(e.IsString() ? e.GetString() : std::string_view{});
                }
            }
        }
    }

    std::vector<McmMenu> SnapshotMcmMenus()
    {
        std::vector<McmMenu> out;
        auto*                vm     = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        auto*                policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
        if (!policy) return out;
        const auto manager = FindConfigManager(vm, policy);
        if (!manager) return out;
        const auto* configsVar = manager->GetVariable("_modConfigs");
        const auto* namesVar   = manager->GetVariable("_modNames");
        if (!configsVar || !namesVar || !configsVar->IsArray() || !namesVar->IsArray()) return out;
        const auto configs = configsVar->GetArray();
        const auto names   = namesVar->GetArray();
        if (!configs || !names) return out;

        for (std::uint32_t i = 0; i < configs->size() && i < names->size(); ++i) {
            const auto obj = (*configs)[i].IsObject() ? (*configs)[i].GetObject() : nullptr;
            if (!obj || !(*names)[i].IsString()) continue;
            McmMenu m;
            m.name = std::string((*names)[i].GetString());
            if (m.name.empty()) continue;
            if (auto* form = policy->GetObjectForHandle(RE::TESQuest::FORMTYPE, obj->GetHandle()))
                if (const auto* file = form->GetFile(0)) m.plugin = std::string(file->GetFilename());

            for (auto* cls = obj->GetTypeInfo(); cls; cls = cls->GetParent()) {
                const std::string name = cls->GetName();
                const auto        l    = Lower(name);
                if (l == "ski_configbase" || l == "ski_questbase" || l == "mcm_configbase" || l == "quest" || l == "form") break;
                m.scripts.push_back(name);
                const auto* vars = cls->GetVariableIter();
                for (std::uint32_t v = 0; vars && v < cls->GetNumVariables(); ++v)
                    if (const auto* value = obj->GetVariable(vars[v].name)) ReadValue(*value, Lower(vars[v].name.c_str()), m, policy);
            }
            if (!m.scripts.empty()) out.push_back(std::move(m));
        }
        return out;
    }
}
