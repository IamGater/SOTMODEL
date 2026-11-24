// Loose (non-package) game files: text, images of the HTML UI shown like textures, the web UI opened in a browser, font previews.
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "app/WebUi.h"
#include "core/ImageDecode.h"
#include <windows.h>
#include <shellapi.h>

namespace {

float Dip(float v) { return v * ImGui::GetStyle().FontSizeBase / 16.f; }
float FontDip(float v) { return ImGui::GetStyle().FontSizeBase * v / 12.f; }
const ImU32 kText = IM_COL32(240, 240, 240, 255), kSeparator = IM_COL32(74, 74, 94, 255);

bool IsFontFileName(std::string_view n) { return IEndsWith(n, ".ttf") || IEndsWith(n, ".otf"); }

u16 Be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
u32 Be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

// family / style / version from the 'name' table and the glyph count from 'maxp'; empty when not a TrueType / OpenType font
std::string FontInfo(const Bytes& d) {
    if (d.size() < 12) return "";
    u32 ver = Be32(d.data());
    if (ver != 0x00010000 && ver != 0x4F54544F /*OTTO*/ && ver != 0x74727565 /*true*/) return "";
    u16 tables = Be16(d.data() + 4);
    std::string names[7];
    int glyphs = 0;
    for (u16 t = 0; t < tables && 12 + (size_t)t * 16 + 16 <= d.size(); t++) {
        const u8* r = d.data() + 12 + (size_t)t * 16;
        u32 off = Be32(r + 8), len = Be32(r + 12);
        if ((u64)off + len > d.size()) continue;
        const u8* tb = d.data() + off;
        if (!memcmp(r, "maxp", 4) && len >= 6) glyphs = Be16(tb + 4);
        if (memcmp(r, "name", 4) || len < 6) continue;
        u16 count = Be16(tb + 2), strOff = Be16(tb + 4);
        for (u16 i = 0; i < count && 6 + (size_t)i * 12 + 12 <= len; i++) {
            const u8* nr = tb + 6 + (size_t)i * 12;
            u16 plat = Be16(nr), lang = Be16(nr + 4), id = Be16(nr + 6), sl = Be16(nr + 8), so = Be16(nr + 10);
            if (id >= 7 || (u64)strOff + so + sl > len) continue;
            const u8* s = tb + strOff + so;
            std::string v;
            if (plat == 3 || plat == 0) {   // UTF-16BE; English first
                if (plat == 3 && lang != 0x409 && !names[id].empty()) continue;
                std::wstring w;
                for (u16 k = 0; k + 1 < sl; k += 2) w += (wchar_t)Be16(s + k);
                v = Narrow(w);
            } else if (plat == 1 && names[id].empty()) {
                v.assign((const char*)s, sl);
            } else {
                continue;
            }
            if (!v.empty()) names[id] = v;
        }
    }
    std::string out = names[1].empty() ? names[4] : names[1];
    if (out.empty()) out = "(unnamed font)";
    if (!names[2].empty()) out += "  " + names[2];
    if (!names[5].empty()) out += "  /  " + names[5];
    if (glyphs) out += Fmt("  /  %d glyphs", glyphs);
    out += ver == 0x4F54544F ? "  /  OpenType (CFF)" : "  /  TrueType";
    return out;
}

void SetLines(Document& d) {
    d.lines.clear();
    d.lines.push_back(0);
    for (u32 i = 0; i < (u32)d.json.size(); i++) if (d.json[i] == '\n') d.lines.push_back(i + 1);
}

std::string Upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

}   // namespace

