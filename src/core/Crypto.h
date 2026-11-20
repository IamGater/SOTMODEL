#pragma once
#include "core/Common.h"
#include <array>

using AesKey = std::array<u8, 32>;

bool ParseAesKey(std::string_view hex, AesKey& out);   // "0x..." or bare 64 hex chars
std::string AesKeyToHex(const AesKey& k);

// AES-256-ECB in-place decryption (size must be a multiple of 16). Thread safe.
bool AesDecrypt(const AesKey& key, u8* data, size_t size);
