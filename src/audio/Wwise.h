#pragma once
#include "core/Common.h"
#include <unordered_map>

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

// Switch / state containers on the way from an event to its media: which switch value plays what.
struct WwiseSwitchGroup {
    u32 group = 0;
    bool state = false;          // a state group (else a switch group)
    u32 defaultValue = 0;
    std::vector<u32> values;     // values with something assigned, in bank order
};
struct WwiseMediaCondition {     // the media plays only while `group` is one of `values` (empty: under no value)
    u32 group = 0;
    std::vector<u32> values;
};
struct WwiseEventTree {
    std::vector<u32> nodes;   // every hierarchy node visited from the event's Play actions
    std::vector<u32> media;
    std::vector<std::vector<WwiseMediaCondition>> conditions;   // per media
    std::vector<WwiseSwitchGroup> switches;
    // the media playable with the given choice (group -> value; groups not in the map: any value)
    bool Plays(size_t mediaIndex, const std::unordered_map<u32, u32>& choice) const;
};
bool ResolveWwiseEvent(const u8* data, const BankInfo& bank, u32 eventId, WwiseEventTree& tree, std::string* error = nullptr);

// RTPC curves (game parameter -> property) of hierarchy nodes: CAkParameterNode's InitialRTPC block (bank v134 layout:
// u32 RTPC id, u8 type, u8 accumulation, var-int property, u32 curve id, u8 scaling, u16 point count, points {f32 x, f32 y, u32 shape}).
struct WwiseRtpcPoint {
    float x = 0, y = 0;
    u32 shape = 4;   // AkCurveInterpolation: 0 Log3, 1 Sine, 2 Log1, 3 InvSCurve, 4 Linear, 5 SCurve, 6 Exp1, 7 SineRecip, 8 Exp3, 9 Constant
};
struct WwiseRtpcCurve {
    u32 node = 0;        // the hierarchy object carrying it
    u8 nodeType = 0;
    u32 rtpc = 0;        // game parameter (or modulator) short id
    u8 rtpcType = 0;     // 0 game parameter, 1 MIDI, 2 modulator
    u8 accum = 0;        // 1 exclusive, 2 additive, 3 multiply, 4 boolean, 5 maximum, 6 filter
    u32 property = 0;    // AkRTPC_ParameterID
    u8 scaling = 0;      // 0 none, 2 dB, 3 log, 4 dB to linear
    std::vector<WwiseRtpcPoint> points;
    float Eval(float x) const;   // the property's value at parameter value x (shapes approximated)
};
const char* WwiseRtpcPropertyName(u32 property);   // "Volume", "Pitch", "LPF", ... ("" when unknown)
// RTPC curves of the given nodes and of their ancestors (actor-mixers / containers listing them as children).
std::vector<WwiseRtpcCurve> FindWwiseRtpcs(const u8* data, const BankInfo& bank, const std::vector<u32>& nodes);
// Property totals of the curves at the given parameter values (rtpc id -> value; missing parameters use the curve's first point):
// additive properties summed (Volume dB, Pitch cents, LPF / HPF), multiplied ones multiplied.
std::unordered_map<u32, float> EvalWwiseRtpcs(const std::vector<WwiseRtpcCurve>& curves, const std::unordered_map<u32, float>& values);