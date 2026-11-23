// Wwise audio decoding.
// Wwise Vorbis -> Ogg is a port of ww2ogg's Wwise_RIFF_Vorbis::generate_ogg (Copyright (c) 2002 Xiph.org Foundation,
// 2009-2016 Adam Gashlin, BSD licence: third_party/ww2ogg_COPYING.txt): Wwise strips the Vorbis identification and
// comment headers, replaces codebooks in the setup header by ids into a shared library and trims packet headers; the
// stream is rebuilt bit for bit and then decoded with stb_vorbis.
#include "audio/Wwise.h"
#include "audio/WwiseCodebooks.inc"
#include <cstring>
#include <stdexcept>
#include <functional>
#include <unordered_map>
#include <unordered_set>

extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);

namespace {

struct AudioError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

u16 Rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
u32 Rd32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

int ilog(unsigned v) {
    int r = 0;
    while (v) { r++; v >>= 1; }
    return r;
}

unsigned Quantvals(unsigned entries, unsigned dimensions) {
    int bits = ilog(entries);
    int vals = (int)(entries >> ((bits - 1) * (dimensions - 1) / dimensions));
    for (;;) {
        unsigned long acc = 1, acc1 = 1;
        for (unsigned i = 0; i < dimensions; i++) { acc *= vals; acc1 *= vals + 1; }
        if (acc <= entries && acc1 > entries) return vals;
        if (acc > entries) vals--;
        else vals++;
    }
}

// LSB-first bit reader over memory
struct BitReader {
    const u8* p;
    size_t size, bit = 0;
    BitReader(const u8* d, size_t n) : p(d), size(n) {}
    unsigned Get(unsigned n) {
        unsigned v = 0;
        for (unsigned i = 0; i < n; i++) {
            if (bit >= size * 8) throw AudioError("bitstream truncated");
            if (p[bit >> 3] & (1u << (bit & 7))) v |= 1u << i;
            bit++;
        }
        return v;
    }
    size_t BitsRead() const { return bit; }
};

u32 OggCrc(const u8* d, size_t n) {
    static u32 table[256];
    static bool init = false;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 r = i << 24;
            for (int k = 0; k < 8; k++) r = r & 0x80000000u ? (r << 1) ^ 0x04c11db7u : r << 1;
            table[i] = r;
        }
        init = true;
    }
    u32 crc = 0;
    for (size_t i = 0; i < n; i++) crc = (crc << 8) ^ table[((crc >> 24) & 0xff) ^ d[i]];
    return crc;
}

// LSB-first bit writer producing Ogg pages (one packet per page, like ww2ogg)
struct OggWriter {
    Bytes out, payload;
    u8 cur = 0;
    unsigned bits = 0;
    bool first = true, continued = false;
    u32 granule = 0, seq = 0;
    void Put(unsigned v, unsigned n) {
        for (unsigned i = 0; i < n; i++) {
            if (v & (1u << i)) cur |= (u8)(1u << bits);
            if (++bits == 8) FlushBits();
        }
    }
    void FlushBits() {
        if (!bits) return;
        if (payload.size() >= 255 * 255) throw AudioError("ogg packet too large");
        payload.push_back(cur);
        cur = 0;
        bits = 0;
    }
    void FlushPage(bool nextContinued = false, bool last = false) {
        FlushBits();
        if (payload.empty()) return;
        unsigned segs = (unsigned)(payload.size() + 255) / 255;
        if (segs == 256) segs = 255;
        size_t start = out.size();
        out.resize(start + 27 + segs);
        u8* h = &out[start];
        memcpy(h, "OggS", 4);
        h[4] = 0;
        h[5] = (u8)((continued ? 1 : 0) | (first ? 2 : 0) | (last ? 4 : 0));
        u32 gh = granule == 0xFFFFFFFFu ? 0xFFFFFFFFu : 0;
        memcpy(h + 6, &granule, 4);
        memcpy(h + 10, &gh, 4);
        u32 serial = 1;
        memcpy(h + 14, &serial, 4);
        memcpy(h + 18, &seq, 4);
        memset(h + 22, 0, 4);
        h[26] = (u8)segs;
        size_t left = payload.size();
        for (unsigned i = 0; i < segs; i++) {
            h[27 + i] = (u8)(left >= 255 ? 255 : left);
            left = left >= 255 ? left - 255 : 0;
        }
        out.insert(out.end(), payload.begin(), payload.end());
        u32 crc = OggCrc(&out[start], out.size() - start);
        memcpy(&out[start + 22], &crc, 4);
        seq++;
        first = false;
        continued = nextContinued;
        payload.clear();
    }
};

void VorbisHeader(OggWriter& os, unsigned type) {
    os.Put(type, 8);
    for (char c : std::string("vorbis")) os.Put((u8)c, 8);
}

