#include "ue/TextureDecode.h"
#include "ue/Texture.h"
#include <cmath>

static inline void Rgb565(u16 c, u8* o) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    o[0] = (u8)((r << 3) | (r >> 2));
    o[1] = (u8)((g << 2) | (g >> 4));
    o[2] = (u8)((b << 3) | (b >> 2));
    o[3] = 255;
}

void DecodeBC1Block(const u8* s, u8* out, bool alpha1bit) {
    u16 c0 = s[0] | (s[1] << 8), c1 = s[2] | (s[3] << 8);
    u8 pal[4][4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    if (c0 > c1 || !alpha1bit) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (u8)((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        pal[3][3] = 0;
    }
    u32 idx = s[4] | (s[5] << 8) | (s[6] << 16) | ((u32)s[7] << 24);
    for (int i = 0; i < 16; i++) memcpy(out + i * 4, pal[(idx >> (2 * i)) & 3], 4);
}

void DecodeBC4Block(const u8* s, u8* out, int stride) {
    int a0 = s[0], a1 = s[1];
    u8 pal[8];
    pal[0] = (u8)a0;
    pal[1] = (u8)a1;
    if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = (u8)(((7 - i) * a0 + i * a1 + 3) / 7);
    else {
        for (int i = 1; i < 5; i++) pal[i + 1] = (u8)(((5 - i) * a0 + i * a1 + 2) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; i++) bits |= (u64)s[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i * stride] = pal[(bits >> (3 * i)) & 7];
}

void DecodeBC2Block(const u8* s, u8* out) {
    DecodeBC1Block(s + 8, out, false);
    for (int i = 0; i < 16; i++) {
        int a = (s[i / 2] >> ((i & 1) * 4)) & 15;
        out[i * 4 + 3] = (u8)(a * 17);
    }
}

void DecodeBC3Block(const u8* s, u8* out) {
    DecodeBC1Block(s + 8, out, false);
    DecodeBC4Block(s, out + 3, 4);
}

void DecodeBC5Block(const u8* s, u8* out, bool reconstructZ) {
    DecodeBC4Block(s, out + 0, 4);
    DecodeBC4Block(s + 8, out + 1, 4);
    for (int i = 0; i < 16; i++) {
        u8* p = out + i * 4;
        if (reconstructZ) {
            float x = p[0] / 127.5f - 1.f, y = p[1] / 127.5f - 1.f;
            float z = sqrtf(std::max(0.f, 1.f - x * x - y * y));
            p[2] = (u8)std::clamp((int)((z * 0.5f + 0.5f) * 255.f + 0.5f), 0, 255);
        } else p[2] = 0;
        p[3] = 255;
    }
}

float HalfToFloat(u16 h) {
    u32 sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    u32 f;
    if (exp == 0) {
        if (man == 0) f = sign << 31;
        else {
            exp = 127 - 15 + 1;
            while (!(man & 1024)) { man <<= 1; exp--; }
            man &= 1023;
            f = (sign << 31) | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) f = (sign << 31) | 0x7F800000 | (man << 13);
    else f = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    float r;
    memcpy(&r, &f, 4);
    return r;
}

namespace {
struct FormatInfo { const char* name; int blockW, blockBytes; };   // blockW 1 = uncompressed
const FormatInfo g_formats[] = {
    {"PF_DXT1", 4, 8}, {"PF_DXT3", 4, 16}, {"PF_DXT5", 4, 16}, {"PF_BC4", 4, 8}, {"PF_BC5", 4, 16}, {"PF_ATI2", 4, 16},
    {"PF_BC6H", 4, 16}, {"PF_BC7", 4, 16},
    {"PF_B8G8R8A8", 1, 4}, {"PF_R8G8B8A8", 1, 4}, {"PF_A8R8G8B8", 1, 4}, {"PF_G8", 1, 1}, {"PF_A8", 1, 1}, {"PF_L8", 1, 1},
    {"PF_G16", 1, 2}, {"PF_V8U8", 1, 2}, {"PF_FloatRGBA", 1, 8}, {"PF_FloatRGB", 1, 6}, {"PF_A32B32G32R32F", 1, 16},
    {"PF_R16F", 1, 2}, {"PF_R32_FLOAT", 1, 4}, {"PF_G16R16", 1, 4}, {"PF_G16R16F", 1, 4}, {"PF_A2B10G10R10", 1, 4}, {"PF_R5G6B5_UNORM", 1, 2},
};
const FormatInfo* Find(const std::string& f) {
    for (auto& x : g_formats) if (f == x.name) return &x;
    return nullptr;
}
inline u8 ToByte(float v) { return (u8)std::clamp((int)(v * 255.f + 0.5f), 0, 255); }
inline u8 ToneMap(float v) { v = v / (1.f + v); return ToByte(powf(std::max(v, 0.f), 1.f / 2.2f)); }
}

size_t PixelDataSize(const std::string& pf, int w, int h) {
    const FormatInfo* f = Find(pf);
    if (!f) return 0;
    if (f->blockW == 1) return (size_t)w * h * f->blockBytes;
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * f->blockBytes;
}

bool DecodePixels(const std::string& pf, const u8* data, size_t size, int w, int h, int slices, Image& out, std::string* error) {
    const FormatInfo* f = Find(pf);
    if (!f) { if (error) *error = "unsupported pixel format " + pf; return false; }
    size_t sliceSize = PixelDataSize(pf, w, h);
    if (size < sliceSize) { if (error) *error = Fmt("pixel data too small (%zu < %zu)", size, sliceSize); return false; }
    slices = std::max(1, std::min<int>(slices, (int)(size / sliceSize)));
    out.width = w;
    out.height = h * slices;
    out.rgba.assign((size_t)w * h * slices * 4, 255);
    for (int sl = 0; sl < slices; sl++) {
        const u8* src = data + sl * sliceSize;
        u8* dst = out.rgba.data() + (size_t)sl * w * h * 4;
        if (f->blockW == 4) {
            if (pf == "PF_BC6H") { if (error) *error = pf + " decoding is not implemented"; return false; }
            int bw = (w + 3) / 4, bh = (h + 3) / 4;
            u8 block[64];
            for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
                const u8* b = src + ((size_t)by * bw + bx) * f->blockBytes;
                if (pf == "PF_DXT1") DecodeBC1Block(b, block, true);
                else if (pf == "PF_DXT3") DecodeBC2Block(b, block);
                else if (pf == "PF_DXT5") DecodeBC3Block(b, block);
                else if (pf == "PF_BC7") DecodeBC7Block(b, block);
                else if (pf == "PF_BC4") {
                    DecodeBC4Block(b, block, 4);
                    for (int i = 0; i < 16; i++) { block[i * 4 + 1] = block[i * 4 + 2] = block[i * 4]; block[i * 4 + 3] = 255; }
                } else DecodeBC5Block(b, block, true);
                for (int y = 0; y < 4; y++) {
                    int py = by * 4 + y;
                    if (py >= h) break;
                    int cols = std::min(4, w - bx * 4);
                    memcpy(dst + ((size_t)py * w + bx * 4) * 4, block + y * 16, cols * 4);
                }
            }
            continue;
        }
        size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; i++) {
            u8* o = dst + i * 4;
            const u8* p = src + i * f->blockBytes;
            if (pf == "PF_B8G8R8A8") { o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; }
            else if (pf == "PF_R8G8B8A8") memcpy(o, p, 4);
            else if (pf == "PF_A8R8G8B8") { o[0] = p[1]; o[1] = p[2]; o[2] = p[3]; o[3] = p[0]; }
            else if (pf == "PF_G8" || pf == "PF_L8") { o[0] = o[1] = o[2] = p[0]; o[3] = 255; }
            else if (pf == "PF_A8") { o[0] = o[1] = o[2] = 255; o[3] = p[0]; }
            else if (pf == "PF_G16") { o[0] = o[1] = o[2] = p[1]; o[3] = 255; }
            else if (pf == "PF_V8U8") { o[0] = (u8)(p[0] + 128); o[1] = (u8)(p[1] + 128); o[2] = 255; o[3] = 255; }
            else if (pf == "PF_FloatRGBA" || pf == "PF_FloatRGB") {
                u16 hv[4] = {0, 0, 0, 0x3C00};
                memcpy(hv, p, f->blockBytes);
                for (int k = 0; k < 3; k++) o[k] = ToneMap(HalfToFloat(hv[k]));
                o[3] = ToByte(HalfToFloat(hv[3]));
            } else if (pf == "PF_A32B32G32R32F") {
                float fv[4];
                memcpy(fv, p, 16);
                for (int k = 0; k < 3; k++) o[k] = ToneMap(fv[k]);
                o[3] = ToByte(fv[3]);
            } else if (pf == "PF_R16F") { o[0] = o[1] = o[2] = ToneMap(HalfToFloat(p[0] | (p[1] << 8))); o[3] = 255; }
            else if (pf == "PF_R32_FLOAT") { float v; memcpy(&v, p, 4); o[0] = o[1] = o[2] = ToneMap(v); o[3] = 255; }
            else if (pf == "PF_G16R16") { o[0] = p[1]; o[1] = p[3]; o[2] = 0; o[3] = 255; }
            else if (pf == "PF_G16R16F") { o[0] = ToneMap(HalfToFloat(p[0] | (p[1] << 8))); o[1] = ToneMap(HalfToFloat(p[2] | (p[3] << 8))); o[2] = 0; o[3] = 255; }
            else if (pf == "PF_A2B10G10R10") {
                u32 v; memcpy(&v, p, 4);
                o[0] = (u8)((v & 1023) >> 2); o[1] = (u8)(((v >> 10) & 1023) >> 2); o[2] = (u8)(((v >> 20) & 1023) >> 2); o[3] = (u8)((v >> 30) * 85);
            } else if (pf == "PF_R5G6B5_UNORM") Rgb565((u16)(p[0] | (p[1] << 8)), o);
        }
    }
    return true;
}
