#pragma once
#include "core/Common.h"

// Wwise audio as Sea of Thieves ships it: .wem streams (RIFF WAVE with Wwise codecs) and .bnk sound banks
// (WwiseBank packages carry the bank bytes) that embed further .wem files.

struct AudioPcm {
    int channels = 0, sampleRate = 0;
    std::vector<i16> samples;   // interleaved
    size_t Frames() const { return channels ? samples.size() / channels : 0; }
    double Seconds() const { return sampleRate ? (double)Frames() / sampleRate : 0; }
};

struct WemInfo {
    u16 codec = 0;              // 0xFFFF Wwise Vorbis, 0x0001/0xFFFE PCM, 0x0002 Wwise IMA ADPCM, ...
    int channels = 0, sampleRate = 0;
    u32 sampleCount = 0;        // when the header says so
    std::string codecName;
};

bool ReadWemInfo(const u8* data, size_t size, WemInfo& out, std::string* error = nullptr);
// Decodes to 16 bit PCM (Wwise Vorbis, PCM, Wwise IMA ADPCM).
bool DecodeWem(const u8* data, size_t size, AudioPcm& out, std::string* error = nullptr);
// Wwise Vorbis -> a standard Ogg Vorbis file (ww2ogg's conversion).
bool WemToOgg(const u8* data, size_t size, Bytes& ogg, std::string* error = nullptr);
Bytes MakeWav(const AudioPcm& pcm);

struct BankWem {
    u32 id = 0;
    u32 offset = 0, size = 0;   // into the bank bytes
};
struct BankInfo {
    u32 bankId = 0, version = 0;
    std::vector<BankWem> wems;  // DIDX entries (data inside DATA)
    u32 hircObjects = 0;
};
// Finds a bank (BKHD...) in `data` (a .bnk or a WwiseBank package) and lists its embedded media.
bool ParseBank(const u8* data, size_t size, BankInfo& out, size_t* bankStart = nullptr, std::string* error = nullptr);