// codebook library (packed_codebooks_aoTuV_603.bin): data, then one u32 offset per codebook, then the offsets' offset
struct CodebookLibrary {
    const u8* data = kWwiseCodebooks;
    size_t size = sizeof(kWwiseCodebooks);
    u32 OffsetsAt() const { return Rd32(data + size - 4); }
    u32 Count() const { return (u32)((size - OffsetsAt()) / 4); }
    bool Get(u32 i, const u8*& cb, size_t& cbSize) const {
        u32 n = Count();
        if (i >= n - 1) return false;
        u32 a = Rd32(data + OffsetsAt() + i * 4), b = Rd32(data + OffsetsAt() + (i + 1) * 4);
        cb = data + a;
        cbSize = b - a;
        return true;
    }
};

// Wwise's compact codebook -> standard Vorbis codebook
void RebuildCodebook(BitReader& bis, size_t cbSize, OggWriter& os) {
    unsigned dimensions = bis.Get(4), entries = bis.Get(14);
    os.Put(0x564342, 24);
    os.Put(dimensions, 16);
    os.Put(entries, 24);
    unsigned ordered = bis.Get(1);
    os.Put(ordered, 1);
    if (ordered) {
        os.Put(bis.Get(5), 5);
        unsigned cur = 0;
        while (cur < entries) {
            unsigned n = ilog(entries - cur);
            unsigned num = bis.Get(n);
            os.Put(num, n);
            cur += num;
        }
        if (cur > entries) throw AudioError("codebook entry out of range");
    } else {
        unsigned lenLen = bis.Get(3), sparse = bis.Get(1);
        if (lenLen == 0 || lenLen > 5) throw AudioError("nonsense codeword length");
        os.Put(sparse, 1);
        for (unsigned i = 0; i < entries; i++) {
            bool present = true;
            if (sparse) {
                unsigned pr = bis.Get(1);
                os.Put(pr, 1);
                present = pr != 0;
            }
            if (present) os.Put(bis.Get(lenLen), 5);
        }
    }
    unsigned lookup = bis.Get(1);
    os.Put(lookup, 4);
    if (lookup == 1) {
        os.Put(bis.Get(32), 32);
        os.Put(bis.Get(32), 32);
        unsigned valueLen = bis.Get(4);
        os.Put(valueLen, 4);
        os.Put(bis.Get(1), 1);
        unsigned q = Quantvals(entries, dimensions);
        for (unsigned i = 0; i < q; i++) os.Put(bis.Get(valueLen + 1), valueLen + 1);
    }
    if (cbSize && bis.BitsRead() / 8 + 1 != cbSize) throw AudioError("codebook size mismatch");
}

void CopyCodebook(BitReader& bis, OggWriter& os) {
    unsigned id = bis.Get(24), dimensions = bis.Get(16), entries = bis.Get(24);
    if (id != 0x564342) throw AudioError("invalid codebook identifier");
    os.Put(id, 24);
    os.Put(dimensions, 16);
    os.Put(entries, 24);
    unsigned ordered = bis.Get(1);
    os.Put(ordered, 1);
    if (ordered) {
        os.Put(bis.Get(5), 5);
        unsigned cur = 0;
        while (cur < entries) {
            unsigned n = ilog(entries - cur), num = bis.Get(n);
            os.Put(num, n);
            cur += num;
        }
    } else {
        unsigned sparse = bis.Get(1);
        os.Put(sparse, 1);
        for (unsigned i = 0; i < entries; i++) {
            bool present = true;
            if (sparse) {
                unsigned pr = bis.Get(1);
                os.Put(pr, 1);
                present = pr != 0;
            }
            if (present) os.Put(bis.Get(5), 5);
        }
    }
    unsigned lookup = bis.Get(4);
    os.Put(lookup, 4);
    if (lookup == 1) {
        os.Put(bis.Get(32), 32);
        os.Put(bis.Get(32), 32);
        unsigned valueLen = bis.Get(4);
        os.Put(valueLen, 4);
        os.Put(bis.Get(1), 1);
        unsigned q = Quantvals(entries, dimensions);
        for (unsigned i = 0; i < q; i++) os.Put(bis.Get(valueLen + 1), valueLen + 1);
    } else if (lookup != 0) {
        throw AudioError("unsupported codebook lookup type");
    }
}

struct Riff {
    const u8* d = nullptr;
    size_t size = 0;
    long fmt = -1, fmtSize = -1, vorb = -1, vorbSize = -1, data = -1, dataSize = -1, smpl = -1;
};

Riff ParseRiff(const u8* d, size_t size) {
    Riff r;
    r.d = d;
    r.size = size;
    if (size < 12 || memcmp(d, "RIFF", 4) != 0) throw AudioError(size >= 4 && memcmp(d, "RIFX", 4) == 0 ? "big endian RIFX is not supported" : "not a RIFF wave");
    if (memcmp(d + 8, "WAVE", 4) != 0) throw AudioError("missing WAVE");
    size_t riffSize = std::min<size_t>(size, (size_t)Rd32(d + 4) + 8);
    size_t off = 12;
    while (off + 8 <= riffSize) {
        u32 csize = Rd32(d + off + 4);
        long body = (long)off + 8;
        if (!memcmp(d + off, "fmt ", 4)) { r.fmt = body; r.fmtSize = csize; }
        else if (!memcmp(d + off, "vorb", 4)) { r.vorb = body; r.vorbSize = csize; }
        else if (!memcmp(d + off, "data", 4)) { r.data = body; r.dataSize = csize; }
        else if (!memcmp(d + off, "smpl", 4)) r.smpl = body;
        off += 8 + (size_t)csize;
    }
    if (r.fmt < 0 || r.data < 0) throw AudioError("expected fmt and data chunks");
    if ((size_t)(r.data + r.dataSize) > size) r.dataSize = (long)(size - r.data);
    return r;
}

