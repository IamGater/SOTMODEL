#include "app/WebUi.h"
#include <windows.h>

std::string WebRootOf(std::string_view gamePath) {
    std::string low = ToLower(std::string(gamePath));
    for (const char* r : {"/content/webroot/", "/content/uiresources/"}) {
        size_t p = low.find(r);
        if (p != std::string::npos) return std::string(gamePath.substr(0, p + strlen(r)));
    }
    return "";
}

bool IsWebPageFile(std::string_view name) { return IEndsWith(name, ".html") || IEndsWith(name, ".htm"); }

bool ExportWebRoot(Api& a, const std::string& root, const std::wstring& outputDir, std::wstring& localRoot, int* written, int* total,
                   std::string* error) {
    if (root.empty() || outputDir.empty()) {
        if (error) *error = root.empty() ? "not a web UI path" : "no output directory set";
        return false;
    }
    std::wstring base = outputDir + L"\\Web\\";
    localRoot = base + Widen(root);
    for (auto& c : localRoot) if (c == L'/') c = L'\\';
    while (!localRoot.empty() && localRoot.back() == L'\\') localRoot.pop_back();
    int n = 0, w = 0;
    std::string firstError;
    for (u32 i = 0; i < a.vfs.FileCount(); i++) {
        std::string_view p = a.vfs.Path(i);
        if (p.size() <= root.size() || !IStartsWith(p, root)) continue;
        n++;
        std::wstring out = base + Widen(p);
        for (auto& c : out) if (c == L'/') c = L'\\';
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fa) &&
            (((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) == a.vfs.Entry(i).usize)
            continue;
        Bytes data;
        std::string err;
        if (!a.vfs.Read(i, data, &err)) {
            if (firstError.empty()) firstError = std::string(p) + ": " + err;
            continue;
        }
        CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
        if (!WriteWholeFile(out, data.data(), data.size())) {
            if (firstError.empty()) firstError = "can't write " + Narrow(out);
            continue;
        }
        w++;
    }
    if (written) *written = w;
    if (total) *total = n;
    if (!n) {
        if (error) *error = "no files under " + root;
        return false;
    }
    if (error) *error = firstError;
    return true;
}
