#include "core/Compression.h"
#include <windows.h>
#include <mutex>

const char* CompressionName(Compression c) {
    switch (c) {
    case Compression::None: return "None";
    case Compression::Zlib: return "Zlib";
    case Compression::Gzip: return "Gzip";
    case Compression::Oodle: return "Oodle";
    case Compression::LZ4: return "LZ4";
    default: return "Unknown";
    }
}

// ---------------------------------------------------------------- inflate (RFC 1950/1951)
namespace {
constexpr int FastBits = 9;

struct Huffman {
    u16 fast[1 << FastBits];
    u16 firstCode[16];
    int maxCode[17];
    u16 firstSymbol[16];
    u8 size[288];
    u16 value[288];
};

inline int BitReverse16(int n) {
    n = ((n & 0xAAAA) >> 1) | ((n & 0x5555) << 1);
    n = ((n & 0xCCCC) >> 2) | ((n & 0x3333) << 2);
    n = ((n & 0xF0F0) >> 4) | ((n & 0x0F0F) << 4);
    n = ((n & 0xFF00) >> 8) | ((n & 0x00FF) << 8);
    return n;
}
inline int BitReverse(int v, int bits) { return BitReverse16(v) >> (16 - bits); }

bool BuildHuffman(Huffman& h, const u8* lengths, int num) {
    int sizes[17] = {}, nextCode[16];
    memset(h.fast, 0, sizeof(h.fast));
    for (int i = 0; i < num; i++) sizes[lengths[i]]++;
    sizes[0] = 0;
    for (int i = 1; i < 16; i++) if (sizes[i] > (1 << i)) return false;
    int code = 0, k = 0;
    for (int i = 1; i < 16; i++) {
        nextCode[i] = code;
        h.firstCode[i] = (u16)code;
        h.firstSymbol[i] = (u16)k;
        code += sizes[i];
        if (sizes[i] && code - 1 >= (1 << i)) return false;
        h.maxCode[i] = code << (16 - i);
        code <<= 1;
        k += sizes[i];
    }
    h.maxCode[16] = 0x10000;
    for (int i = 0; i < num; i++) {
        int s = lengths[i];
        if (!s) continue;
        int c = nextCode[s] - h.firstCode[s] + h.firstSymbol[s];
        u16 fastv = (u16)((s << 9) | i);
        h.size[c] = (u8)s;
        h.value[c] = (u16)i;
        if (s <= FastBits) {
            for (int j = BitReverse(nextCode[s], s); j < (1 << FastBits); j += (1 << s)) h.fast[j] = fastv;
        }
        nextCode[s]++;
    }
    return true;
}

struct Inflater {
    const u8* src;
    const u8* srcEnd;
    u8* dst;
    u8* out;
    u8* dstEnd;
    u64 bits = 0;
    int numBits = 0;
    int overrun = 0;
    Huffman lit, dist;

    void Fill() {
        while (numBits <= 56) {
            u64 b;
            if (src < srcEnd) b = *src++;
            else { b = 0; overrun++; }
            bits |= b << numBits;
            numBits += 8;
        }
    }
    u32 Get(int n) {
        if (numBits < n) Fill();
        u32 v = (u32)(bits & ((1ull << n) - 1));
        bits >>= n;
        numBits -= n;
        return v;
    }
    int Decode(const Huffman& h) {
        if (numBits < 16) Fill();
        int b = h.fast[bits & ((1 << FastBits) - 1)];
        if (b) {
            int s = b >> 9;
            bits >>= s;
            numBits -= s;
            return b & 511;
        }
        int k = BitReverse16((int)(bits & 0xFFFF));
        int s;
        for (s = FastBits + 1;; s++) if (k < h.maxCode[s]) break;
        if (s >= 16) return -1;
        int idx = (k >> (16 - s)) - h.firstCode[s] + h.firstSymbol[s];
        if (idx < 0 || idx >= 288 || h.size[idx] != s) return -1;
        bits >>= s;
        numBits -= s;
        return h.value[idx];
    }