void App::LoadLooseFile(std::shared_ptr<Document> d) {
    d->loading = true;
    d->webRoot = WebRootOf(d->gamePath);
    d->isFont = IsFontFileName(d->gamePath);
    if (d->isFont) {   // the atlas font is made once per file (fonts can't be dropped while a frame may still draw them)
        auto it = m_previewFonts.find(d->gamePath);
        if (it != m_previewFonts.end()) {
            d->previewFont = it->second.first;
            d->fontInfo = it->second.second;
        }
    }
    m_jobs->Push([this, d] {
        auto data = std::make_shared<Bytes>();
        std::string err;
        if (!m_api->vfs.Read(d->entry, *data, &err)) {
            d->json = "// " + err;
        } else {
            bool text = true;
            for (size_t i = 0; i < std::min<size_t>(data->size(), 4096); i++)
                if ((*data)[i] == 0) { text = false; break; }
            d->json = text ? std::string((const char*)data->data(), data->size()) : Fmt("// binary file, %s", HumanSize(data->size()).c_str());
            if (IsImageFileName(d->gamePath)) {
                Image img;
                if (DecodeImage(data->data(), data->size(), img.rgba, img.width, img.height, &err)) {
                    d->tex.pixelFormat = Upper(d->gamePath.substr(d->gamePath.rfind('.') + 1));
                    d->tex.sizeX = img.width;
                    d->tex.sizeY = img.height;
                    d->tex.mips.resize(1);
                    d->texture = m_renderer->CreateTexture(img, true, true);
                    d->looseImage = d->texture != nullptr;
                } else {
                    d->json += "\n// image: " + err;
                }
            }
            if (d->isFont && !d->previewFont) {
                std::string info = FontInfo(*data);
                if (info.empty()) {
                    d->json += "\n// not a TrueType / OpenType font";
                } else {
                    std::string path = d->gamePath;
                    Post([this, d, data, info, path] {
                        auto it = m_previewFonts.find(path);
                        if (it == m_previewFonts.end()) {
                            void* mem = IM_ALLOC(data->size());
                            memcpy(mem, data->data(), data->size());
                            ImFontConfig cfg;   // the atlas owns (and frees) the copy
                            snprintf(cfg.Name, sizeof(cfg.Name), "%s", path.substr(path.rfind('/') + 1).c_str());
                            ImFont* f = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(mem, (int)data->size(), 32.f, &cfg);
                            it = m_previewFonts.emplace(path, std::make_pair(f, f ? info : info + "  (the font could not be loaded)")).first;
                        }
                        d->previewFont = it->second.first;
                        d->fontInfo = it->second.second;
                    });
                }
            }
            if (!d->webRoot.empty()) {
                for (u32 i = 0; i < m_api->vfs.FileCount(); i++) {
                    std::string_view p = m_api->vfs.Path(i);
                    if (IStartsWith(p, d->webRoot) && IsWebPageFile(p)) d->webPages.emplace_back(p.substr(d->webRoot.size()));
                }
            }
        }
        SetLines(*d);
        d->loading = false;
        Post([] {});
    });
}

// ------------------------------------------------------------------ web UI

