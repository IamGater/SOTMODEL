#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <stdexcept>
#include <functional>
#include <algorithm>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using Bytes = std::vector<u8>;

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string Fmt(const char* fmt, ...);
std::string ToLower(std::string_view s);
bool IEquals(std::string_view a, std::string_view b);
bool IStartsWith(std::string_view s, std::string_view prefix);
bool IEndsWith(std::string_view s, std::string_view suffix);
std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);

// Log ring shared by the core and the UI (thread safe).
enum class LogLevel { Info, Warn, Error };
void Log(LogLevel level, const char* fmt, ...);
#define LOGI(...) Log(LogLevel::Info, __VA_ARGS__)
#define LOGW(...) Log(LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) Log(LogLevel::Error, __VA_ARGS__)
struct LogLine { LogLevel level; std::string text; };
void LogSnapshot(std::vector<LogLine>& out, size_t& seen);   // appends lines newer than `seen`

bool ReadWholeFile(const std::wstring& path, Bytes& out);
bool WriteWholeFile(const std::wstring& path, const void* data, size_t size);
bool CreateDirectories(const std::wstring& path);
std::wstring ExeDirectory();
