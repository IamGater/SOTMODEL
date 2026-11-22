#include "app/Settings.h"
#include <windows.h>
#include <shlobj.h>
#include <regex>

static std::wstring AppData() {
    wchar_t* p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Settings::Path() { return AppData() + L"\\SOTModel\\Settings.ini"; }

void Settings::Load() {
    Bytes b;
    if (ReadWholeFile(Path(), b)) {
        std::string text(b.begin(), b.end());
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? text.size() : nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == '#' || line[0] == ';') continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "GameDir") gameDir = Widen(v);
            else if (k == "OutputDir") outputDir = Widen(v);
            else if (k == "OodlePath") oodlePath = Widen(v);
            else if (k == "AesKey") { if (!v.empty()) aesKeys.push_back(v); }
            else if (k == "PreviewMaxTexture") previewMaxTexture = std::clamp(atoi(v.c_str()), 256, 16384);
            else if (k == "KeepDirectories") exportKeepDirectories = v == "1";
        else if (k == "AnimationSounds") exportAnimSounds = v == "1";
            else if (k == "MeshFormat") meshFormat = atoi(v.c_str());
            else if (k == "TextureFormat") textureFormat = atoi(v.c_str());
            else if (k == "WorldDrawDistance") worldDrawDistance = (float)atof(v.c_str());
            else if (k == "ShowGrid") showGrid = v == "1";
            else if (k == "ExplorerViewMode") explorerViewMode = std::clamp(atoi(v.c_str()), 0, 1);
            else if (k == "LoggerExpanded") loggerExpanded = v == "1";
            else if (k == "LeftPanelWidth") leftPanelWidth = (float)atof(v.c_str());
            else if (k == "PreviewTextures") previewTextures = v == "1";
            else if (k == "ViewerWindow") viewerWindow = v == "1";
            else if (k == "Language") { if (!v.empty()) language = v; }
            else if (k == "FavoriteDir") { if (!v.empty()) favoriteDirs.push_back(v); }
        }
    }
    if (outputDir.empty()) {
        wchar_t* docs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) outputDir = std::wstring(docs) + L"\\SOTModel\\Output";
        CoTaskMemFree(docs);
    }
}

bool Settings::Save() const {
    std::string s = "# SOTModel settings\n";
    s += "GameDir=" + Narrow(gameDir) + "\n";
    s += "OutputDir=" + Narrow(outputDir) + "\n";
    s += "OodlePath=" + Narrow(oodlePath) + "\n";
    for (auto& k : aesKeys) s += "AesKey=" + k + "\n";
    s += "PreviewMaxTexture=" + std::to_string(previewMaxTexture) + "\n";
    s += std::string("KeepDirectories=") + (exportKeepDirectories ? "1" : "0") + "\n";
    s += std::string("AnimationSounds=") + (exportAnimSounds ? "1" : "0") + "\n";
    s += "MeshFormat=" + std::to_string(meshFormat) + "\n";
    s += "TextureFormat=" + std::to_string(textureFormat) + "\n";
    s += Fmt("WorldDrawDistance=%.0f\n", worldDrawDistance);
    s += std::string("ShowGrid=") + (showGrid ? "1" : "0") + "\n";
    s += "ExplorerViewMode=" + std::to_string(explorerViewMode) + "\n";
    s += std::string("LoggerExpanded=") + (loggerExpanded ? "1" : "0") + "\n";
    s += Fmt("LeftPanelWidth=%.0f\n", leftPanelWidth);
    s += std::string("PreviewTextures=") + (previewTextures ? "1" : "0") + "\n";
    s += std::string("ViewerWindow=") + (viewerWindow ? "1" : "0") + "\n";
    s += "Language=" + language + "\n";
    for (auto& f : favoriteDirs) s += "FavoriteDir=" + f + "\n";
    std::wstring p = Path();
    CreateDirectories(p.substr(0, p.find_last_of(L'\\')));
    return WriteWholeFile(p, s.data(), s.size());
}

std::vector<AesKey> Settings::ParsedKeys() const {
    std::vector<AesKey> out;
    for (auto& k : aesKeys) {
        AesKey key;
        if (ParseAesKey(k, key) && std::find(out.begin(), out.end(), key) == out.end()) out.push_back(key);
    }
    return out;
}

std::wstring Settings::PaksDir() const {
    std::wstring p = gameDir + L"\\Athena\\Content\\Paks";
    return DirExists(p) ? p : gameDir;
}

static std::string JsonUnescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            r += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else r += s[i];
    }
    return r;
}

std::string Settings::ImportFromFModel() {
    Bytes b;
    if (!ReadWholeFile(AppData() + L"\\FModel\\AppSettings.json", b)) return {};
    std::string text(b.begin(), b.end());
    std::string summary;
    // keys
    std::regex keyRe("0x[0-9A-Fa-f]{64}");
    int added = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), keyRe); it != std::sregex_iterator(); ++it) {
        std::string k = it->str();
        bool have = false;
        for (auto& e : aesKeys) if (IEquals(e, k)) have = true;
        if (!have) { aesKeys.push_back(k); added++; }
    }
    if (added) summary += Fmt("%d AES key(s); ", added);
    // game directory: the first configured directory that actually holds Sea of Thieves paks
    std::regex dirRe("\"GameDirectory\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
    if (gameDir.empty() || !DirExists(gameDir)) {
        for (auto it = std::sregex_iterator(text.begin(), text.end(), dirRe); it != std::sregex_iterator(); ++it) {
            std::wstring d = Widen(JsonUnescape((*it)[1].str()));
            if (DirExists(d + L"\\Athena\\Content\\Paks")) { gameDir = d; summary += "game directory; "; break; }
        }
    }
    // oodle: FModel keeps it in <OutputDirectory>\.data
    std::regex outRe("\"OutputDirectory\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
    std::smatch m;
    if ((oodlePath.empty() || !FileExists(oodlePath)) && std::regex_search(text, m, outRe)) {
        std::wstring dll = Widen(JsonUnescape(m[1].str())) + L"\\.data\\oodle-data-shared.dll";
        if (FileExists(dll)) { oodlePath = dll; summary += "Oodle DLL; "; }
    }
    return summary.empty() ? "nothing new" : summary;
}
