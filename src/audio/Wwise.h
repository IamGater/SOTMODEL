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
// One HIRC (hierarchy) object: sound, action, event, container, music node, ...
struct HircObject {
    u8 type = 0;     // 2 sound, 3 action, 4 event, 5 random/sequence, 6 switch, 7 actor-mixer, 9 layer, 10 music segment, 11 music track, ...
    u32 id = 0;
    u32 offset = 0, size = 0;   // body after the id, into the bank bytes
};
struct BankInfo {
    u32 bankId = 0, version = 0;
    std::vector<BankWem> wems;  // DIDX entries (data inside DATA)
    u32 hircObjects = 0;
    std::vector<HircObject> hirc;
    const HircObject* Find(u32 id) const;
    std::vector<u32> Events() const;   // ids of the events defined in the bank
};
// Finds a bank (BKHD...) in `data` (a .bnk or a WwiseBank package) and lists its embedded media and HIRC objects.
bool ParseBank(const u8* data, size_t size, BankInfo& out, size_t* bankStart = nullptr, std::string* error = nullptr);

// Wwise short id of a name (FNV-1 32 of the lower-case name): event "Play_x" -> its id in the banks.
u32 WwiseShortId(std::string_view name);
// What an event can play: its Play actions' targets, followed down through containers (random / sequence / switch / layer,
// music switch / playlist / segment) to sounds and music tracks. Returns their media (.wem) ids, in hierarchy order,
// without duplicates. The media are embedded in some bank (DIDX) or streamed (WwiseAudio/Streams/<id>.wem).
bool ResolveWwiseEvent(const u8* data, const BankInfo& bank, u32 eventId, std::vector<u32>& media, std::string* error = nullptr);