Bytes ConvertVorbis(const u8* d, size_t size) {
    Riff r = ParseRiff(d, size);
    if (r.vorb < 0 && r.fmtSize != 0x42) throw AudioError("expected 0x42 fmt if vorb missing");
    if (r.vorb >= 0 && r.fmtSize != 0x28 && r.fmtSize != 0x18 && r.fmtSize != 0x12) throw AudioError("bad fmt size");
    if (r.vorb < 0) r.vorb = r.fmt + 0x18;   // vorb data inside fmt (vorbSize stays -1)
    const u8* f = d + r.fmt;
    if (Rd16(f) != 0xFFFF) throw AudioError("not Wwise Vorbis");
    unsigned channels = Rd16(f + 2), sampleRate = Rd32(f + 4), avgBytes = Rd32(f + 8);
    // vorb
    const u8* v = d + r.vorb;
    long vs = r.vorbSize;
    if (vs != -1 && vs != 0x28 && vs != 0x2A && vs != 0x2C && vs != 0x32 && vs != 0x34) throw AudioError("bad vorb size");
    bool noGranule = false, modPackets = false, headerTriad = false, oldHeaders = false;
    u32 setupOffset, firstAudioOffset, b0 = 0, b1 = 0, sampleCount = Rd32(v);
    long p;
    if (vs == -1 || vs == 0x2A) {
        noGranule = true;
        u32 modSignal = Rd32(v + 4);
        if (modSignal != 0x4A && modSignal != 0x4B && modSignal != 0x69 && modSignal != 0x70) modPackets = true;
        p = 0x10;
    } else {
        p = 0x18;
    }
    setupOffset = Rd32(v + p);
    firstAudioOffset = Rd32(v + p + 4);
    if (vs == 0x28 || vs == 0x2C) {
        headerTriad = oldHeaders = true;
    } else {
        long q = (vs == -1 || vs == 0x2A) ? 0x24 : 0x2C;
        b0 = v[q + 4];
        b1 = v[q + 5];
    }
    if (headerTriad) throw AudioError("Wwise Vorbis with an inline header triad is not supported");

    const u8* data = d + r.data;
    long dataSize = r.dataSize;
    auto pktSize = [&](long off) { return (u32)Rd16(data + off); };
    auto pktGranule = [&](long off) { return noGranule ? 0u : Rd32(data + off + 2); };
    long hdr = noGranule ? 2 : 6;
    (void)oldHeaders;

    OggWriter os;
    std::vector<bool> modeBlockflag;
    int modeBits = 0;
    // identification
    VorbisHeader(os, 1);
    os.Put(0, 32);
    os.Put(channels, 8);
    os.Put(sampleRate, 32);
    os.Put(0, 32);
    os.Put(avgBytes * 8, 32);
    os.Put(0, 32);
    os.Put(b0, 4);
    os.Put(b1, 4);
    os.Put(1, 1);
    os.FlushPage();
    // comment
    VorbisHeader(os, 3);
    const char vendor[] = "converted from Audiokinetic Wwise by SOTModel (ww2ogg)";
    os.Put((unsigned)strlen(vendor), 32);
    for (const char* c = vendor; *c; c++) os.Put((u8)*c, 8);
    os.Put(0, 32);
    os.Put(1, 1);
    os.FlushPage();
    // setup
    {
        VorbisHeader(os, 5);
        long sp = setupOffset;
        if (sp + hdr > dataSize) throw AudioError("setup packet out of range");
        u32 ssize = pktSize(sp);
        if (pktGranule(sp) != 0) throw AudioError("setup packet granule != 0");
        if (sp + hdr + (long)ssize > dataSize) throw AudioError("setup packet truncated");
        BitReader ss(data + sp + hdr, ssize);
        unsigned cbCountLess1 = ss.Get(8), cbCount = cbCountLess1 + 1;
        os.Put(cbCountLess1, 8);
        CodebookLibrary lib;
        for (unsigned i = 0; i < cbCount; i++) {
            unsigned id = ss.Get(10);
            const u8* cb;
            size_t cbSize;
            if (!lib.Get(id, cb, cbSize)) throw AudioError(Fmt("invalid codebook id %u", id));
            BitReader cbr(cb, cbSize);
            RebuildCodebook(cbr, cbSize, os);
        }
        os.Put(0, 6);    // time domain transforms
        os.Put(0, 16);
        // floors
        unsigned floorCountLess1 = ss.Get(6), floorCount = floorCountLess1 + 1;
        os.Put(floorCountLess1, 6);
        for (unsigned i = 0; i < floorCount; i++) {
            os.Put(1, 16);   // floor type 1
            unsigned partitions = ss.Get(5);
            os.Put(partitions, 5);
            std::vector<unsigned> classList(partitions);
            unsigned maxClass = 0;
            for (unsigned j = 0; j < partitions; j++) {
                unsigned c = ss.Get(4);
                os.Put(c, 4);
                classList[j] = c;
                maxClass = std::max(maxClass, c);
            }
            std::vector<unsigned> classDims(maxClass + 1);
            for (unsigned j = 0; j <= maxClass; j++) {
                unsigned dimLess1 = ss.Get(3);
                os.Put(dimLess1, 3);
                classDims[j] = dimLess1 + 1;
                unsigned subclasses = ss.Get(2);
                os.Put(subclasses, 2);
                if (subclasses) {
                    unsigned master = ss.Get(8);
                    os.Put(master, 8);
                    if (master >= cbCount) throw AudioError("invalid floor1 masterbook");
                }
                for (unsigned k = 0; k < (1u << subclasses); k++) {
                    unsigned book = ss.Get(8);
                    os.Put(book, 8);
                    if ((int)book - 1 >= (int)cbCount) throw AudioError("invalid floor1 subclass book");
                }
            }
            os.Put(ss.Get(2), 2);   // multiplier
            unsigned rangebits = ss.Get(4);
            os.Put(rangebits, 4);
            for (unsigned j = 0; j < partitions; j++)
                for (unsigned k = 0; k < classDims[classList[j]]; k++) os.Put(ss.Get(rangebits), rangebits);
        }
        // residues
        unsigned residueCountLess1 = ss.Get(6), residueCount = residueCountLess1 + 1;
        os.Put(residueCountLess1, 6);
        for (unsigned i = 0; i < residueCount; i++) {
            unsigned type = ss.Get(2);
            os.Put(type, 16);
            if (type > 2) throw AudioError("invalid residue type");
            unsigned begin = ss.Get(24), end = ss.Get(24), partSize = ss.Get(24), classificationsLess1 = ss.Get(6), classbook = ss.Get(8);
            os.Put(begin, 24);
            os.Put(end, 24);
            os.Put(partSize, 24);
            os.Put(classificationsLess1, 6);
            os.Put(classbook, 8);
            if (classbook >= cbCount) throw AudioError("invalid residue classbook");
            unsigned classifications = classificationsLess1 + 1;
            std::vector<unsigned> cascade(classifications);
            for (unsigned j = 0; j < classifications; j++) {
                unsigned high = 0, low = ss.Get(3);
                os.Put(low, 3);
                unsigned flag = ss.Get(1);
                os.Put(flag, 1);
                if (flag) {
                    high = ss.Get(5);
                    os.Put(high, 5);
                }
                cascade[j] = high * 8 + low;
            }
            for (unsigned j = 0; j < classifications; j++)
                for (unsigned k = 0; k < 8; k++)
                    if (cascade[j] & (1u << k)) {
                        unsigned book = ss.Get(8);
                        os.Put(book, 8);
                        if (book >= cbCount) throw AudioError("invalid residue book");
                    }
        }
        // mappings
        unsigned mappingCountLess1 = ss.Get(6), mappingCount = mappingCountLess1 + 1;
        os.Put(mappingCountLess1, 6);
        for (unsigned i = 0; i < mappingCount; i++) {
            os.Put(0, 16);
            unsigned submapsFlag = ss.Get(1);
            os.Put(submapsFlag, 1);
            unsigned submaps = 1;
            if (submapsFlag) {
                unsigned s = ss.Get(4);
                os.Put(s, 4);
                submaps = s + 1;
            }
            unsigned squarePolar = ss.Get(1);
            os.Put(squarePolar, 1);
            if (squarePolar) {
                unsigned stepsLess1 = ss.Get(8);
                os.Put(stepsLess1, 8);
                unsigned bits = ilog(channels - 1);
                for (unsigned j = 0; j <= stepsLess1; j++) {
                    unsigned mag = ss.Get(bits), ang = ss.Get(bits);
                    os.Put(mag, bits);
                    os.Put(ang, bits);
                    if (ang == mag || mag >= channels || ang >= channels) throw AudioError("invalid coupling");
                }
            }
            unsigned reserved = ss.Get(2);
            os.Put(reserved, 2);
            if (reserved) throw AudioError("mapping reserved field nonzero");
            if (submaps > 1)
                for (unsigned j = 0; j < channels; j++) {
                    unsigned mux = ss.Get(4);
                    os.Put(mux, 4);
                    if (mux >= submaps) throw AudioError("mapping_mux >= submaps");
                }
            for (unsigned j = 0; j < submaps; j++) {
                os.Put(ss.Get(8), 8);   // time config
                unsigned floorN = ss.Get(8), residueN = ss.Get(8);
                os.Put(floorN, 8);
                os.Put(residueN, 8);
                if (floorN >= floorCount || residueN >= residueCount) throw AudioError("invalid mapping");
            }
        }
        // modes
        unsigned modeCountLess1 = ss.Get(6), modeCount = modeCountLess1 + 1;
        os.Put(modeCountLess1, 6);
        modeBlockflag.resize(modeCount);
        modeBits = ilog(modeCount - 1);
        for (unsigned i = 0; i < modeCount; i++) {
            unsigned blockFlag = ss.Get(1);
            os.Put(blockFlag, 1);
            modeBlockflag[i] = blockFlag != 0;
            os.Put(0, 16);
            os.Put(0, 16);
            unsigned mapping = ss.Get(8);
            os.Put(mapping, 8);
            if (mapping >= mappingCount) throw AudioError("invalid mode mapping");
        }
        os.Put(1, 1);
        os.FlushPage();
        if ((ss.BitsRead() + 7) / 8 != ssize) throw AudioError("did not read exactly the setup packet");
        if (sp + hdr + (long)ssize != (long)firstAudioOffset) throw AudioError("first audio packet does not follow the setup packet");
    }
    // audio packets
    bool prevBlockflag = false;
    long off = firstAudioOffset;
    while (off < dataSize) {
        if (off + hdr > dataSize) throw AudioError("packet header truncated");
        u32 size = pktSize(off), granule = pktGranule(off);
        long payload = off + hdr, next = payload + size;
        if (next > dataSize) throw AudioError("packet truncated");
        os.granule = granule == 0xFFFFFFFFu ? 1 : granule;
        const u8* pk = data + payload;
        if (modPackets) {
            if (modeBlockflag.empty()) throw AudioError("no mode block flags");
            os.Put(0, 1);   // audio packet
            BitReader br(pk, size);
            unsigned mode = br.Get(modeBits);
            os.Put(mode, modeBits);
            unsigned remainder = br.Get(8 - modeBits);
            if (mode >= modeBlockflag.size()) throw AudioError("invalid mode");
            if (modeBlockflag[mode]) {   // long window: previous / next window flags
                bool nextFlag = false;
                if (next + hdr <= dataSize) {
                    u32 nsize = pktSize(next);
                    if (nsize > 0 && next + hdr < dataSize) {
                        BitReader nr(data + next + hdr, nsize);
                        unsigned nmode = nr.Get(modeBits);
                        if (nmode < modeBlockflag.size()) nextFlag = modeBlockflag[nmode];
                    }
                }
                os.Put(prevBlockflag, 1);
                os.Put(nextFlag, 1);
            }
            prevBlockflag = modeBlockflag[mode];
            os.Put(remainder, 8 - modeBits);
        } else {
            if (size) os.Put(pk[0], 8);
        }
        for (u32 i = 1; i < size; i++) os.Put(pk[i], 8);
        off = next;
        // without per packet granules the end-of-stream page still needs the length, or decoders trim the last packet
        if (off == dataSize && noGranule && sampleCount) os.granule = sampleCount;
        os.FlushPage(false, off == dataSize);
    }
    os.FlushPage();
    return std::move(os.out);
}