    bool Stored() {
        int skip = numBits & 7;
        Get(skip);
        u8 header[4];
        for (int i = 0; i < 4; i++) {
            if (numBits >= 8) header[i] = (u8)Get(8);
            else { if (src >= srcEnd) return false; header[i] = *src++; }
        }
        u32 len = header[0] | (header[1] << 8), nlen = header[2] | (header[3] << 8);
        if ((len ^ 0xFFFF) != nlen) return false;
        while (len && numBits >= 8) { if (out >= dstEnd) return false; *out++ = (u8)Get(8); len--; }
        if (len > (size_t)(srcEnd - src) || len > (size_t)(dstEnd - out)) return false;
        memcpy(out, src, len);
        out += len;
        src += len;
        return true;
    }

    bool Dynamic() {
        static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        u8 lengths[286 + 32 + 137], clen[19] = {};
        int hlit = Get(5) + 257, hdist = Get(5) + 1, hclen = Get(4) + 4;
        for (int i = 0; i < hclen; i++) clen[order[i]] = (u8)Get(3);
        Huffman ch;
        if (!BuildHuffman(ch, clen, 19)) return false;
        int n = 0, total = hlit + hdist;
        while (n < total) {
            int c = Decode(ch);
            if (c < 0 || c >= 19) return false;
            if (c < 16) lengths[n++] = (u8)c;
            else {
                u8 fill = 0;
                int rep;
                if (c == 16) { rep = Get(2) + 3; if (!n) return false; fill = lengths[n - 1]; }
                else if (c == 17) rep = Get(3) + 3;
                else rep = Get(7) + 11;
                if (total - n < rep) return false;
                memset(lengths + n, fill, rep);
                n += rep;
            }
        }
        return BuildHuffman(lit, lengths, hlit) && BuildHuffman(dist, lengths + hlit, hdist);
    }

    void Fixed() {
        u8 l[288], d[32];
        int i = 0;
        for (; i <= 143; i++) l[i] = 8;
        for (; i <= 255; i++) l[i] = 9;
        for (; i <= 279; i++) l[i] = 7;
        for (; i <= 287; i++) l[i] = 8;
        for (i = 0; i < 32; i++) d[i] = 5;
        BuildHuffman(lit, l, 288);
        BuildHuffman(dist, d, 32);
    }

    bool Codes() {
        static const u16 lenBase[31] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0};
        static const u8 lenExtra[31] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0, 0};
        static const u16 distBase[32] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 0, 0};
        static const u8 distExtra[32] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int z = Decode(lit);
            if (z < 0) return false;
            if (z < 256) {
                if (out >= dstEnd) return false;
                *out++ = (u8)z;
                continue;
            }
            if (z == 256) return overrun <= 8;
            z -= 257;
            if (z >= 29) return false;
            int len = lenBase[z] + (lenExtra[z] ? (int)Get(lenExtra[z]) : 0);
            int dz = Decode(dist);
            if (dz < 0 || dz >= 30) return false;
            size_t d = distBase[dz] + (distExtra[dz] ? Get(distExtra[dz]) : 0);
            if (d > (size_t)(out - dst) || (size_t)len > (size_t)(dstEnd - out)) return false;
            const u8* p = out - d;
            if (d == 1) { memset(out, *p, len); out += len; }
            else while (len--) *out++ = *p++;
        }
    }

    bool Run() {
        for (;;) {
            int final = Get(1), type = Get(2);
            if (type == 0) { if (!Stored()) return false; }
            else if (type == 3) return false;
            else {
                if (type == 1) Fixed();
                else if (!Dynamic()) return false;
                if (!Codes()) return false;
            }
            if (final) return true;
        }
    }
};
}

bool InflateZlib(const u8* src, size_t srcSize, u8* dst, size_t dstSize) {
    if (srcSize < 2) return false;
    size_t skip = 0;
    if ((src[0] & 0x0F) == 8 && ((src[0] << 8) | src[1]) % 31 == 0) skip = (src[1] & 0x20) ? 6 : 2;   // zlib header (+ preset dict id)
    auto inf = std::make_unique<Inflater>();
    inf->src = src + skip;
    inf->srcEnd = src + srcSize;
    inf->dst = inf->out = dst;
    inf->dstEnd = dst + dstSize;
    return inf->Run() && (size_t)(inf->out - dst) == dstSize;
}

