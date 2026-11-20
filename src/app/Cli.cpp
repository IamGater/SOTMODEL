// Command line mode (used for scripting and for testing the parsers on the whole game):
//   SOTModel.exe --cli list <substring>
//   SOTModel.exe --cli json <game path>
//   SOTModel.exe --cli scan <substring> [max]     parse every matching package and report failures
//   SOTModel.exe --cli export <game path> [out dir]
#include "app/Cli.h"
#include "app/Settings.h"
#include "core/Vfs.h"
#include "ue/Package.h"
#include "ue/AssetJson.h"
#include "export/Exporter.h"
#include "ue/Texture.h"
#include <windows.h>
#include <map>

static void Out(const std::string& s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), (DWORD)s.size(), &w, nullptr);
}

static bool MountFromSettings(Settings& st, Vfs& vfs) {
    st.Load();
    if (st.gameDir.empty() || st.aesKeys.empty() || st.oodlePath.empty()) st.ImportFromFModel();
    if (!st.oodlePath.empty()) LoadOodle(st.oodlePath);
    vfs.SetKeys(st.ParsedKeys());
    if (st.gameDir.empty()) { Out("game directory not configured\n"); return false; }
    DWORD t0 = GetTickCount();
    bool ok = vfs.Mount(st.PaksDir());
    Out(Fmt("mounted %zu paks, %zu files in %u ms\n", vfs.Paks().size(), vfs.FileCount(), GetTickCount() - t0));
    return ok;
}

int RunCli(int argc, wchar_t** argv) {
    // argv[0] exe, argv[1] --cli
    if (argc < 3) { Out("usage: --cli list|json|scan|export ...\n"); return 1; }
    std::string cmd = Narrow(argv[2]);
    std::string arg = argc > 3 ? Narrow(argv[3]) : "";
    Settings st;
    Vfs vfs;
    if (!MountFromSettings(st, vfs)) return 2;

    if (cmd == "list") {
        std::string needle = ToLower(arg);
        size_t n = 0;
        for (u32 i = 0; i < vfs.FileCount(); i++) {
            if (!needle.empty() && ToLower(vfs.Path(i)).find(needle) == std::string::npos) continue;
            const auto& e = vfs.Entry(i);
            Out(Fmt("%s\t%llu\t%s%s\n", std::string(vfs.Path(i)).c_str(), (unsigned long long)e.usize, CompressionName(e.comp), e.encrypted ? "+AES" : ""));
            if (++n >= 2000) break;
        }
        return 0;
    }
    if (cmd == "json" || cmd == "export" || cmd == "native") {
        i64 idx = vfs.Find(arg);
        if (idx < 0) idx = vfs.FindPackage(arg);
        if (idx < 0) { Out("not found\n"); return 3; }
        PackageCache cache(vfs);
        std::string err;
        auto pkg = cache.Load((u32)idx, &err);
        if (!pkg) { Out("load failed: " + err + "\n"); return 4; }
        if (cmd == "json") { Out(PackageToJson(*pkg)); Out("\n"); return 0; }
        if (cmd == "native") {   // hex of the class data following each export's properties
            size_t n = argc > 4 ? (size_t)_wtoi(argv[4]) : 128;
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
        std::wstring out = argc > 4 ? argv[4] : st.outputDir;
        ExportOptions opt;
        opt.outputDir = out;
        opt.meshFormat = st.meshFormat;
        opt.textureFormat = st.textureFormat;
        std::vector<std::string> written;
        bool ok = ExportPackage(cache, *pkg, opt, written, &err);
        for (auto& w : written) Out("wrote " + w + "\n");
        if (!ok) Out("export failed: " + err + "\n");
        return ok ? 0 : 5;
    }
    if (cmd == "scan") {
        std::string needle = ToLower(arg);
        size_t max = argc > 4 ? (size_t)_wtoi(argv[4]) : SIZE_MAX;
        std::map<std::string, std::pair<int, int>> perClass;   // class -> (ok, property errors)
        std::map<std::string, int> formats;
        size_t total = 0, readFail = 0, loadFail = 0;
        PackageCache cache(vfs, 64ull << 20);
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
                else {
                    if (c.second++ < 3) Out("PROPS " + p + " [" + pkg.Exports()[e].objectName + "]: " + perr + "\n");
                }
                if (IsTextureClass(cls)) {
                    TextureInfo ti;
                    std::string terr;
                    if (ReadTextureInfo(pkg, e, ti, &terr)) formats[ti.pixelFormat]++;
                    else { formats["<error>"]++; if (formats["<error>"] <= 5) Out("TEXTURE " + p + ": " + terr + "\n"); }
                }
            }
        }
        Out(Fmt("\npackages %zu, read failures %zu, load failures %zu\n", total, readFail, loadFail));
        for (auto& [k, v] : perClass) Out(Fmt("%-48s ok %6d  errors %d\n", k.c_str(), v.first, v.second));
        for (auto& [k, v] : formats) Out(Fmt("texture format %-20s %d\n", k.c_str(), v));
        return 0;
    }
    Out("unknown command\n");
    return 1;
}
