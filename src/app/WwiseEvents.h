#pragma once
#include "app/Api.h"
#include "audio/Wwise.h"

// Wwise events: WwiseEvent assets (WwiseId + RequiredBank) resolved through the bank's HIRC to the media they play.
struct WwiseEventMedia {
    u32 id = 0;
    enum Where { Missing, Bank, Stream } where = Missing;
    u32 offset = 0, size = 0;   // Bank: into the bank package bytes
    u32 entry = 0;              // Stream: vfs entry of WwiseAudio/Streams/<id>.wem
    std::vector<WwiseMediaCondition> conditions;   // switch / state values it plays under
};
struct WwiseEventInfo {
    std::string name;       // event name ("Play_sfx_...") when known
    u32 id = 0;
    std::string bankPath;   // game path of the bank package
    std::shared_ptr<Package> bank;
    BankInfo bankInfo;
    std::vector<WwiseEventMedia> media;
    std::vector<WwiseSwitchGroup> switches;   // switch / state groups that pick what plays
    // plays media i with the choice (group -> value; groups not in it: any value)
    bool Plays(size_t i, const std::unordered_map<u32, u32>& choice) const;
};

// Resolves an event of a loaded bank.
bool ResolveEventInBank(Api& api, std::shared_ptr<Package> bank, const BankInfo& info, u32 eventId, WwiseEventInfo& out, std::string* error = nullptr);
// A WwiseEvent asset (package path), or an event name ("Play_sfx_x", its asset is looked up under WwiseAudio).
bool LoadWwiseEvent(Api& api, const std::string& pathOrName, WwiseEventInfo& out, std::string* error = nullptr);
// The event's name from the WwiseEvent asset file names (lower case; "" if unknown). Thread safe, built once.
std::string WwiseEventName(Api& api, u32 id);
// A switch / state group or value name: the banks only keep hashes, so names are guessed from the words of the event names
// (and runs of them: "footstep_surface"). "" if none hashes to id.
std::string WwiseGuessName(Api& api, u32 id);
// The guessed name, or the id in hex.
std::string WwiseNameOrId(Api& api, u32 id);
// Reads the bytes of a resolved media (a view into the bank, or the streamed file).
bool ReadEventMedia(Api& api, const WwiseEventInfo& ev, const WwiseEventMedia& m, Bytes& out, std::string* error = nullptr);
