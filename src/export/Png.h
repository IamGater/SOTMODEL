#pragma once
#include "core/Common.h"

// zlib stream (LZ77 + fixed Huffman) - small and fast, ~deflate level 3 ratio.
Bytes DeflateZlib(const u8* data, size_t size);
u32 Crc32(const u8* data, size_t size, u32 crc = 0);

// RGBA8 -> PNG (channels 4, or 3 when the alpha channel is fully opaque and dropAlpha).
Bytes EncodePng(const u8* rgba, int width, int height, bool dropOpaqueAlpha = true);
