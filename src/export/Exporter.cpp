#include "export/Exporter.h"
#include "ue/AssetJson.h"

std::wstring ExportPathFor(const ExportOptions& opt, const Package& pkg, const std::wstring& ext) {
    std::string rel = pkg.GamePath();
    size_t dot = rel.rfind('.');
    if (dot != std::string::npos) rel.resize(dot);
    if (!opt.keepDirectories) rel = rel.substr(rel.rfind('/') + 1);
    std::wstring p = opt.outputDir + L"\\" + Widen(rel) + ext;
    for (auto& c : p) if (c == L'/') c = L'\\';
    return p;
}

static bool WriteOut(const std::wstring& path, const void* data, size_t size, std::vector<std::string>& written) {
    CreateDirectories(path.substr(0, path.find_last_of(L'\\')));
    if (!WriteWholeFile(path, data, size)) return false;
    written.push_back(Narrow(path));
    return true;
}

bool ExportPackage(PackageCache& cache, const Package& pkg, const ExportOptions& opt, std::vector<std::string>& written, std::string* error) {
    (void)cache;
    bool ok = true;
    if (opt.json) {
        std::string j = PackageToJson(pkg);
        ok &= WriteOut(ExportPathFor(opt, pkg, L".json"), j.data(), j.size(), written);
    }
    if (opt.raw) {
        std::wstring ext = IEndsWith(pkg.GamePath(), ".umap") ? L".umap" : L".uasset";
        ok &= WriteOut(ExportPathFor(opt, pkg, ext), pkg.Data().data(), pkg.Data().size(), written);
    }
    if (!ok && error) *error = "could not write output files";
    return ok;
}
