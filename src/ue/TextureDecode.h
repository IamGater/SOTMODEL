#pragma once
#include "core/Common.h"

// Block decoders: write a 4x4 RGBA8 block (64 bytes, row major).
void DecodeBC1Block(const u8* src, u8* rgba, bool alpha1bit);
void DecodeBC2Block(const u8* src, u8* rgba);
void DecodeBC3Block(const u8* src, u8* rgba);
void DecodeBC4Block(const u8* src, u8* out, int stride);   // one channel, 16 values with byte stride
void DecodeBC5Block(const u8* src, u8* rgba, bool reconstructZ);
void DecodeBC7Block(const u8* src, u8* rgba);
float HalfToFloat(u16 h);
