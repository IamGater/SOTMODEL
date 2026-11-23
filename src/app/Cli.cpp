// Command line mode. Every Api tool is available and prints JSON:
//   SOTModel.exe --cli <tool> [primary-arg] [key=value ...]
//   SOTModel.exe --cli <tool> --json "{\"path\": \"...\"}"
//   SOTModel.exe --cli tools                     list tools with their arguments
// Images returned by preview tools are written to files (out=<file.png>, default %TEMP%).
// Developer commands: scan <substring> [max], native <path> [bytes], materials [substring], particles
#include "app/Cli.h"
#include "app/Api.h"
#include "core/Json.h"
#include "ue/Texture.h"
#include "ue/Mesh.h"
#include "ue/Animation.h"
#include "ue/MaterialGraph.h"
#include "ue/Material.h"
#include "ue/Particles.h"
#include "ue/Blueprint.h"
#include <set>
#include "render/GraphShader.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#include <windows.h>
#include <map>
#include <algorithm>
#include <functional>

static void Out(const std::string& s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), (DWORD)s.size(), &w, nullptr);
}

static int DevScan(Api& api, const std::string& arg, size_t max) {
    std::string needle = ToLower(arg);
    std::map<std::string, std::pair<int, int>> perClass;   // class -> (ok, property errors)
    std::map<std::string, int> formats;
    std::map<std::string, std::pair<int, std::string>> undecoded;   // raw property type -> (count, first example)
    size_t total = 0, readFail = 0, loadFail = 0;
    Vfs& vfs = api.vfs;
    for (u32 i = 0; i < vfs.FileCount() && total < max; i++) {
        std::string p = ToLower(vfs.Path(i));
        if (!(IEndsWith(p, ".uasset") || IEndsWith(p, ".umap"))) continue;
        if (!needle.empty() && p.find(needle) == std::string::npos) continue;
        total++;
        Bytes data;
        std::string err;
        if (!vfs.Read(i, data, &err)) { readFail++; Out("READ " + p + ": " + err + "\n"); continue; }
        Package pkg;
        if (!pkg.Load(std::move(data), std::string(vfs.Path(i)), &err)) { loadFail++; Out("LOAD " + p + ": " + err + "\n"); continue; }
        for (size_t e = 0; e < pkg.Exports().size(); e++) {
            std::string perr;
            PropertyList props = pkg.ReadProperties(e, nullptr, &perr);
            std::string cls = pkg.ExportClass(e);
            std::function<void(const PropValue&, const std::string&)> walk = [&](const PropValue& v, const std::string& name) {
                if (v.type == PropType::Raw) {
                    auto& u = undecoded[v.str];
                    if (u.first++ == 0) u.second = p + " " + cls + "." + name;
                }
                for (auto& it : v.items) walk(it, name);
                if (v.fields) for (auto& f : *v.fields) walk(f.value, name + "." + f.name);
            };
            for (auto& pr : props) walk(pr.value, pr.name);
            auto& c = perClass[cls];
            if (perr.empty()) c.first++;
            else if (c.second++ < 3) Out("PROPS " + p + " [" + pkg.Exports()[e].objectName + "]: " + perr + "\n");
            if (IsStaticMeshClass(cls) || IsSkeletalMeshClass(cls)) {
                MeshData md;
                std::string merr;
                if (ReadMesh(pkg, e, md, &merr)) formats[cls + " ok"]++;
                else if (++formats[cls + " <error>"] <= 8) Out("MESH " + p + ": " + merr + "\n");
            }
            if (IsAnimSequenceClass(cls)) {
                AnimSequenceData ad;
                std::string aerr;
                if (ReadAnimSequence(pkg, e, ad, &aerr)) formats["AnimSequence ok " + ad.encoding + (ad.sequentialOffsets ? " (sequential)" : "")]++;
                else if (++formats["AnimSequence <error>"] <= 12) Out("ANIM " + p + ": " + aerr + "\n");
            }
            if (IsTextureClass(cls)) {
                TextureInfo ti;
                std::string terr;
                if (ReadTextureInfo(pkg, e, ti, &terr)) {
                    if (formats[ti.pixelFormat]++ < 3 && ti.pixelFormat.find("BC6H") != std::string::npos) Out(ti.pixelFormat + " " + p + "\n");
                }
                else if (++formats["<error>"] <= 5) Out("TEXTURE " + p + ": " + terr + "\n");
            }
        }
    }
    Out(Fmt("\npackages %zu, read failures %zu, load failures %zu\n", total, readFail, loadFail));
    for (auto& [k, v] : perClass) Out(Fmt("%-48s ok %6d  errors %d\n", k.c_str(), v.first, v.second));
    for (auto& [k, v] : formats) Out(Fmt("texture format %-20s %d\n", k.c_str(), v));
    for (auto& [k, v] : undecoded) Out(Fmt("undecoded %-32s %6d  e.g. %s\n", k.c_str(), v.first, v.second.c_str()));
    return 0;
}

