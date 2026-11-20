#include "core/Common.h"
#include <windows.h>
#include <cstdarg>
#include <mutex>
#include <deque>

std::string Fmt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(buf)) return std::string(buf, n > 0 ? n : 0);
    std::string s(n, '\0');
    va_start(ap, fmt);
    vsnprintf(s.data(), n + 1, fmt, ap);
    va_end(ap);
    return s;
}

std::string ToLower(std::string_view s) {
    std::string r(s);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c += 32;
    return r;
}

static inline char Low(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (Low(a[i]) != Low(b[i])) return false;
    return true;
}
bool IStartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && IEquals(s.substr(0, p.size()), p); }
bool IEndsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && IEquals(s.substr(s.size() - p.size()), p); }

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::mutex g_logMutex;
static std::deque<LogLine> g_log;
static size_t g_logBase = 0;   // index of g_log.front()

void Log(LogLevel level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard lk(g_logMutex);
    g_log.push_back({level, buf});
    if (g_log.size() > 4000) { g_log.pop_front(); g_logBase++; }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

void LogSnapshot(std::vector<LogLine>& out, size_t& seen) {
    std::lock_guard lk(g_logMutex);
    if (seen < g_logBase) seen = g_logBase;
    for (size_t i = seen - g_logBase; i < g_log.size(); i++) out.push_back(g_log[i]);
    seen = g_logBase + g_log.size();
}

bool ReadWholeFile(const std::wstring& path, Bytes& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    CloseHandle(h);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = size == 0 || (WriteFile(h, data, (DWORD)size, &put, nullptr) && put == size);
    CloseHandle(h);
    return ok;
}

bool CreateDirectories(const std::wstring& path) {
    std::wstring cur;
    for (size_t i = 0; i < path.size(); i++) {
        wchar_t c = path[i];
        if ((c == L'\\' || c == L'/') && !cur.empty() && cur.back() != L':') CreateDirectoryW(cur.c_str(), nullptr);
        cur += (c == L'/') ? L'\\' : c;
    }
    if (!cur.empty()) CreateDirectoryW(cur.c_str(), nullptr);
    DWORD a = GetFileAttributesW(cur.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ExeDirectory() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}
