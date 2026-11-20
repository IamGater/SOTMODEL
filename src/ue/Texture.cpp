#include "ue/Texture.h"
#include "ue/TextureDecode.h"
#include "core/Compression.h"

bool IsTextureClass(std::string_view c) {
    return c == "Texture2D" || c == "TextureCube" || c == "LightMapTexture2D" || c == "ShadowMapTexture2D" || c == "Texture2DDynamic" || c == "VirtualTexture2D";
}

bool ReadBulkDataHeader(const Package& pkg, Reader& r, BulkDataInfo& b) {
    b.flags = r.U32();
    b.elementCount = r.I32();
    b.sizeOnDisk = r.I32();
    b.offset = r.I64();
    if (b.flags & BULKDATA_PayloadAtEndOfFile) b.offset += pkg.Summary().bulkDataStartOffset;
    else {
        b.offset = (i64)r.Pos();   // inline payload follows
        if (b.sizeOnDisk > 0) r.Skip(b.sizeOnDisk);
    }
    return true;
}

bool LoadBulkData(const Package& pkg, const BulkDataInfo& b, Bytes& out, std::string* error) {
    const Bytes& d = pkg.Data();
    if (b.Unused()) { out.clear(); return true; }
    if (b.offset < 0 || (u64)b.offset + (u64)b.sizeOnDisk > d.size()) {
        if (error) *error = Fmt("bulk data out of range (offset %lld size %d, package %zu)", (long long)b.offset, b.sizeOnDisk, d.size());
        return false;
    }
    const u8* src = d.data() + b.offset;
    if (!(b.flags & BULKDATA_SerializeCompressedZLIB)) {
        out.assign(src, src + b.sizeOnDisk);
        return true;
    }
    // FArchive::SerializeCompressed: tag, chunk size, summary (compressed, uncompressed), chunk table, chunks
    try {
        Reader r(src, (size_t)b.sizeOnDisk);
        r.I64();                                     // PACKAGE_FILE_TAG
        i64 chunkSize = r.I64();
        i64 totalComp = r.I64(), totalRaw = r.I64();
        (void)totalComp;
        if (chunkSize <= 0 || totalRaw < 0 || totalRaw > (1ll << 31)) throw ParseError("bad compressed bulk header");
        size_t n = (size_t)((totalRaw + chunkSize - 1) / chunkSize);
        std::vector<std::pair<i64, i64>> chunks(n);
        for (auto& c : chunks) { c.first = r.I64(); c.second = r.I64(); }
        out.resize((size_t)totalRaw);
        size_t at = 0;
        for (auto& [cs, us] : chunks) {
            r.Need((size_t)cs);
            if (!InflateZlib(r.Here(), (size_t)cs, out.data() + at, (size_t)us)) throw ParseError("zlib bulk chunk failed");
            r.Skip(cs);
            at += (size_t)us;
        }
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    return true;
}

bool ReadTextureInfo(const Package& pkg, size_t exportIdx, TextureInfo& t, std::string* error) {
    try {
        t.className = pkg.ExportClass(exportIdx);
        size_t end = 0;
        std::string perr;
        PropertyList props = pkg.ReadProperties(exportIdx, &end, &perr);
        t.srgb = PropBool(props, "SRGB", true);
        t.compressionSettings = PropStr(props, "CompressionSettings");
        t.lodGroup = PropStr(props, "LODGroup");
        t.isNormalMap = t.compressionSettings == "TC_Normalmap";
        Reader r = pkg.ExportReader(exportIdx);
        r.Seek(end);
        if (r.U32()) r.Skip(16);   // object guid
        r.Skip(2);                 // UTexture strip flags
        r.Skip(2);                 // UTexture2D / Cube strip flags
        bool cooked = r.U32() != 0;
        if (!cooked) throw ParseError("texture has no cooked data");
        for (;;) {
            std::string fmt = pkg.ReadName(r);
            if (fmt == "None") break;
            i32 skip = r.I32();
            if (!t.pixelFormat.empty()) { r.Seek((size_t)skip); continue; }   // only the first platform format
            t.sizeX = r.I32();
            t.sizeY = r.I32();
            t.numSlices = r.I32();
            t.pixelFormat = r.FString();
            r.I32();   // first mip
            i32 nm = r.Count(20);
            for (i32 m = 0; m < nm; m++) {
                TextureMip mip;
                r.U32();   // bCooked
                ReadBulkDataHeader(pkg, r, mip.bulk);
                mip.sizeX = r.I32();
                mip.sizeY = r.I32();
                t.mips.push_back(mip);
            }
            r.Seek((size_t)skip);   // Athena carries 4 extra bytes after the mips (CUE4Parse); the skip offset covers them
        }
        if (t.mips.empty()) throw ParseError("no mips");
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    return true;
}

bool DecodeTexture(const Package& pkg, const TextureInfo& info, int mip, int maxDim, Image& out, std::string* error, int* usedMip) {
    if (info.mips.empty()) { if (error) *error = "no mips"; return false; }
    int m = mip;
    if (m < 0) {
        m = 0;
        while (m + 1 < (int)info.mips.size() && (info.mips[m].sizeX > maxDim || info.mips[m].sizeY > maxDim || info.mips[m].bulk.Unused())) m++;
    }
    m = std::clamp(m, 0, (int)info.mips.size() - 1);
    while (m + 1 < (int)info.mips.size() && info.mips[m].bulk.Unused()) m++;   // stripped top mips
    if (usedMip) *usedMip = m;
    Bytes data;
    if (!LoadBulkData(pkg, info.mips[m].bulk, data, error)) return false;
    int slices = info.className == "TextureCube" ? 6 : 1;
    return DecodePixels(info.pixelFormat, data.data(), data.size(), info.mips[m].sizeX, info.mips[m].sizeY, slices, out, error);
}
