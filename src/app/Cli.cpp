// Command line mode. Every Api tool is available and prints JSON:
//   SOTModel.exe --cli <tool> [primary-arg] [key=value ...]
//   SOTModel.exe --cli <tool> --json "{\"path\": \"...\"}"
//   SOTModel.exe --cli tools                     list tools with their arguments
// Images returned by preview tools are written to files (out=<file.png>, default %TEMP%).
// Developer commands: scan <substring> [max], native <path> [bytes]
#include "app/Cli.h"
#include "app/Api.h"
#include "core/Json.h"
#include "ue/Texture.h"
#include "ue/Mesh.h"
#include <windows.h>
#include <map>

static void Out(const std::string& s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), (DWORD)s.size(), &w, nullptr);
}

static int DevScan(Api& api, const std::string& arg, size_t max) {
    std::string needle = ToLower(arg);
    std::map<std::string, std::pair<int, int>> perClass;   // class -> (ok, property errors)
    std::map<std::string, int> formats;
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
            pkg.ReadProperties(e, nullptr, &perr);
            std::string cls = pkg.ExportClass(e);
            auto& c = perClass[cls];
            if (perr.empty()) c.first++;
            else if (c.second++ < 3) Out("PROPS " + p + " [" + pkg.Exports()[e].objectName + "]: " + perr + "\n");
            if (IsStaticMeshClass(cls) || IsSkeletalMeshClass(cls)) {
                MeshData md;
                std::string merr;
                if (ReadMesh(pkg, e, md, &merr)) formats[cls + " ok"]++;
                else if (++formats[cls + " <error>"] <= 8) Out("MESH " + p + ": " + merr + "\n");
            }
            if (IsTextureClass(cls)) {
                TextureInfo ti;
                std::string terr;
                if (ReadTextureInfo(pkg, e, ti, &terr)) formats[ti.pixelFormat]++;
                else if (++formats["<error>"] <= 5) Out("TEXTURE " + p + ": " + terr + "\n");
            }
        }
    }
    Out(Fmt("\npackages %zu, read failures %zu, load failures %zu\n", total, readFail, loadFail));
    for (auto& [k, v] : perClass) Out(Fmt("%-48s ok %6d  errors %d\n", k.c_str(), v.first, v.second));
    for (auto& [k, v] : formats) Out(Fmt("texture format %-20s %d\n", k.c_str(), v));
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
        if (value == "true" || value == "false") { v.type = JsonValue::Type::Bool; v.b = value == "true"; }
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
