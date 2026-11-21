// Audio tools: Wwise .wem streams and sound banks (WwiseBank packages / .bnk) -> info and WAV / Ogg files.
#include "app/Api.h"
#include "core/Json.h"
#include "audio/Wwise.h"

namespace {

bool ReadFileBytes(Api& a, const std::string& path, u32& entry, Bytes& data, std::string& err) {
    i64 e = a.ResolveFile(path);
    if (e < 0) { err = "file not found: " + path; return false; }
    entry = (u32)e;
    return a.vfs.Read(entry, data, &err);
}

void WriteWemInfo(JsonWriter& w, const u8* d, size_t n, bool decode) {
    WemInfo info;
    std::string err;
    if (!ReadWemInfo(d, n, info, &err)) { w.KV("error", err); return; }
    w.KV("codec", info.codecName);
    w.KV("channels", info.channels);
    w.KV("sampleRate", info.sampleRate);
    if (info.sampleCount && info.sampleRate) w.KV("seconds", (double)info.sampleCount / info.sampleRate);
    if (decode) {
        AudioPcm pcm;
        if (DecodeWem(d, n, pcm, &err)) w.KV("decodedSeconds", pcm.Seconds());
        else w.KV("decodeError", err);
    }
}

std::wstring OutPath(Api& a, const std::string& od, const std::string& rel) {
    std::wstring out = (od.empty() ? a.settings.outputDir : Widen(od)) + L"\\" + Widen(rel);
    for (auto& c : out) if (c == L'/') c = L'\\';
    CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
    return out;
}

}   // namespace

void RegisterAudioTools(Api& api) {
    api.Register({"get_audio_info",
                  "Inspect Wwise audio: a .wem stream (codec, channels, sample rate, length) or a sound bank (WwiseBank package or .bnk: "
                  "bank id and the embedded .wem media with their formats).",
                  R"({"type":"object","properties":{"path":{"type":"string"},"limit":{"type":"integer","default":200}},"required":["path"]})", "path",
                  [](Api& a, const JsonValue& args) {
                      std::string err;
                      u32 e;
                      Bytes data;
                      if (!ReadFileBytes(a, args.GetStr("path"), e, data, err)) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("path", std::string(a.vfs.Path(e)));
                      if (data.size() >= 4 && !memcmp(data.data(), "RIFF", 4)) {
                          w.KV("type", "wem");
                          WriteWemInfo(w, data.data(), data.size(), true);
                      } else {
                          BankInfo bank;
                          if (!ParseBank(data.data(), data.size(), bank, nullptr, &err)) return ApiResult::Error("not a .wem or a sound bank: " + err);
                          w.KV("type", "bank");
                          w.KV("bankId", (i64)bank.bankId);
                          w.KV("bankVersion", (i64)bank.version);
                          w.KV("hircObjects", (i64)bank.hircObjects);
                          w.KV("embeddedMedia", (i64)bank.wems.size());
                          w.Key("media");
                          w.BeginArray();
                          int limit = (int)args.GetNum("limit", 200);
                          for (size_t i = 0; i < bank.wems.size() && (int)i < limit; i++) {
                              auto& m = bank.wems[i];
                              w.BeginObject();
                              w.KV("id", (i64)m.id);
                              w.KV("bytes", (i64)m.size);
                              WriteWemInfo(w, data.data() + m.offset, m.size, false);
                              w.EndObject();
                          }
                          w.EndArray();
                      }
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"export_audio",
                  "Decode Wwise audio to files: a .wem -> .wav (or .ogg for Wwise Vorbis with format=ogg); a sound bank -> one file per embedded "
                  "media in a folder named after the bank. 'output_dir' defaults to the configured output directory.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"output_dir":{"type":"string"},"format":{"type":"string","enum":["wav","ogg"],"default":"wav"}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::string err;
                      u32 e;
                      Bytes data;
                      if (!ReadFileBytes(a, args.GetStr("path"), e, data, err)) return ApiResult::Error(err);
                      std::string od = args.GetStr("output_dir");
                      bool ogg = args.GetStr("format", "wav") == "ogg";
                      std::string rel(a.vfs.Path(e));
                      rel = rel.substr(0, rel.rfind('.'));
                      if (!a.settings.exportKeepDirectories) rel = rel.substr(rel.rfind('/') + 1);
                      std::vector<std::string> written, failed;
                      auto one = [&](const u8* d, size_t n, const std::string& base) {
                          std::string e2;
                          if (ogg) {
                              Bytes o;
                              if (WemToOgg(d, n, o, &e2)) {
                                  std::wstring p = OutPath(a, od, base + ".ogg");
                                  if (WriteWholeFile(p, o.data(), o.size())) { written.push_back(Narrow(p)); return; }
                                  e2 = "write failed";
                              }
                          } else {
                              AudioPcm pcm;
                              if (DecodeWem(d, n, pcm, &e2)) {
                                  Bytes wav = MakeWav(pcm);
                                  std::wstring p = OutPath(a, od, base + ".wav");
                                  if (WriteWholeFile(p, wav.data(), wav.size())) { written.push_back(Narrow(p)); return; }
                                  e2 = "write failed";
                              }
                          }
                          failed.push_back(base + ": " + e2);
                      };
                      if (data.size() >= 4 && !memcmp(data.data(), "RIFF", 4)) {
                          one(data.data(), data.size(), rel);
                      } else {
                          BankInfo bank;
                          if (!ParseBank(data.data(), data.size(), bank, nullptr, &err)) return ApiResult::Error("not a .wem or a sound bank: " + err);
                          for (auto& m : bank.wems) one(data.data() + m.offset, m.size, rel + "/" + std::to_string(m.id));
                      }
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("ok", failed.empty());
                      w.Key("files");
                      w.BeginArray();
                      for (auto& f : written) w.Str(f);
                      w.EndArray();
                      if (!failed.empty()) {
                          w.Key("failed");
                          w.BeginArray();
                          for (auto& f : failed) w.Str(f);
                          w.EndArray();
                      }
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
