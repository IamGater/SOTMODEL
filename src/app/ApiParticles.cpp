// Cascade particle system tools: emitters / modules / curves, and a simulated sprite preview.
#include "app/Api.h"
#include "core/Json.h"
#include "ue/Particles.h"
#include "ue/Texture.h"
#include "export/Png.h"

namespace {

bool LoadSystem(Api& a, const JsonValue& args, ParticleSystemInfo& ps, std::string& err) {
    std::string path = args.GetStr("path");
    auto pkg = a.LoadPackage(path, &err);
    if (!pkg) return false;
    i32 main = pkg->MainExport();
    if (!main || pkg->ExportClass(main - 1) != "ParticleSystem") { err = "not a ParticleSystem: " + (main ? pkg->ExportClass(main - 1) : std::string("?")); return false; }
    return ReadParticleSystem(*a.cache, *pkg, main - 1, ps, &err);
}

}   // namespace

void RegisterParticleTools(Api& api) {
    api.Register({"get_particle_system",
                  "Decode a Cascade ParticleSystem: emitters (type sprite / mesh / GPU / beam / ribbon, material, mesh, screen alignment, "
                  "duration / loops, subUV grid, spawn rate and bursts) and every module with its baked distributions (constant, random "
                  "range or curve). 'curves=true' adds the sampled curve values.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"curves":{"type":"boolean","default":false}},"required":["path"]})", "path",
                  [](Api& a, const JsonValue& args) {
                      ParticleSystemInfo ps;
                      std::string err;
                      if (!LoadSystem(a, args, ps, err)) return ApiResult::Error(err);
                      bool curves = args.GetBool("curves", false);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("path", ps.path);
                      if (ps.warmup > 0) w.KV("warmupSeconds", (double)ps.warmup);
                      w.Key("lodDistances");
                      w.BeginArray();
                      for (float d : ps.lodDistances) w.Num(d);
                      w.EndArray();
                      w.Key("emitters");
                      w.BeginArray();
                      for (auto& e : ps.emitters) {
                          w.BeginObject();
                          w.KV("name", e.name);
                          w.KV("type", e.type);
                          if (!e.enabled) w.KV("enabled", false);
                          w.KV("material", e.material);
                          if (!e.blendMode.empty()) w.KV("blendMode", e.blendMode);
                          if (!e.mesh.empty()) w.KV("mesh", e.mesh);
                          w.KV("screenAlignment", e.screenAlignment);
                          w.KV("localSpace", e.localSpace);
                          w.KV("duration", (double)e.duration);
                          if (e.delay > 0) w.KV("delay", (double)e.delay);
                          w.KV("loops", e.loops);
                          if (e.subH * e.subV > 1) w.KV("subUV", Fmt("%dx%d %s", e.subH, e.subV, e.subUVMethod.c_str()));
                          if (e.rate.Valid()) w.KV("spawnRate", e.rate.Describe());
                          if (!e.bursts.empty()) {
                              w.Key("bursts");
                              w.BeginArray();
                              for (auto& b : e.bursts) w.Str(b.countLow >= 0 ? Fmt("%d-%d at %g", b.countLow, b.count, b.time) : Fmt("%d at %g", b.count, b.time));
                              w.EndArray();
                          }
                          if (e.peakParticles) w.KV("peakParticles", e.peakParticles);
                          w.Key("modules");
                          w.BeginArray();
                          for (auto& m : e.modules) {
                              w.BeginObject();
                              w.KV("class", m.cls);
                              if (!m.enabled) w.KV("enabled", false);
                              if (!m.simulated) w.KV("previewed", false);
                              for (auto& [n, c] : m.curves) {
                                  if (!curves) { w.KV(n, c.Describe()); continue; }
                                  w.Key(n);
                                  w.BeginObject();
                                  w.KV("summary", c.Describe());
                                  w.KV("source", c.source);
                                  w.Key("samples");   // 9 samples over 0..1
                                  w.BeginArray();
                                  for (int i = 0; i <= 8; i++) {
                                      w.BeginArray();
                                      for (int ch = 0; ch < c.dims; ch++) {
                                          w.Num(c.Eval(i / 8.f, ch, 0));
                                          if (c.op >= 2) w.Num(c.Eval(i / 8.f, ch, 1));
                                      }
                                      w.EndArray();
                                  }
                                  w.EndArray();
                                  w.EndObject();
                              }
                              for (auto& [n, v] : m.values) w.KV(n, v);
                              w.EndObject();
                          }
                          w.EndArray();
                          w.EndObject();
                      }
                      w.EndArray();
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"preview_particles",
                  "Simulate a particle system on the CPU (spawn rate, bursts, lifetime, size / color / alpha over life, velocity, cone, "
                  "acceleration, drag, sphere / cylinder locations, rotation, subUV) and render the sprites with their material's texture "
                  "as a contact sheet of 'frames' times up to 'time' seconds (default: the longest emitter loop). Mesh / beam / ribbon "
                  "emitters are drawn as sprites.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"time":{"type":"number"},"frames":{"type":"integer","default":4},)"
                  R"("size":{"type":"integer","default":320},"yaw":{"type":"number","default":-135},"pitch":{"type":"number","default":-15},)"
                  R"("save_to":{"type":"string"}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      ParticleSystemInfo ps;
                      std::string err;
                      if (!LoadSystem(a, args, ps, err)) return ApiResult::Error(err);
                      float longest = 0.5f;
                      for (auto& e : ps.emitters) longest = std::max(longest, e.delay + e.duration);
                      float upTo = args.Get("time") ? (float)args.GetNum("time", 1) : std::min(longest, 8.f);
                      int frames = std::clamp((int)args.GetNum("frames", 4), 1, 16), size = std::clamp((int)args.GetNum("size", 320), 64, 1024);
                      std::vector<Image> textures(ps.emitters.size());
                      std::vector<ParticleRenderEmitter> re(ps.emitters.size());
                      for (size_t i = 0; i < ps.emitters.size(); i++) {
                          auto& e = ps.emitters[i];
                          if (LoadParticleTexture(*a.cache, e.material, 1024, textures[i])) re[i].texture = &textures[i];
                          re[i].subH = e.subH;
                          re[i].subV = e.subV;
                          re[i].additive = e.blendMode.find("Additive") != std::string::npos;
                          re[i].alignment = e.screenAlignment;
                          re[i].axisLock = e.axisLock;
                          re[i].alignToVelocity = e.alignToVelocity;
                          re[i].pivot = ParticlePivot(e);
                          re[i].frameBlend = e.subUVMethod.find("Blend") != std::string::npos;
                      }
                      Vec3 centre;
                      float dist;
                      ParticleFrame(ps, upTo, centre, dist);
                      float yaw = (float)args.GetNum("yaw", -135), pitch = (float)args.GetNum("pitch", -15);
                      ParticleSim sim;
                      sim.Init(ps);
                      int cols = std::min(frames, 4), rows = (frames + cols - 1) / cols;
                      Image sheet;
                      sheet.width = cols * size;
                      sheet.height = rows * size;
                      sheet.rgba.assign((size_t)sheet.width * sheet.height * 4, 0);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("path", ps.path);
                      w.Key("frames");
                      w.BeginArray();
                      std::vector<ParticleSprite> sp;
                      for (int f = 0; f < frames; f++) {
                          float t = frames > 1 ? upTo * (f + 1) / frames : upTo;
                          sim.AdvanceTo(t);
                          sim.Sprites(sp);
                          Image img;
                          RenderParticleSprites(sp, re, centre, dist, yaw, pitch, size, size, img);
                          int ox = (f % cols) * size, oy = (f / cols) * size;
                          for (int y = 0; y < size; y++) memcpy(&sheet.rgba[((size_t)(oy + y) * sheet.width + ox) * 4], &img.rgba[(size_t)y * size * 4], (size_t)size * 4);
                          w.BeginObject();
                          w.KV("time", (double)t);
                          w.KV("particles", (i64)sp.size());
                          w.EndObject();
                      }
                      w.EndArray();
                      w.KV("layout", Fmt("%d columns x %d rows, left to right", cols, rows));
                      ApiResult r;
                      ApiImage im;
                      im.mime = "image/png";
                      im.data = EncodePng(sheet.rgba.data(), sheet.width, sheet.height);
                      std::string save = args.GetStr("save_to");
                      if (!save.empty()) {
                          WriteWholeFile(Widen(save), im.data.data(), im.data.size());
                          w.KV("savedTo", save);
                      }
                      w.EndObject();
                      r.json = w.Take();
                      r.images.push_back(std::move(im));
                      return r;
                  }});
}