// Every Material / MaterialFunction under paths containing `arg`: node types the HLSL translator leaves at 0.
static int DevMaterials(Api& api, const std::string& arg) {
    std::string needle = ToLower(arg);
    std::map<std::string, std::pair<int, std::string>> missing;   // node type -> (materials, first example)
    std::map<std::string, std::pair<int, std::string>> compileErrors;   // fxc error -> (materials, first example)
    size_t total = 0, failed = 0;
    Vfs& vfs = api.vfs;
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string p(vfs.Path(i));
        if (!IEndsWith(p, ".uasset") || (!needle.empty() && ToLower(p).find(needle) == std::string::npos)) continue;
        auto pkg = api.cache->LoadPath(p);
        if (!pkg || !pkg->MainExport()) continue;
        std::string cls = pkg->ExportClass((size_t)pkg->MainExport() - 1);
        if (cls != "Material" && cls != "MaterialFunction") continue;
        total++;
        std::string hlsl, err;
        MaterialGraphStats st;
        if (!MaterialToHlsl(*api.cache, p, hlsl, &st, &err)) { failed++; Out("FAIL " + p + ": " + err + "\n"); continue; }
        for (auto& u : st.unsupported) { auto& m = missing[u]; if (m.first++ == 0) m.second = p; }
        // the viewer's preview shader built from it must compile
        MaterialInfo mi;
        GraphShaderSource src;
        if (cls == "Material" && ReadMaterial(*api.cache, p, mi, &err) && BuildGraphShaderSource(hlsl, mi.blendMode, mi.shadingModel, src, &err)) {
            ComPtr<ID3DBlob> code, msgs;
            if (FAILED(D3DCompile(src.hlsl.data(), src.hlsl.size(), nullptr, nullptr, nullptr, "GraphPSMain", "ps_5_0", 0, 0, &code, &msgs))) {
                std::string m = msgs ? std::string((const char*)msgs->GetBufferPointer(), msgs->GetBufferSize()) : "?";
                size_t k = m.find("error X");
                m = k == std::string::npos ? m.substr(0, 120) : m.substr(k, m.find('\n', k) - k);
                auto& c = compileErrors[m];
                if (c.first++ == 0) c.second = p;
            }
        }
    }
    Out(Fmt("\nmaterials %zu, failed %zu\n", total, failed));
    for (auto& [k, v] : missing) Out(Fmt("untranslated %-40s %5d  e.g. %s\n", k.c_str(), v.first, v.second.c_str()));
    for (auto& [k, v] : compileErrors) Out(Fmt("preview shader %5d  %s  e.g. %s\n", v.first, k.c_str(), v.second.c_str()));
    return 0;
}

// Every ParticleSystem: modules (enabled, in some emitter LOD) the preview simulation does not understand.
static int DevParticles(Api& api) {
    std::map<std::string, std::pair<int, std::string>> modules;   // class -> (systems, first example)
    std::map<std::string, std::string> notes;                     // class -> why the preview cannot show it
    size_t total = 0;
    Vfs& vfs = api.vfs;
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string p(vfs.Path(i));
        if (!IEndsWith(p, ".uasset") || ToLower(p).find("particle") == std::string::npos) continue;
        auto pkg = api.cache->LoadPath(p);
        if (!pkg || !pkg->MainExport() || pkg->ExportClass((size_t)pkg->MainExport() - 1) != "ParticleSystem") continue;
        total++;
        ParticleSystemInfo ps;
        if (!ReadParticleSystem(*api.cache, *pkg, (size_t)pkg->MainExport() - 1, ps)) { Out("FAIL " + p + "\n"); continue; }
        std::set<std::string> seen;
        for (auto& e : ps.emitters)
            for (auto& m : e.modules)
                if (!m.simulated && m.enabled) { seen.insert(m.cls); if (m.previewNote) notes[m.cls] = m.previewNote; }
        for (auto& c : seen) { auto& m = modules[c]; if (m.first++ == 0) m.second = p; }
    }
    Out(Fmt("\nparticle systems %zu\n", total));
    for (auto& [k, v] : modules) {
        bool np = notes.count(k) > 0;
        Out(Fmt("%s %-48s %5d  e.g. %s\n", np ? "not previewable" : "not simulated  ", k.c_str(), v.first, v.second.c_str()));
        if (np) Out("    (" + notes[k] + ")\n");
    }
    return 0;
}