// Wwise IMA ADPCM: per frame, each channel's block (4 byte header + nibbles) one after another
bool DecodeImaAdpcm(const Riff& r, AudioPcm& out) {
    static const int steps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
                                  157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
                                  1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
                                  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    static const int idxTab[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
    const u8* f = r.d + r.fmt;
    int ch = Rd16(f + 2);
    int blockAlign = Rd16(f + 12);
    if (ch <= 0 || blockAlign <= 4 * ch) return false;
    int perCh = blockAlign / ch;
    int samplesPerBlock = (perCh - 4) * 2 + 1;
    const u8* d = r.d + r.data;
    long blocks = r.dataSize / blockAlign;
    out.channels = ch;
    out.sampleRate = (int)Rd32(f + 4);
    out.samples.assign((size_t)blocks * samplesPerBlock * ch, 0);
    for (long b = 0; b < blocks; b++)
        for (int c = 0; c < ch; c++) {
            const u8* blk = d + b * blockAlign + c * perCh;
            int pred = (i16)Rd16(blk), idx = std::clamp((int)blk[2], 0, 88);
            size_t base = (size_t)b * samplesPerBlock;
            out.samples[(base) * ch + c] = (i16)pred;
            for (int s = 1; s < samplesPerBlock; s++) {
                int byte = blk[4 + (s - 1) / 2];
                int nib = (s - 1) & 1 ? byte >> 4 : byte & 15;
                int step = steps[idx], diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                pred = std::clamp(nib & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + idxTab[nib], 0, 88);
                out.samples[(base + s) * ch + c] = (i16)pred;
            }
        }
    return true;
}

}   // namespace

