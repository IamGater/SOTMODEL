#pragma once
#include "app/Api.h"
#include "audio/Wwise.h"

// Wwise events: WwiseEvent assets (WwiseId + RequiredBank) resolved through the bank's HIRC to the media they play.
struct WwiseEventMedia {
    u32 id = 0;
    enum Where { Missing, Bank, Stream } where = Missing;
    u32 offset = 0, size = 0;   // Bank: into the bank package bytes
    u32 entry = 0;              // Stream: vfs entry of WwiseAudio/Streams/<id>.wem
};
struct WwiseEventInfo {
    std::string name;       // event name ("Play_sfx_...") when known
    u32 id = 0;
    std::string bankPath;   // game path of the bank package
    std::shared_ptr<Package> bank;
    BankInfo bankInfo;
    std::vector<WwiseEventMedia> media;
};

// Resolves an event of a loaded bank.
bool ResolveEventInBank(Api& api, std::shared_ptr<Package> bank, const BankInfo& info, u32 eventId, WwiseEventInfo& out, std::string* error = nullptr);
// A WwiseEvent asset (package path), or an event name ("Play_sfx_x", its asset is looked up under WwiseAudio).
bool LoadWwiseEvent(Api& api, const std::string& pathOrName, WwiseEventInfo& out, std::string* error = nullptr);
// The event's name from the WwiseEvent asset file names (lower case; "" if unknown). Thread safe, built once.
std::string WwiseEventName(Api& api, u32 id);
// Reads the bytes of a resolved media (a view into the bank, or the streamed file).
bool ReadEventMedia(Api& api, const WwiseEventInfo& ev, const WwiseEventMedia& m, Bytes& out, std::string* error = nullptr);
