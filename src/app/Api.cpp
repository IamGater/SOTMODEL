#include "app/Api.h"
#include "core/Json.h"
#include "ue/AssetJson.h"
#include "ue/NativeInfo.h"
#include "ue/Texture.h"
#include "export/Exporter.h"
#include "export/Png.h"
#include <windows.h>

ApiResult ApiResult::Error(const std::string& msg) {
    ApiResult r;
    r.ok = false;
    JsonWriter w;
    w.BeginObject();
    w.KV("error", msg);
    w.EndObject();
    r.json = w.Take();
    return r;
}

std::string Base64(const u8* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        u32 v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += t[(v >> 18) & 63];
        o += t[(v >> 12) & 63];
        o += i + 1 < n ? t[(v >> 6) & 63] : '=';
        o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

bool WildcardMatch(std::string_view pat, std::string_view s) {
    size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; };
    while (t < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || low(pat[p]) == low(s[t]))) { p++; t++; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
        else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

Api::Api(Settings& s) : settings(s) {
    RegisterCoreTools();
    RegisterAssetTools();
    RegisterMeshTools(*this);
}

bool Api::EnsureMounted(std::string* error) {
    std::lock_guard lk(m_mountMutex);
    if (m_mounted) return true;
    if (settings.gameDir.empty() || settings.aesKeys.empty() || settings.oodlePath.empty()) settings.ImportFromFModel();
    if (!settings.oodlePath.empty()) LoadOodle(settings.oodlePath);
    vfs.SetKeys(settings.ParsedKeys());
    if (settings.gameDir.empty()) { if (error) *error = "game directory is not configured (Settings.ini GameDir=...)"; return false; }
    if (!vfs.Mount(settings.PaksDir())) { if (error) *error = "no pak files found in " + Narrow(settings.PaksDir()); return false; }
    cache = std::make_unique<PackageCache>(vfs, 256ull << 20);
    m_mounted = true;
    return true;
}

const ApiTool* Api::FindTool(std::string_view name) const {
    for (auto& t : m_tools) if (t.name == name) return &t;
    return nullptr;
}

ApiResult Api::Call(std::string_view name, const JsonValue& args) {
    const ApiTool* t = FindTool(name);
    if (!t) return ApiResult::Error("unknown tool: " + std::string(name));
    std::string err;
    if (!EnsureMounted(&err)) return ApiResult::Error(err);
    try {
        return t->run(*this, args);
    } catch (const std::exception& ex) {
        return ApiResult::Error(std::string("exception: ") + ex.what());
    }
}

i64 Api::ResolveFile(const std::string& path) const {
    i64 i = vfs.Find(path);
    if (i < 0) i = vfs.FindPackage(path);
    return i;
}

std::shared_ptr<Package> Api::LoadPackage(const std::string& path, std::string* error) {
    i64 i = ResolveFile(path);
    if (i < 0) { if (error) *error = "file not found: " + path + " (use search to find paths)"; return nullptr; }
    return cache->Load((u32)i, error);
}

// ---------------------------------------------------------------- browsing
void Api::RegisterCoreTools() {
    Register({"info", "Summary of the mounted game: archive and file counts, top-level folders, configuration.",
              R"({"type":"object","properties":{}})", "",
              [](Api& a, const JsonValue&) {
                  JsonWriter w;
                  w.BeginObject();
                  w.KV("game", "Sea of Thieves (UE4.10 Athena, legacy packages v483)");
                  w.KV("gameDirectory", Narrow(a.settings.gameDir));
                  w.KV("archives", (i64)a.vfs.Paks().size());
                  w.KV("files", (i64)a.vfs.FileCount());
                  w.KV("aesKeys", (i64)a.settings.ParsedKeys().size());
                  w.KV("oodleLoaded", OodleLoaded());
                  w.KV("outputDirectory", Narrow(a.settings.outputDir));
                  w.Key("pathFormat");
                  w.Str("Athena/Content/<folders>/<name>.uasset ; /Game/<folders>/<name> is accepted too");
                  w.Key("rootFolders");
                  w.BeginArray();
                  const auto& f = a.vfs.Folders();
                  for (u32 c : f[0].folders) for (u32 cc : f[c].folders) w.Str(f[c].name + "/" + f[cc].name);
                  w.EndArray();
                  w.EndObject();
                  ApiResult r;
                  r.json = w.Take();
                  return r;
              }});

    Register({"search",
              "Find files by path. 'query' is a case-insensitive substring, or a wildcard pattern when it contains * or ? "
              "(matched against the full path). Several space-separated terms must all match. Optional 'folder' restricts the search, "
              "'extension' filters (uasset, umap, wem, ...).",
              R"({"type":"object","properties":{"query":{"type":"string"},"folder":{"type":"string"},"extension":{"type":"string"},"limit":{"type":"integer","default":100},"offset":{"type":"integer","default":0}},"required":["query"]})",
              "query",
              [](Api& a, const JsonValue& args) {
                  std::string q = args.GetStr("query"), folder = Vfs::NormalizePath(args.GetStr("folder")), ext = ToLower(args.GetStr("extension"));
                  if (!ext.empty() && ext[0] != '.') ext = "." + ext;
                  size_t limit = (size_t)std::clamp(args.GetNum("limit", 100), 1.0, 5000.0), offset = (size_t)std::max(0.0, args.GetNum("offset", 0));
                  std::vector<std::string> terms;
                  bool wildcard = q.find_first_of("*?") != std::string::npos;
                  if (!wildcard) {
                      size_t s = 0;
                      std::string lq = ToLower(q);
                      while (s < lq.size()) {
                          size_t e = lq.find(' ', s);
                          if (e == std::string::npos) e = lq.size();
                          if (e > s) terms.push_back(lq.substr(s, e - s));
                          s = e + 1;
                      }
                  }
                  JsonWriter w;
                  w.BeginObject();
                  w.Key("results");
                  w.BeginArray();
                  size_t matched = 0, shown = 0;
                  std::string low;
                  for (u32 i = 0; i < a.vfs.FileCount(); i++) {
                      std::string_view p = a.vfs.Path(i);
                      if (!folder.empty() && !IStartsWith(p, folder)) continue;
                      if (!ext.empty() && !IEndsWith(p, ext)) continue;
                      bool ok;
                      if (wildcard) ok = WildcardMatch(q, p);
                      else {
                          low = ToLower(p);
                          ok = true;
                          for (auto& t : terms) if (low.find(t) == std::string::npos) { ok = false; break; }
                      }
                      if (!ok) continue;
                      if (matched++ < offset || shown >= limit) continue;
                      shown++;
                      w.Str(p);
                  }
                  w.EndArray();
                  w.KV("total", (i64)matched);
                  w.KV("offset", (i64)offset);
                  if (matched > offset + shown) w.KV("more", "increase offset to page");
                  w.EndObject();
                  ApiResult r;
                  r.json = w.Take();
                  return r;
              }});

    Register({"list_folder", "List subfolders (with recursive file counts) and files of a folder, like FModel's archive tree. Empty path = root.",
              R"({"type":"object","properties":{"path":{"type":"string","default":""},"limit":{"type":"integer","default":500}}})", "path",
              [](Api& a, const JsonValue& args) {
                  std::string path = Vfs::NormalizePath(args.GetStr("path"));
                  while (!path.empty() && path.back() == '/') path.pop_back();
                  size_t limit = (size_t)std::clamp(args.GetNum("limit", 500), 1.0, 20000.0);
                  const auto& folders = a.vfs.Folders();
                  u32 cur = 0;
                  size_t s = 0;
                  while (s < path.size()) {
                      size_t e = path.find('/', s);
                      if (e == std::string::npos) e = path.size();
                      std::string part = path.substr(s, e - s);
                      u32 next = UINT32_MAX;
                      for (u32 c : folders[cur].folders) if (IEquals(folders[c].name, part)) { next = c; break; }
                      if (next == UINT32_MAX) return ApiResult::Error("folder not found: " + path);
                      cur = next;
                      s = e + 1;
                  }
                  JsonWriter w;
                  w.BeginObject();
                  w.KV("path", path);
                  w.Key("folders");
                  w.BeginArray();
                  for (u32 c : folders[cur].folders) {
                      w.BeginObject();
                      w.KV("name", folders[c].name);
                      w.KV("files", (i64)folders[c].totalFiles);
                      w.EndObject();
                  }
                  w.EndArray();
                  w.Key("files");
                  w.BeginArray();
                  size_t n = 0;
                  for (u32 f : folders[cur].files) {
                      if (n++ >= limit) break;
                      w.Str(a.vfs.FileName(f));
                  }
                  w.EndArray();
                  if (folders[cur].files.size() > limit) w.KV("filesTruncated", (i64)folders[cur].files.size());
                  w.EndObject();
                  ApiResult r;
                  r.json = w.Take();
                  return r;
              }});
}

// ---------------------------------------------------------------- packages
void Api::RegisterAssetTools() {
    Register({"get_package",
              "Overview of a package (.uasset/.umap): main asset class, every export (name, class, outer), imported packages "
              "(dependencies) and decoded native summaries (texture formats, mesh/anim stats). Cheap - use before get_properties.",
              R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})", "path",
              [](Api& a, const JsonValue& args) {
                  std::string err;
                  auto pkg = a.LoadPackage(args.GetStr("path"), &err);
                  if (!pkg) return ApiResult::Error(err);
                  JsonWriter w;
                  SetJsonPackage(pkg.get());
                  w.BeginObject();
                  w.KV("path", pkg->GamePath());
                  w.KV("packageName", pkg->PackageName());
                  i32 main = pkg->MainExport();
                  if (main) { w.KV("mainExport", pkg->Exports()[main - 1].objectName); w.KV("mainClass", pkg->ExportClass(main - 1)); }
                  w.KV("sizeBytes", (i64)pkg->Data().size());
                  w.Key("exports");
                  w.BeginArray();
                  for (size_t i = 0; i < pkg->Exports().size(); i++) {
                      const auto& ex = pkg->Exports()[i];
                      w.BeginObject();
                      w.KV("index", (i64)i);
                      w.KV("name", ex.objectName);
                      w.KV("class", pkg->ExportClass(i));
                      if (ex.outerIndex) w.KV("outer", pkg->ObjectName(ex.outerIndex));
                      if (ex.superIndex) w.KV("super", pkg->FullPath(ex.superIndex));
                      WriteNativeInfo(w, *pkg, i, 0);
                      w.EndObject();
                  }
                  w.EndArray();
                  w.Key("dependencies");
                  w.BeginArray();
                  std::vector<std::string> deps;
                  for (auto& im : pkg->Imports())
                      if (im.outerIndex == 0 && im.className == "Package" && std::find(deps.begin(), deps.end(), im.objectName) == deps.end()) deps.push_back(im.objectName);
                  std::sort(deps.begin(), deps.end());
                  for (auto& d : deps) w.Str(d);
                  w.EndArray();
                  w.EndObject();
                  SetJsonPackage(nullptr);
                  ApiResult r;
                  r.json = w.Take();
                  return r;
              }});

    Register({"get_properties",
              "FModel-style JSON of a package: every export with all decoded properties. Optional 'export' (name or index) limits the output to one export. "
              "Output longer than 'max_chars' (default 200000) is truncated.",
              R"({"type":"object","properties":{"path":{"type":"string"},"export":{"type":"string"},"max_chars":{"type":"integer","default":200000}},"required":["path"]})",
              "path",
              [](Api& a, const JsonValue& args) {
                  std::string err;
                  auto pkg = a.LoadPackage(args.GetStr("path"), &err);
                  if (!pkg) return ApiResult::Error(err);
                  std::string which = args.GetStr("export");
                  std::string json;
                  if (which.empty()) json = PackageToJson(*pkg);
                  else {
                      i64 idx = -1;
                      if (!which.empty() && std::all_of(which.begin(), which.end(), ::isdigit)) idx = atoll(which.c_str());
                      else idx = (i64)pkg->FindExport(which) - 1;
                      if (idx < 0 || idx >= (i64)pkg->Exports().size()) return ApiResult::Error("export not found: " + which);
                      JsonWriter w;
                      SetJsonPackage(pkg.get());
                      w.BeginObject();
                      w.KV("Type", pkg->ExportClass((size_t)idx));
                      w.KV("Name", pkg->Exports()[(size_t)idx].objectName);
                      size_t end = 0;
                      std::string perr;
                      PropertyList props = pkg->ReadProperties((size_t)idx, &end, &perr);
                      w.Key("Properties");
                      WriteProperties(w, props);
                      if (!perr.empty()) w.KV("PropertyError", perr);
                      WriteNativeInfo(w, *pkg, (size_t)idx, end);
                      w.EndObject();
                      SetJsonPackage(nullptr);
                      json = w.Take();
                  }
                  size_t maxChars = (size_t)std::max(1000.0, args.GetNum("max_chars", 200000));
                  if (json.size() > maxChars) json = json.substr(0, maxChars) + "\n... [truncated, " + std::to_string(json.size()) + " chars total; request a single export]";
                  ApiResult r;
                  r.json = std::move(json);
                  return r;
              }});

    Register({"export",
              "Export a package to disk: textures -> PNG (or DDS), meshes -> glTF .glb (or OBJ), animations -> .glb, plus the JSON. "
              "Returns the written file paths. 'output_dir' defaults to the configured output directory.",
              R"({"type":"object","properties":{"path":{"type":"string"},"output_dir":{"type":"string"},"texture_format":{"type":"string","enum":["png","dds"]},"mesh_format":{"type":"string","enum":["glb","obj"]},"json":{"type":"boolean","default":true},"raw":{"type":"boolean","default":false}},"required":["path"]})",
              "path",
              [](Api& a, const JsonValue& args) {
                  std::string err;
                  auto pkg = a.LoadPackage(args.GetStr("path"), &err);
                  if (!pkg) return ApiResult::Error(err);
                  ExportOptions opt;
                  std::string od = args.GetStr("output_dir");
                  opt.outputDir = od.empty() ? a.settings.outputDir : Widen(od);
                  opt.textureFormat = args.GetStr("texture_format", a.settings.textureFormat ? "dds" : "png") == "dds" ? 1 : 0;
                  opt.meshFormat = args.GetStr("mesh_format", a.settings.meshFormat ? "obj" : "glb") == "obj" ? 1 : 0;
                  opt.json = args.GetBool("json", true);
                  opt.raw = args.GetBool("raw", false);
                  opt.keepDirectories = a.settings.exportKeepDirectories;
                  std::vector<std::string> written;
                  bool ok = ExportPackage(*a.cache, *pkg, opt, written, &err);
                  JsonWriter w;
                  w.BeginObject();
                  w.KV("ok", ok);
                  if (!ok) w.KV("error", err);
                  w.Key("files");
                  w.BeginArray();
                  for (auto& f : written) w.Str(f);
                  w.EndArray();
                  w.EndObject();
                  ApiResult r;
                  r.ok = ok;
                  r.json = w.Take();
                  return r;
              }});

    Register({"preview_texture",
              "Decode a texture and return it as a PNG image (downscaled to max_size) so it can be looked at, plus its format info. "
              "Use 'save_to' to also write the PNG to a file.",
              R"({"type":"object","properties":{"path":{"type":"string"},"max_size":{"type":"integer","default":512},"save_to":{"type":"string"}},"required":["path"]})",
              "path",
              [](Api& a, const JsonValue& args) {
                  std::string err;
                  auto pkg = a.LoadPackage(args.GetStr("path"), &err);
                  if (!pkg) return ApiResult::Error(err);
                  i32 main = pkg->MainExport();
                  if (!main || !IsTextureClass(pkg->ExportClass(main - 1))) return ApiResult::Error("not a texture package (main class " + (main ? pkg->ExportClass(main - 1) : std::string("none")) + ")");
                  TextureInfo info;
                  if (!ReadTextureInfo(*pkg, main - 1, info, &err)) return ApiResult::Error(err);
                  int maxSize = (int)std::clamp(args.GetNum("max_size", 512), 16.0, 8192.0);
                  Image img;
                  int mip = 0;
                  if (!DecodeTexture(*pkg, info, -1, maxSize, img, &err, &mip)) return ApiResult::Error(err);
                  ApiResult r;
                  ApiImage im;
                  im.mime = "image/png";
                  im.data = EncodePng(img.rgba.data(), img.width, img.height);
                  std::string save = args.GetStr("save_to");
                  if (!save.empty()) WriteWholeFile(Widen(save), im.data.data(), im.data.size());
                  JsonWriter w;
                  w.BeginObject();
                  w.KV("path", pkg->GamePath());
                  w.KV("pixelFormat", info.pixelFormat);
                  w.KV("sizeX", info.sizeX);
                  w.KV("sizeY", info.sizeY);
                  w.KV("previewMip", mip);
                  w.KV("previewSize", Fmt("%dx%d", img.width, img.height));
                  w.KV("srgb", info.srgb);
                  w.KV("compressionSettings", info.compressionSettings);
                  if (!save.empty()) w.KV("savedTo", save);
                  w.EndObject();
                  r.json = w.Take();
                  r.images.push_back(std::move(im));
                  return r;
              }});

    Register({"find_assets_by_class",
              "Find packages whose main asset is of a given class (e.g. StaticMesh, SkeletalMesh, AnimSequence, Texture2D, "
              "MaterialInstanceConstant, BlueprintGeneratedClass, World). Reads package headers, so restrict 'folder' for speed.",
              R"({"type":"object","properties":{"class":{"type":"string"},"folder":{"type":"string"},"name":{"type":"string","description":"optional substring of the file name"},"limit":{"type":"integer","default":100}},"required":["class"]})",
              "class",
              [](Api& a, const JsonValue& args) {
                  std::string cls = args.GetStr("class"), folder = Vfs::NormalizePath(args.GetStr("folder")), name = ToLower(args.GetStr("name"));
                  size_t limit = (size_t)std::clamp(args.GetNum("limit", 100), 1.0, 5000.0);
                  JsonWriter w;
                  w.BeginObject();
                  w.Key("results");
                  w.BeginArray();
                  size_t found = 0, scanned = 0;
                  for (u32 i = 0; i < a.vfs.FileCount() && found < limit; i++) {
                      std::string_view p = a.vfs.Path(i);
                      if (!(IEndsWith(p, ".uasset") || IEndsWith(p, ".umap"))) continue;
                      if (!folder.empty() && !IStartsWith(p, folder)) continue;
                      if (!name.empty() && ToLower(a.vfs.FileName(i)).find(name) == std::string::npos) continue;
                      Bytes data;
                      if (!a.vfs.Read(i, data)) continue;
                      scanned++;
                      Package pkg;
                      if (!pkg.Load(std::move(data), std::string(p))) continue;
                      i32 m = pkg.MainExport();
                      if (!m) continue;
                      std::string c = pkg.ExportClass(m - 1);
                      bool match = IEquals(c, cls) || (IEquals(cls, "World") && IEndsWith(p, ".umap")) ||
                                   (IEquals(cls, "Blueprint") && IEndsWith(c, "BlueprintGeneratedClass"));
                      if (!match) {
                          for (auto& ex : pkg.Exports()) if (ex.outerIndex == 0 && IEquals(pkg.ClassName((i32)(&ex - pkg.Exports().data()) + 1), cls)) { match = true; break; }
                      }
                      if (!match) continue;
                      found++;
                      w.Str(p);
                  }
                  w.EndArray();
                  w.KV("scannedPackages", (i64)scanned);
                  w.EndObject();
                  ApiResult r;
                  r.json = w.Take();
                  return r;
              }});
}