// Every Blueprint package: decompile failures and where function bodies stop, grouped by reason.
static int DevBlueprints(Api& api, const std::string& filter) {
    std::map<std::string, std::pair<int, std::string>> stops, fails, flat;   // reason -> (count, first example)
    size_t total = 0, funcs = 0, stopped = 0;
    BlueprintStats stats;
    Vfs& vfs = api.vfs;
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string p(vfs.Path(i));
        if (!IEndsWith(p, ".uasset") || (!filter.empty() && ToLower(p).find(ToLower(filter)) == std::string::npos)) continue;
        auto pkg = api.cache->LoadPath(p);
        if (!pkg || !IsBlueprintPackage(*pkg)) continue;
        total++;
        std::string code, err;
        BlueprintOptions bo;
        bo.cache = api.cache.get();
        bo.stats = &stats;
        if (!DecompileBlueprint(*pkg, code, &err, bo)) { auto& f = fails[err]; if (f.first++ == 0) f.second = p; continue; }
        for (size_t k = 0; (k = code.find("\n{\n", k)) != std::string::npos; k += 3) funcs++;
        for (size_t k = 0; (k = code.find("// control flow kept as jumps (", k)) != std::string::npos; k++) {
            size_t e = code.find(')', k + 31);
            auto& s = flat[code.substr(k + 31, e - k - 31)];
            if (s.first++ == 0) s.second = p;
        }
        for (size_t k = 0; (k = code.find("// event graph kept whole (", k)) != std::string::npos; k++) {
            size_t e = code.find(")\n", k);
            if (e == std::string::npos) break;
            auto& s = flat["event graph: " + code.substr(k + 27, e - k - 27)];
            if (s.first++ == 0) s.second = p;
        }
        for (size_t k = 0; (k = code.find("// decompilation stopped: ", k)) != std::string::npos; k++) {
            size_t e = code.find('\n', k);
            std::string why = code.substr(k + 26, e - k - 26);
            size_t at = why.find(" at ");
            if (at != std::string::npos) why.resize(at);   // group by opcode, not offset
            auto& s = stops[why];
            if (s.first++ == 0) s.second = p;
            stopped++;
        }
    }
    Out(Fmt("\nblueprints %zu, failed %zu, function bodies ~%zu, stopped %zu\n", total, (size_t)fails.size(), funcs, stopped));
    for (auto& [k, v] : fails) Out(Fmt("fail %-60s %5d  e.g. %s\n", k.c_str(), v.first, v.second.c_str()));
    for (auto& [k, v] : stops) Out(Fmt("stop %-60s %5d  e.g. %s\n", k.c_str(), v.first, v.second.c_str()));
    for (auto& [k, v] : flat) Out(Fmt("jumps %-59s %5d  e.g. %s\n", k.c_str(), v.first, v.second.c_str()));
    Out(Fmt("calls %zu, with a signature %zu (%.1f%%)\n", stats.calls, stats.callsWithSignature, stats.calls ? 100.0 * stats.callsWithSignature / stats.calls : 0.0));
    std::vector<std::pair<int, std::string>> top;
    for (auto& [k, v] : stats.unresolved) top.push_back({v, k});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 40; i++) Out(Fmt("no signature %6d  %s\n", top[i].first, top[i].second.c_str()));
    return 0;
}

static int DevNative(Api& api, const std::string& path, size_t n) {
    std::string err;
    auto pkg = api.LoadPackage(path, &err);
    if (!pkg) { Out(err + "\n"); return 3; }
    for (size_t e = 0; e < pkg->Exports().size(); e++) {
        size_t end = 0;
        pkg->ReadProperties(e, &end);
        const auto& ex = pkg->Exports()[e];
        size_t stop = (size_t)ex.serialOffset + ex.serialSize;
        Out(Fmt("[%zu] %s (%s) props end %zu, native %zu bytes\n", e, ex.objectName.c_str(), pkg->ExportClass(e).c_str(), end, stop - end));
        for (size_t p = end; p < std::min(stop, end + n); p += 16) {
            std::string line = Fmt("  %08zx: ", p);
            for (size_t k = p; k < std::min(stop, p + 16); k++) line += Fmt("%02x ", pkg->Data()[k]);
            Out(line + "\n");
        }
    }
    return 0;
}