// ---------------------------------------------------------------- LZ4 block
bool DecompressLZ4Block(const u8* src, size_t srcSize, u8* dst, size_t dstSize) {
    const u8* ip = src;
    const u8* ie = src + srcSize;
    u8* op = dst;
    u8* oe = dst + dstSize;
    while (ip < ie) {
        u8 tok = *ip++;
        size_t lit = tok >> 4;
        if (lit == 15) {
            u8 b;
            do { if (ip >= ie) return false; b = *ip++; lit += b; } while (b == 255);
        }
        if (lit > (size_t)(ie - ip) || lit > (size_t)(oe - op)) return false;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip >= ie || op >= oe) break;   // last sequence carries literals only
        if (ie - ip < 2) return false;
        size_t off = ip[0] | (ip[1] << 8);
        ip += 2;
        size_t ml = tok & 15;
        if (ml == 15) {
            u8 b;
            do { if (ip >= ie) return false; b = *ip++; ml += b; } while (b == 255);
        }
        ml += 4;
        if (off == 0 || off > (size_t)(op - dst)) return false;
        if (ml > (size_t)(oe - op)) ml = oe - op;
        const u8* m = op - off;
        if (off >= ml) { memcpy(op, m, ml); op += ml; }
        else while (ml--) *op++ = *m++;
    }
    return op == oe;
}

// ---------------------------------------------------------------- Oodle (runtime loaded)
namespace {
using OodleDecompressFn = intptr_t(__stdcall*)(const void* comp, intptr_t compSize, void* raw, intptr_t rawSize, int fuzzSafe, int checkCrc, int verbosity,
                                               void* decBufBase, intptr_t decBufSize, void* cb, void* cbData, void* decoderMemory, intptr_t decoderMemorySize, int threadPhase);
std::mutex g_oodleMutex;
OodleDecompressFn g_oodle = nullptr;
}

bool LoadOodle(const std::wstring& dllPath) {
    std::lock_guard lk(g_oodleMutex);
    if (g_oodle) return true;
    HMODULE m = LoadLibraryW(dllPath.c_str());
    if (!m) return false;
    g_oodle = (OodleDecompressFn)GetProcAddress(m, "OodleLZ_Decompress");
    if (!g_oodle) { FreeLibrary(m); return false; }
    LOGI("Oodle loaded from %s", Narrow(dllPath).c_str());
    return true;
}

bool OodleLoaded() { return g_oodle != nullptr; }

bool DecompressOodle(const u8* src, size_t srcSize, u8* dst, size_t dstSize) {
    if (!g_oodle) return false;
    intptr_t n = g_oodle(src, (intptr_t)srcSize, dst, (intptr_t)dstSize, 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    return n == (intptr_t)dstSize;
}

bool Decompress(Compression method, const u8* src, size_t srcSize, u8* dst, size_t dstSize) {
    switch (method) {
    case Compression::None:
        if (srcSize < dstSize) return false;
        memcpy(dst, src, dstSize);
        return true;
    case Compression::Zlib: return InflateZlib(src, srcSize, dst, dstSize);
    case Compression::Gzip: {
        if (srcSize < 18 || src[0] != 0x1F || src[1] != 0x8B) return false;
        size_t p = 10;
        u8 flg = src[3];
        if (flg & 4) { if (p + 2 > srcSize) return false; p += 2 + (src[p] | (src[p + 1] << 8)); }
        if (flg & 8) { while (p < srcSize && src[p]) p++; p++; }
        if (flg & 16) { while (p < srcSize && src[p]) p++; p++; }
        if (flg & 2) p += 2;
        if (p >= srcSize) return false;
        auto inf = std::make_unique<Inflater>();
        inf->src = src + p;
        inf->srcEnd = src + srcSize;
        inf->dst = inf->out = dst;
        inf->dstEnd = dst + dstSize;
        return inf->Run() && (size_t)(inf->out - dst) == dstSize;
    }
    case Compression::Oodle: return DecompressOodle(src, srcSize, dst, dstSize);
    case Compression::LZ4: return DecompressLZ4Block(src, srcSize, dst, dstSize);
    default: return false;
    }
}
