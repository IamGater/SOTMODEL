#include "export/Png.h"

namespace {
struct BitWriter {
    Bytes& out;
    u64 acc = 0;
    int n = 0;
    explicit BitWriter(Bytes& o) : out(o) {}
    void Put(u32 bits, int count) {   // LSB first
        acc |= (u64)bits << n;
        n += count;
        while (n >= 8) { out.push_back((u8)acc); acc >>= 8; n -= 8; }
    }
    void PutRev(u32 code, int len) {   // Huffman codes are sent MSB first
        u32 r = 0;
        for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i);
        Put(r, len);
    }
    void Flush() { if (n) { out.push_back((u8)acc); acc = 0; n = 0; } }
};

void Literal(BitWriter& w, int c) {
    if (c < 144) w.PutRev(0x30 + c, 8);
    else if (c < 256) w.PutRev(0x190 + c - 144, 9);
    else if (c < 280) w.PutRev(c - 256, 7);
    else w.PutRev(0xC0 + c - 280, 8);
}

const u16 kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const u8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const u16 kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void Match(BitWriter& w, int len, int dist) {
    int li = 28;
    while (kLenBase[li] > len) li--;
    Literal(w, 257 + li);
    if (kLenExtra[li]) w.Put(len - kLenBase[li], kLenExtra[li]);
    int di = 29;
    while (kDistBase[di] > dist) di--;
    w.PutRev(di, 5);
    if (kDistExtra[di]) w.Put(dist - kDistBase[di], kDistExtra[di]);
}
}

u32 Crc32(const u8* d, size_t n, u32 crc) {
    static u32 table[256];
    static bool init = false;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ d[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

Bytes DeflateZlib(const u8* d, size_t n) {
    Bytes out;
    out.reserve(n / 2 + 64);
    out.push_back(0x78);
    out.push_back(0x5E);
    BitWriter w(out);
    w.Put(1, 1);   // final block
    w.Put(1, 2);   // fixed Huffman
    constexpr int HashBits = 15, Window = 32768, MaxChain = 24;
    std::vector<i32> head(1 << HashBits, -1), prev(Window, -1);
    auto hash = [&](size_t i) { return ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & ((1 << HashBits) - 1); };
    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + 3 <= n) {
            int h = hash(i);
            i32 cand = head[h];
            int chain = MaxChain;
            size_t maxLen = std::min<size_t>(258, n - i);
            while (cand >= 0 && (i - cand) <= Window && chain--) {
                if (d[cand + bestLen] == d[i + bestLen] || bestLen == 0) {
                    size_t l = 0;
                    while (l < maxLen && d[cand + l] == d[i + l]) l++;
                    if ((int)l > bestLen) { bestLen = (int)l; bestDist = (int)(i - cand); if (l == maxLen) break; }
                }
                i32 p = prev[cand & (Window - 1)];
                if (p >= cand) break;
                cand = p;
            }
            prev[i & (Window - 1)] = head[h];
            head[h] = (i32)i;
        }
        if (bestLen >= 3) {
            Match(w, bestLen, bestDist);
            for (size_t k = 1; k < (size_t)bestLen; k++) {
                size_t j = i + k;
                if (j + 3 <= n) {
                    int h = hash(j);
                    prev[j & (Window - 1)] = head[h];
                    head[h] = (i32)j;
                }
            }
            i += bestLen;
        } else {
            Literal(w, d[i]);
            i++;
        }
    }
    Literal(w, 256);
    w.Flush();
    // adler32
    u32 a = 1, b = 0;
    for (size_t k = 0; k < n; k++) { a = (a + d[k]) % 65521; b = (b + a) % 65521; }
    u32 ad = (b << 16) | a;
    out.push_back((u8)(ad >> 24)); out.push_back((u8)(ad >> 16)); out.push_back((u8)(ad >> 8)); out.push_back((u8)ad);
    return out;
}

static void PutBE(Bytes& o, u32 v) { o.push_back((u8)(v >> 24)); o.push_back((u8)(v >> 16)); o.push_back((u8)(v >> 8)); o.push_back((u8)v); }

static void Chunk(Bytes& png, const char* type, const u8* data, size_t size) {
    PutBE(png, (u32)size);
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    if (size) png.insert(png.end(), data, data + size);
    PutBE(png, Crc32(png.data() + start, size + 4));
}

Bytes EncodePng(const u8* rgba, int w, int h, bool dropOpaqueAlpha) {
    bool opaque = dropOpaqueAlpha;
    for (size_t i = 3; opaque && i < (size_t)w * h * 4; i += 4) if (rgba[i] != 255) opaque = false;
    int ch = opaque ? 3 : 4;
    size_t stride = (size_t)w * ch;
    Bytes raw((stride + 1) * h);
    std::vector<u8> line(stride), prevLine(stride, 0), cand(stride);
    for (int y = 0; y < h; y++) {
        const u8* src = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) memcpy(&line[x * ch], src + x * 4, ch);
        // pick the filter with the smallest absolute sum (standard heuristic)
        int bestF = 0;
        u64 bestSum = UINT64_MAX;
        u8* dst = raw.data() + y * (stride + 1);
        for (int f = 0; f < 5; f++) {
            u64 sum = 0;
            for (size_t i = 0; i < stride; i++) {
                int a = i >= (size_t)ch ? line[i - ch] : 0, b = prevLine[i], c = i >= (size_t)ch ? prevLine[i - ch] : 0;
                int pred = 0;
                if (f == 1) pred = a;
                else if (f == 2) pred = b;
                else if (f == 3) pred = (a + b) / 2;
                else if (f == 4) {
                    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                    pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                }
                u8 v = (u8)(line[i] - pred);
                cand[i] = v;
                sum += v < 128 ? v : 256 - v;
            }
            if (sum < bestSum) { bestSum = sum; bestF = f; memcpy(dst + 1, cand.data(), stride); }
        }
        dst[0] = (u8)bestF;
        std::swap(line, prevLine);
    }
    Bytes png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    u8 ihdr[13];
    ihdr[0] = (u8)(w >> 24); ihdr[1] = (u8)(w >> 16); ihdr[2] = (u8)(w >> 8); ihdr[3] = (u8)w;
    ihdr[4] = (u8)(h >> 24); ihdr[5] = (u8)(h >> 16); ihdr[6] = (u8)(h >> 8); ihdr[7] = (u8)h;
    ihdr[8] = 8;
    ihdr[9] = opaque ? 2 : 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    Chunk(png, "IHDR", ihdr, 13);
    Bytes z = DeflateZlib(raw.data(), raw.size());
    Chunk(png, "IDAT", z.data(), z.size());
    Chunk(png, "IEND", nullptr, 0);
    return png;
}
