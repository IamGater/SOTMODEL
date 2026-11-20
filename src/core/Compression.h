#pragma once
#include "core/Common.h"

enum class Compression : u8 { None = 0, Zlib, Gzip, Oodle, LZ4, Unknown };

const char* CompressionName(Compression c);

// Each returns true when exactly dstSize bytes were produced.
bool InflateZlib(const u8* src, size_t srcSize, u8* dst, size_t dstSize);   // zlib-wrapped or raw deflate
bool DecompressLZ4Block(const u8* src, size_t srcSize, u8* dst, size_t dstSize);
bool DecompressOodle(const u8* src, size_t srcSize, u8* dst, size_t dstSize);
bool Decompress(Compression method, const u8* src, size_t srcSize, u8* dst, size_t dstSize);

// Oodle is not redistributable: it is loaded at runtime from a user supplied DLL (FModel ships one in Output/.data).
bool LoadOodle(const std::wstring& dllPath);
bool OodleLoaded();
