#pragma once
#include "ue/Package.h"

// Bulk data record (4.10): flags, element count, size on disk, offset (relative to BulkDataStartOffset when at end of file).
struct BulkDataInfo {
    u32 flags = 0;
    i32 elementCount = 0;
    i32 sizeOnDisk = 0;
    i64 offset = 0;          // absolute offset of the payload in the package
    bool Unused() const { return (flags & 0x20) != 0 || sizeOnDisk <= 0; }
};
enum : u32 { BULKDATA_PayloadAtEndOfFile = 1, BULKDATA_SerializeCompressedZLIB = 2, BULKDATA_Unused = 0x20 };

bool ReadBulkDataHeader(const Package& pkg, Reader& r, BulkDataInfo& out);   // advances past an inline payload
bool LoadBulkData(const Package& pkg, const BulkDataInfo& info, Bytes& out, std::string* error = nullptr);

struct TextureMip {
    int sizeX = 0, sizeY = 0;
    BulkDataInfo bulk;
};

struct TextureInfo {
    std::string className;
    std::string pixelFormat;   // "PF_DXT5"
    int sizeX = 0, sizeY = 0, numSlices = 1;
    bool srgb = true;
    bool isNormalMap = false;
    std::string compressionSettings, lodGroup;
    std::vector<TextureMip> mips;
};

bool ReadTextureInfo(const Package& pkg, size_t exportIdx, TextureInfo& out, std::string* error = nullptr);

// Decoded RGBA8 image of one mip (largest mip not above maxDim when mip < 0).
struct Image {
    int width = 0, height = 0;
    std::vector<u8> rgba;   // width*height*4 (all slices stacked vertically for cubes)
};
bool DecodeTexture(const Package& pkg, const TextureInfo& info, int mip, int maxDim, Image& out, std::string* error = nullptr, int* usedMip = nullptr);
bool DecodePixels(const std::string& pixelFormat, const u8* data, size_t size, int w, int h, int slices, Image& out, std::string* error);

bool IsTextureClass(std::string_view cls);
size_t PixelDataSize(const std::string& pixelFormat, int w, int h);   // 0 = unknown format