void App::DrawWebPanel(Document& d) {
    auto open = [&](const std::string& gamePath, bool folder) {
        if (!folder) {   // served from the archives with the game stood in for (a file:// page stays white)
            std::string err, url = WebUiUrl(*m_api, gamePath, &err);
            if (url.empty()) {
                d.webStatus = "failed: " + err;
            } else {
                ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                d.webStatus = "opened " + url;
            }
            return;
        }
        if (d.webBusy) return;
        d.webBusy = true;
        d.webStatus = "writing the web UI files...";
        std::string root = d.webRoot;
        std::wstring outDir = m_settings.outputDir;
        Document* dp = &d;
        std::shared_ptr<Document> keep;
        for (auto& sp : m_docs) if (sp.get() == dp) keep = sp;
        m_jobs->Push([this, keep, root, outDir, gamePath, folder] {
            std::wstring local;
            int written = 0, total = 0;
            std::string err;
            bool ok = ExportWebRoot(*m_api, root, outDir, local, &written, &total, &err);
            std::string status;
            if (ok) {
                std::wstring target = local + L"\\" + Widen(gamePath.substr(root.size()));
                for (auto& c : target) if (c == L'/') c = L'\\';
                if (folder) {
                    std::wstring args = L"/select,\"" + target + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                } else {
                    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                status = Fmt("%d files in %s (%d written)%s%s", total, Narrow(local).c_str(), written, err.empty() ? "" : "\n", err.c_str());
            } else {
                status = "failed: " + err;
            }
            Post([keep, status] {
                keep->webStatus = status;
                keep->webBusy = false;
            });
        });
    };
    ImGui::TextDisabled("Game HTML interface (Coherent UI)");
    ImGui::TextUnformatted(d.webRoot.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("The pages run in the game's embedded browser with data bound by the game. \"Open in browser\" serves them from "
                       "the archives on a local web server that stands in for the game (empty data, the game's fonts, textures and "
                       "text), so the layout shows without live values; add ?state=Lobby / PirateSelection / ... to the front end URL "
                       "for another screen than the title. \"Show in folder\" writes the whole folder to the output directory.");
    ImGui::Spacing();
    ImGui::BeginDisabled(d.webBusy);
    if (ImGui::Button(IsWebPageFile(d.gamePath) ? "Open in browser" : "Open file")) open(d.gamePath, false);
    ImGui::SameLine();
    if (ImGui::Button("Show in folder")) open(d.gamePath, true);
    ImGui::EndDisabled();
    if (!d.webStatus.empty()) ImGui::TextWrapped("%s", d.webStatus.c_str());
    ImGui::Separator();
    ImGui::TextDisabled("Pages (%zu)", d.webPages.size());
    ImGui::BeginChild("##webpages");
    for (auto& p : d.webPages) {
        bool cur = IEndsWith(d.gamePath, p) && d.gamePath.size() == d.webRoot.size() + p.size();
        if (ImGui::Selectable(p.c_str(), cur)) {
            i64 e = m_api->ResolveFile(d.webRoot + p);
            if (e >= 0) OpenFile((u32)e, true);
        }
        if (ImGui::IsItemHovered()) Tip("click to open the source, double click to open in the browser");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) open(d.webRoot + p, false);
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ fonts

void App::DrawFontPanel(Document& d) {
    ImGui::PushFont(boldFont, FontDip(13));
    ImGui::TextUnformatted(d.name.c_str());
    ImGui::PopFont();
    ImGui::TextWrapped("%s", d.fontInfo.empty() ? "loading..." : d.fontInfo.c_str());
    if (!d.previewFont) return;
    if (!d.fontSample[0]) snprintf(d.fontSample, sizeof(d.fontSample), "%s", "The quick brown fox jumps over the lazy dog 0123456789");
    ImGui::SetNextItemWidth(-Dip(70));
    ImGui::InputText("##sample", d.fontSample, sizeof(d.fontSample));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##size", &d.fontSize, 0.5f, 6, 200, "%.0f px");
    ImGui::Separator();
    ImGui::BeginChild("##fontview", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    auto line = [&](float size, const char* s) {
        ImGui::TextDisabled("%.0f", size);
        ImGui::SameLine(Dip(34));
        ImGui::PushFont(d.previewFont, Dip(size));
        ImGui::TextUnformatted(s);
        ImGui::PopFont();
    };
    line(d.fontSize, d.fontSample);
    ImGui::Spacing();
    for (float s : {12.f, 16.f, 24.f, 36.f, 48.f}) line(s, d.fontSample);
    line(24, "\xD0\xA1\xD1\x8A\xD0\xB5\xD1\x88\xD1\x8C \xD0\xB5\xD1\x89\xD1\x91 \xD1\x8D\xD1\x82\xD0\xB8\xD1\x85 \xD0\xBC\xD1\x8F\xD0\xB3\xD0\xBA\xD0\xB8\xD1\x85 "
             "\xD1\x84\xD1\x80\xD0\xB0\xD0\xBD\xD1\x86\xD1\x83\xD0\xB7\xD1\x81\xD0\xBA\xD0\xB8\xD1\x85 \xD0\xB1\xD1\x83\xD0\xBB\xD0\xBE\xD0\xBA");   // Russian pangram
    ImGui::Spacing();
    ImGui::Separator();
    // glyph grid: Latin, Latin-1, Cyrillic, Greek, punctuation, symbols the font actually has
    ImGui::TextDisabled("Glyphs (hover for the code point)");
    float cell = Dip(40), avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)(avail / cell));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int n = 0;
    std::pair<unsigned, unsigned> ranges[] = {{0x21, 0x7E}, {0xA1, 0x17F}, {0x370, 0x3FF}, {0x400, 0x4FF}, {0x2010, 0x206F}, {0x20A0, 0x20BF}, {0x2190, 0x21FF}, {0x2200, 0x22FF}, {0x25A0, 0x25FF}, {0x2600, 0x26FF}, {0xE000, 0xE0FF}};
    ImGui::PushFont(d.previewFont, Dip(24));
    for (auto [lo, hi] : ranges)
        for (unsigned c = lo; c <= hi; c++) {
            if (!d.previewFont->IsGlyphInFont((ImWchar)c)) continue;
            if (n % cols) ImGui::SameLine(0, 0);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(cell, cell));
            bool hov = ImGui::IsItemHovered();
            dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), hov ? kText : kSeparator);
            char u8s[5] = {};
            ImTextCharToUtf8(u8s, c);
            ImVec2 ts = ImGui::CalcTextSize(u8s);
            dl->AddText(ImVec2(p.x + (cell - ts.x) * 0.5f, p.y + (cell - ts.y) * 0.5f), kText, u8s);
            if (hov) {
                ImGui::PushFont(nullptr, FontDip(12));
                Tip("U+%04X", c);
                ImGui::PopFont();
            }
            n++;
        }
    ImGui::PopFont();
    if (!n) ImGui::TextDisabled("(no glyphs in these ranges)");
    ImGui::EndChild();
}