int RunCli(int argc, wchar_t** argv) {
    if (argc < 3) { Out("usage: SOTModel.exe --cli <tool> [arg] [key=value ...]   (--cli tools lists them)\n"); return 1; }
    std::string tool = Narrow(argv[2]);
    Settings settings;
    settings.Load();
    Api api(settings);

    if (tool == "tools") {
        JsonWriter w;
        w.BeginArray();
        for (auto& t : api.Tools()) {
            w.BeginObject();
            w.KV("name", t.name);
            w.KV("description", t.description);
            if (!t.primaryArg.empty()) w.KV("primaryArgument", t.primaryArg);
            w.Key("arguments");
            JsonValue schema;
            JsonValue::Parse(t.schema, schema);
            w.BeginObject();
            if (auto props = schema.Get("properties"))
                for (auto& [k, v] : props->obj) w.KV(k, v.GetStr("type") + (v.Get("default") ? " = " + v.Get("default")->Dump() : ""));
            w.EndObject();
            w.EndObject();
        }
        w.EndArray();
        Out(w.Take() + "\n");
        return 0;
    }

    std::string err;
    if (!api.EnsureMounted(&err)) { Out("{\"error\": \"" + err + "\"}\n"); return 2; }
    if (tool == "scan") return DevScan(api, argc > 3 ? Narrow(argv[3]) : "", argc > 4 ? (size_t)_wtoi(argv[4]) : SIZE_MAX);
    if (tool == "materials") return DevMaterials(api, argc > 3 ? Narrow(argv[3]) : "/materials/");
    if (tool == "particles") return DevParticles(api);
    if (tool == "blueprints") return DevBlueprints(api, argc > 3 ? Narrow(argv[3]) : "");
    if (tool == "native") return DevNative(api, argc > 3 ? Narrow(argv[3]) : "", argc > 4 ? (size_t)_wtoi(argv[4]) : 128);

    const ApiTool* t = api.FindTool(tool);
    if (!t) { Out("{\"error\": \"unknown tool '" + tool + "' (--cli tools lists them)\"}\n"); return 1; }
    JsonValue args;
    args.type = JsonValue::Type::Object;
    std::wstring imageOut;
    for (int i = 3; i < argc; i++) {
        std::string a = Narrow(argv[i]);
        if (a == "--json" && i + 1 < argc) {
            if (!JsonValue::Parse(Narrow(argv[++i]), args, &err)) { Out("{\"error\": \"bad --json: " + err + "\"}\n"); return 1; }
            continue;
        }
        size_t eq = a.find('=');
        std::string key, value;
        if (eq != std::string::npos && eq > 0 && a.find('/') > eq) { key = a.substr(0, eq); value = a.substr(eq + 1); }
        else { key = t->primaryArg; value = a; }
        if (key == "out") { imageOut = Widen(value); continue; }
        JsonValue v;
        if ((value.size() > 1 && (value[0] == '[' || value[0] == '{')) && JsonValue::Parse(value, v, nullptr)) {}   // key=[1,2,3]
        else if (value == "true" || value == "false") { v.type = JsonValue::Type::Bool; v.b = value == "true"; }
        else if (!value.empty() && value.find_first_not_of("0123456789.-") == std::string::npos && value != "-") { v.type = JsonValue::Type::Number; v.num = atof(value.c_str()); }
        else { v.type = JsonValue::Type::String; v.str = value; }
        args.obj.emplace_back(key, std::move(v));
    }
    ApiResult r = api.Call(tool, args);
    Out(r.json + "\n");
    for (size_t i = 0; i < r.images.size(); i++) {
        std::wstring path = imageOut;
        if (path.empty()) {
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            path = std::wstring(tmp) + L"sotmodel_" + Widen(tool) + L".png";
        } else if (i) path = path.substr(0, path.rfind(L'.')) + L"_" + std::to_wstring(i) + L".png";
        WriteWholeFile(path, r.images[i].data.data(), r.images[i].data.size());
        JsonWriter w;
        w.BeginObject();
        w.KV("image", Narrow(path));
        w.EndObject();
        Out(w.Take() + "\n");
    }
    return r.ok ? 0 : 1;
}