bool ReadWemInfo(const u8* data, size_t size, WemInfo& out, std::string* error) {
    try {
        Riff r = ParseRiff(data, size);
        const u8* f = data + r.fmt;
        out.codec = Rd16(f);
        out.channels = Rd16(f + 2);
        out.sampleRate = (int)Rd32(f + 4);
        switch (out.codec) {
        case 0xFFFF: out.codecName = "Wwise Vorbis"; break;
        case 0x0001:
        case 0xFFFE: out.codecName = "PCM"; break;
        case 0x0002: out.codecName = "Wwise IMA ADPCM"; break;
        case 0x3039:
        case 0x3040: out.codecName = "Opus"; break;
        case 0x0166: out.codecName = "XMA2"; break;
        default: out.codecName = Fmt("codec 0x%04X", out.codec); break;
        }
        if (out.codec == 0xFFFF) {
            long v = r.vorb >= 0 ? r.vorb : r.fmt + 0x18;
            out.sampleCount = Rd32(data + v);
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool WemToOgg(const u8* data, size_t size, Bytes& ogg, std::string* error) {
    try {
        ogg = ConvertVorbis(data, size);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool DecodeWem(const u8* data, size_t size, AudioPcm& out, std::string* error) {
    try {
        Riff r = ParseRiff(data, size);
        u16 codec = Rd16(data + r.fmt);
        if (codec == 0xFFFF) {
            Bytes ogg = ConvertVorbis(data, size);
            int ch = 0, rate = 0;
            short* pcm = nullptr;
            int frames = stb_vorbis_decode_memory(ogg.data(), (int)ogg.size(), &ch, &rate, &pcm);
            if (frames < 0 || !pcm) throw AudioError("Vorbis decoding failed");
            out.channels = ch;
            out.sampleRate = rate;
            // the header knows the exact length; the last Vorbis packet decodes past it
            WemInfo info;
            if (ReadWemInfo(data, size, info) && info.sampleCount && (u32)frames > info.sampleCount) frames = (int)info.sampleCount;
            out.samples.assign(pcm, pcm + (size_t)frames * ch);
            free(pcm);
            return true;
        }
        if (codec == 0x0001 || codec == 0xFFFE) {
            int bits = Rd16(data + r.fmt + 14);
            if (bits != 16) throw AudioError(Fmt("%d bit PCM is not supported", bits));
            out.channels = Rd16(data + r.fmt + 2);
            out.sampleRate = (int)Rd32(data + r.fmt + 4);
            out.samples.resize(r.dataSize / 2);
            memcpy(out.samples.data(), data + r.data, out.samples.size() * 2);
            return true;
        }
        if (codec == 0x0002) {
            if (!DecodeImaAdpcm(r, out)) throw AudioError("bad IMA ADPCM block layout");
            return true;
        }
        WemInfo info;
        ReadWemInfo(data, size, info);
        throw AudioError(info.codecName + " is not supported yet");
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

Bytes MakeWav(const AudioPcm& pcm) {
    u32 dataBytes = (u32)(pcm.samples.size() * 2);
    Bytes w(44 + dataBytes);
    auto put32 = [&](size_t o, u32 v) { memcpy(&w[o], &v, 4); };
    auto put16 = [&](size_t o, u16 v) { memcpy(&w[o], &v, 2); };
    memcpy(&w[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    memcpy(&w[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, (u16)pcm.channels);
    put32(24, (u32)pcm.sampleRate);
    put32(28, (u32)(pcm.sampleRate * pcm.channels * 2));
    put16(32, (u16)(pcm.channels * 2));
    put16(34, 16);
    memcpy(&w[36], "data", 4);
    put32(40, dataBytes);
    memcpy(&w[44], pcm.samples.data(), dataBytes);
    return w;
}

const HircObject* BankInfo::Find(u32 id) const {
    for (auto& o : hirc)
        if (o.id == id) return &o;
    return nullptr;
}

std::vector<u32> BankInfo::Events() const {
    std::vector<u32> r;
    for (auto& o : hirc)
        if (o.type == 4) r.push_back(o.id);
    return r;
}

u32 WwiseShortId(std::string_view name) {
    u32 h = 2166136261u;
    for (char c : name) {
        h *= 16777619u;
        h ^= (u8)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return h;
}

bool WwiseEventTree::Plays(size_t i, const std::unordered_map<u32, u32>& choice) const {
    if (i >= conditions.size()) return true;
    for (auto& c : conditions[i]) {
        auto it = choice.find(c.group);
        if (it != choice.end() && std::find(c.values.begin(), c.values.end(), it->second) == c.values.end()) return false;
    }
    return true;
}

bool ResolveWwiseEvent(const u8* data, const BankInfo& bank, u32 eventId, std::vector<u32>& media, std::string* error) {
    WwiseEventTree tree;
    bool ok = ResolveWwiseEvent(data, bank, eventId, tree, error);
    media = std::move(tree.media);
    return ok;
}

bool ResolveWwiseEvent(const u8* data, const BankInfo& bank, u32 eventId, WwiseEventTree& tree, std::string* error) {
    tree = WwiseEventTree{};
    const HircObject* ev = bank.Find(eventId);
    if (!ev || ev->type != 4) {
        if (error) *error = Fmt("event %u is not in bank %u", eventId, bank.bankId);
        return false;
    }
    std::unordered_map<u32, const HircObject*> byId;
    for (auto& o : bank.hirc) byId[o.id] = &o;
    // CAkEvent: action count (u32 up to bank version 122, then a variable-length int), action ids
    const u8* b = data + ev->offset;
    size_t p = 0, count = 0;
    if (bank.version <= 122) {
        if (ev->size < 4) return false;
        count = Rd32(b);
        p = 4;
    } else {
        for (int k = 0; k < 5 && p < ev->size; k++) {
            u8 c = b[p++];
            count = (count << 7) | (c & 0x7f);
            if (!(c & 0x80)) break;
        }
    }
    std::vector<u32> targets;
    for (size_t k = 0; k < count && p + 4 <= ev->size; k++, p += 4) {
        auto it = byId.find(Rd32(b + p));
        if (it == byId.end() || it->second->type != 3 || it->second->size < 6) continue;
        const u8* a = data + it->second->offset;
        u16 actionType = (u16)(a[0] | (a[1] << 8));
        if ((actionType & 0xff00) == 0x0400) targets.push_back(Rd32(a + 2));   // Play: ulActionType 0x04xx, idExt = the played node
    }
    // nodes: children lists are "u32 count + count ids of nodes in this bank"; a node's lone parent id has no count in front
    auto isNode = [&](u32 id) {
        auto it = byId.find(id);
        if (it == byId.end()) return false;
        u8 t = it->second->type;
        return t == 2 || t == 5 || t == 6 || t == 9 || t == 10 || t == 11 || t == 12 || t == 13;
    };
    using Conds = std::vector<WwiseMediaCondition>;
    std::unordered_set<u32> seen;
    std::unordered_map<u32, size_t> mediaIndex;
    auto addMedia = [&](u32 id, const Conds& conds) {
        if (!id) return;
        auto [it, fresh] = mediaIndex.try_emplace(id, tree.media.size());
        if (fresh) {
            tree.media.push_back(id);
            tree.conditions.push_back(conds);
            return;
        }
        // reached again on another path: a group stays a condition only if both paths have it (values merged)
        Conds& old = tree.conditions[it->second];
        Conds merged;
        for (auto& c : old)
            for (auto& n : conds)
                if (n.group == c.group) {
                    WwiseMediaCondition m = c;
                    for (u32 v : n.values)
                        if (std::find(m.values.begin(), m.values.end(), v) == m.values.end()) m.values.push_back(v);
                    merged.push_back(std::move(m));
                }
        old = std::move(merged);
    };
    // children list at q of a node: the first run of "count + node ids"
    auto childList = [&](u32 id, const u8* d, size_t size, size_t& at) {
        for (size_t q = 0; q + 8 <= size; q++) {
            u32 n = Rd32(d + q);
            bool run = n >= 1 && n <= 512 && q + 4 + (size_t)n * 4 <= size;
            for (u32 k = 0; run && k < n; k++) {
                u32 c = Rd32(d + q + 4 + k * 4);
                run = c != id && isNode(c);
            }
            if (run) { at = q; return true; }
        }
        return false;
    };
    std::function<void(u32, int, const Conds&)> walk = [&](u32 id, int depth, const Conds& conds) {
        auto it = byId.find(id);
        if (depth > 24 || it == byId.end() || !seen.insert(id).second) return;
        tree.nodes.push_back(id);
        const HircObject& o = *it->second;
        const u8* d = data + o.offset;
        if (o.type == 2) {   // CAkSound: AkBankSourceData = u32 plugin, u8 stream type, u32 source id, ...
            if (o.size >= 9) addMedia(Rd32(d + 5), conds);
            return;
        }
        if (o.type == 11) {   // CAkMusicTrack: u8 flags, u32 source count, sources of 14 bytes (as above + u32 size, u8 bits)
            if (o.size < 5) return;
            u32 n = Rd32(d + 1);
            for (u32 k = 0; k < n && k < 256 && 5 + (size_t)(k + 1) * 14 <= o.size; k++) addMedia(Rd32(d + 5 + k * 14 + 5), conds);
            return;
        }
        if (o.type == 6) {
            // CAkSwitchCntr after the node params: u8 group type (1 state), u32 group, u32 default value, u8 continuous validation,
            // children (u32 count + ids), u32 switch package count, packages (u32 value, u32 count, node ids), switch params
            size_t q = 0;
            if (childList(id, d, o.size, q) && q >= 10) {
                u32 n = Rd32(d + q);
                std::vector<u32> children(n);
                for (u32 k = 0; k < n; k++) children[k] = Rd32(d + q + 4 + k * 4);
                size_t r = q + 4 + (size_t)n * 4;
                WwiseSwitchGroup g;
                g.state = d[q - 10] == 1;
                g.group = Rd32(d + q - 9);
                g.defaultValue = Rd32(d + q - 5);
                std::unordered_map<u32, std::vector<u32>> valuesOf;   // child -> the values it is assigned to
                bool ok = r + 4 <= o.size;
                u32 packages = ok ? Rd32(d + r) : 0;
                ok = ok && packages <= 4096;
                r += 4;
                for (u32 k = 0; ok && k < packages; k++) {
                    if (r + 8 > o.size) { ok = false; break; }
                    u32 value = Rd32(d + r), items = Rd32(d + r + 4);
                    r += 8;
                    if (items > n || r + (size_t)items * 4 > o.size) { ok = false; break; }
                    for (u32 j = 0; j < items; j++) {
                        u32 c = Rd32(d + r + j * 4);
                        if (std::find(children.begin(), children.end(), c) == children.end()) { ok = false; break; }
                        valuesOf[c].push_back(value);
                    }
                    r += (size_t)items * 4;
                    if (items && std::find(g.values.begin(), g.values.end(), value) == g.values.end()) g.values.push_back(value);
                }
                if (ok && g.group) {
                    auto known = std::find_if(tree.switches.begin(), tree.switches.end(), [&](const WwiseSwitchGroup& s) { return s.group == g.group; });
                    if (known == tree.switches.end()) tree.switches.push_back(g);
                    else
                        for (u32 v : g.values)
                            if (std::find(known->values.begin(), known->values.end(), v) == known->values.end()) known->values.push_back(v);
                    for (u32 c : children) {
                        Conds sub = conds;
                        auto same = std::find_if(sub.begin(), sub.end(), [&](const WwiseMediaCondition& m) { return m.group == g.group; });
                        if (same != sub.end()) {   // nested on the same group: only the values both allow
                            std::vector<u32> both;
                            for (u32 v : valuesOf[c])
                                if (std::find(same->values.begin(), same->values.end(), v) != same->values.end()) both.push_back(v);
                            same->values = both;
                        } else
                            sub.push_back({g.group, valuesOf[c]});
                        walk(c, depth + 1, sub);
                    }
                    return;
                }
            }
        }
        for (size_t q = 0; q + 8 <= o.size;) {
            u32 n = Rd32(d + q);
            bool run = n >= 1 && n <= 512 && q + 4 + (size_t)n * 4 <= o.size;
            for (u32 k = 0; run && k < n; k++) {
                u32 c = Rd32(d + q + 4 + k * 4);
                run = c != id && isNode(c);
            }
            if (!run) {
                q++;
                continue;
            }
            for (u32 k = 0; k < n; k++) walk(Rd32(d + q + 4 + k * 4), depth + 1, conds);
            q += 4 + (size_t)n * 4;
        }
    };
    for (u32 t : targets) walk(t, 0, {});
    if (targets.empty() && error) *error = "the event has no Play action";
    return !targets.empty();
}
bool ParseBank(const u8* data, size_t size, BankInfo& out, size_t* bankStart, std::string* error) {
    // the bank starts with its BKHD chunk; packages carry it after their own header
    size_t start = SIZE_MAX;
    for (size_t i = 0; i + 8 <= size; i++)
        if (data[i] == 'B' && !memcmp(data + i, "BKHD", 4) && Rd32(data + i + 4) >= 8 && Rd32(data + i + 4) < 4096) { start = i; break; }
    if (start == SIZE_MAX) {
        if (error) *error = "no BKHD chunk";
        return false;
    }
    if (bankStart) *bankStart = start;
    size_t didx = 0, didxSize = 0, dataChunk = SIZE_MAX;
    for (size_t off = start; off + 8 <= size;) {
        u32 csize = Rd32(data + off + 4);
        const u8* tag = data + off;
        if (!memcmp(tag, "BKHD", 4) && csize >= 8) { out.version = Rd32(data + off + 8); out.bankId = Rd32(data + off + 12); }
        else if (!memcmp(tag, "DIDX", 4)) { didx = off + 8; didxSize = csize; }
        else if (!memcmp(tag, "DATA", 4)) dataChunk = off + 8;
        else if (!memcmp(tag, "HIRC", 4) && csize >= 4) {
            out.hircObjects = Rd32(data + off + 8);
            size_t end = std::min(size, off + 8 + (size_t)csize);
            size_t p = off + 12;
            for (u32 k = 0; k < out.hircObjects && p + 9 <= end; k++) {   // u8 type, u32 size (from the id on), u32 id
                HircObject o;
                o.type = data[p];
                u32 osize = Rd32(data + p + 1);
                if (osize < 4 || p + 5 + (size_t)osize > end) break;
                o.id = Rd32(data + p + 5);
                o.offset = (u32)(p + 9);
                o.size = osize - 4;
                out.hirc.push_back(o);
                p += 5 + (size_t)osize;
            }
        }
        else if (memcmp(tag, "STID", 4) && memcmp(tag, "STMG", 4) && memcmp(tag, "ENVS", 4) && memcmp(tag, "INIT", 4) && memcmp(tag, "PLAT", 4) && memcmp(tag, "FXPR", 4))
            break;   // past the bank
        off += 8 + (size_t)csize;
    }
    if (didx && dataChunk != SIZE_MAX)
        for (size_t k = 0; k + 12 <= didxSize; k += 12) {
            BankWem w;
            w.id = Rd32(data + didx + k);
            w.offset = (u32)(dataChunk + Rd32(data + didx + k + 4));
            w.size = Rd32(data + didx + k + 8);
            if ((size_t)w.offset + w.size <= size) out.wems.push_back(w);
        }
    return true;
}
