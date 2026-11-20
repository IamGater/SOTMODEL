#include "core/Crypto.h"
#include <windows.h>
#include <bcrypt.h>
#include <mutex>
#include <map>

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseAesKey(std::string_view hex, AesKey& out) {
    while (!hex.empty() && (hex.front() == ' ' || hex.front() == '"')) hex.remove_prefix(1);
    while (!hex.empty() && (hex.back() == ' ' || hex.back() == '"' || hex.back() == '\r' || hex.back() == '\n')) hex.remove_suffix(1);
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex.remove_prefix(2);
    if (hex.size() != 64) return false;
    for (int i = 0; i < 32; i++) {
        int a = HexVal(hex[i * 2]), b = HexVal(hex[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (u8)(a * 16 + b);
    }
    return true;
}

std::string AesKeyToHex(const AesKey& k) {
    static const char* d = "0123456789ABCDEF";
    std::string s = "0x";
    for (u8 b : k) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

namespace {
struct KeyHandle {
    BCRYPT_KEY_HANDLE handle = nullptr;
};
std::mutex g_mutex;
BCRYPT_ALG_HANDLE g_alg = nullptr;
thread_local std::map<AesKey, KeyHandle> t_keys;   // key objects are per thread: BCrypt does not promise concurrent use of one handle

BCRYPT_KEY_HANDLE GetKey(const AesKey& key) {
    {
        std::lock_guard lk(g_mutex);
        if (!g_alg) {
            if (BCryptOpenAlgorithmProvider(&g_alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return nullptr;
            BCryptSetProperty(g_alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        }
    }
    auto& kh = t_keys[key];
    if (!kh.handle) BCryptGenerateSymmetricKey(g_alg, &kh.handle, nullptr, 0, (PUCHAR)key.data(), 32, 0);
    return kh.handle;
}
}

bool AesDecrypt(const AesKey& key, u8* data, size_t size) {
    if (size % 16) return false;
    BCRYPT_KEY_HANDLE h = GetKey(key);
    if (!h) return false;
    ULONG got = 0;
    for (size_t off = 0; off < size;) {
        ULONG chunk = (ULONG)std::min<size_t>(size - off, 1u << 30);
        if (BCryptDecrypt(h, data + off, chunk, nullptr, nullptr, 0, data + off, chunk, &got, 0) != 0) return false;
        off += chunk;
    }
    return true;
}
