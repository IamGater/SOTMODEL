#include "app/WebUi.h"
#include "core/Json.h"
#include "export/MeshExport.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <thread>

std::string WebRootOf(std::string_view gamePath) {
    std::string low = ToLower(std::string(gamePath));
    for (const char* r : {"/content/webroot/", "/content/uiresources/"}) {
        size_t p = low.find(r);
        if (p != std::string::npos) return std::string(gamePath.substr(0, p + strlen(r)));
    }
    return "";
}

bool IsWebPageFile(std::string_view name) { return IEndsWith(name, ".html") || IEndsWith(name, ".htm"); }

bool ExportWebRoot(Api& a, const std::string& root, const std::wstring& outputDir, std::wstring& localRoot, int* written, int* total,
                   std::string* error) {
    if (root.empty() || outputDir.empty()) {
        if (error) *error = root.empty() ? "not a web UI path" : "no output directory set";
        return false;
    }
    std::wstring base = outputDir + L"\\Web\\";
    localRoot = base + Widen(root);
    for (auto& c : localRoot) if (c == L'/') c = L'\\';
    while (!localRoot.empty() && localRoot.back() == L'\\') localRoot.pop_back();
    int n = 0, w = 0;
    std::string firstError;
    for (u32 i = 0; i < a.vfs.FileCount(); i++) {
        std::string_view p = a.vfs.Path(i);
        if (p.size() <= root.size() || !IStartsWith(p, root)) continue;
        n++;
        std::wstring out = base + Widen(p);
        for (auto& c : out) if (c == L'/') c = L'\\';
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fa) &&
            (((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) == a.vfs.Entry(i).usize)
            continue;
        Bytes data;
        std::string err;
        if (!a.vfs.Read(i, data, &err)) {
            if (firstError.empty()) firstError = std::string(p) + ": " + err;
            continue;
        }
        CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
        if (!WriteWholeFile(out, data.data(), data.size())) {
            if (firstError.empty()) firstError = "can't write " + Narrow(out);
            continue;
        }
        w++;
    }
    if (written) *written = w;
    if (total) *total = n;
    if (!n) {
        if (error) *error = "no files under " + root;
        return false;
    }
    if (error) *error = firstError;
    return true;
}

// ------------------------------------------------------------------ local web server

namespace {

// Stands in for the game inside the browser. The pages (Coherent GT) start on the engine's UIGTFinishedLoading event,
// wait for engine.call results and for request / response events on the ObjectMessaging dispatchers, read feature flags
// from featureConfig and get text from engine.translate / data-l10n-id: none of that exists outside the game, so the
// page stayed white.
const char* kShim = R"JS(
(function () {
    "use strict";
    var Q = {};
    location.search.substr(1).split("&").forEach(function (kv) {
        var i = kv.indexOf("=");
        if (i > 0) Q[kv.substr(0, i)] = decodeURIComponent(kv.substr(i + 1));
    });
    // Bluebird schedules on animation frames, which stop in background tabs: run on a timer too, whichever comes first
    var raf = window.requestAnimationFrame.bind(window), caf = window.cancelAnimationFrame.bind(window), frames = {}, nextFrame = 1;
    var dropFrame = function (id) {
        caf(frames[id].raf);
        clearTimeout(frames[id].timer);
        delete frames[id];
    };
    window.requestAnimationFrame = function (fn) {
        var id = nextFrame++;
        var run = function (t) {
            if (!frames[id]) return;
            dropFrame(id);
            fn(t === undefined ? performance.now() : t);
        };
        frames[id] = { raf: raf(run), timer: setTimeout(run, 50) };
        return id;
    };
    window.cancelAnimationFrame = function (id) { if (frames[id]) dropFrame(id); };
    // what Coherent's browser adds: toArray on element lists, static methods on the resolver-style Promise the HUD
    // scripts wrap the browser's in
    [window.NodeList, window.HTMLCollection].forEach(function (c) {
        if (c && !c.prototype.toArray) c.prototype.toArray = function () { return Array.prototype.slice.call(this); };
    });
    var NativePromise = window.Promise;
    var yes = function () { return true; }, no = function () { return false; };
    window.featureConfig = window.featureConfig || {
        isFeatureEnabled: yes, isPC: yes, isWin64: yes, hasKeyboard: yes, hasMouse: yes,
        isUWP: no, isXbox: no, isTechAlpha: no, isHDRAvailable: no, isHDREnabled: no, showPerformanceMode: no
    };
    var LOC = window.__sotLoc || {};
    var has = function (key) { return Object.prototype.hasOwnProperty.call(LOC, key); };
    var translate = function (key) { return has(key) ? LOC[key] : key; };
    var localize = function (el) {
        var key = el.getAttribute("data-l10n-id");
        if (key && el.__sotL10n !== key && has(key)) {
            el.__sotL10n = key;
            el.textContent = LOC[key];
        }
    };
    var localizeTree = function (root) {
        if (root.nodeType !== 1) return;
        if (root.hasAttribute("data-l10n-id")) localize(root);
        var list = root.querySelectorAll("[data-l10n-id]");
        for (var i = 0; i < list.length; i++) localize(list[i]);
    };
    document.addEventListener("DOMContentLoaded", function () {
        localizeTree(document.documentElement);
        new MutationObserver(function (records) {
            records.forEach(function (r) {
                if (r.type === "attributes") localize(r.target);
                else for (var i = 0; i < r.addedNodes.length; i++) localizeTree(r.addedNodes[i]);
            });
        }).observe(document.documentElement, { childList: true, subtree: true, attributes: true, attributeFilter: ["data-l10n-id"] });
    });
    var engine = window.engine;
    if (!engine || !engine._mocks) return;
    engine.translate = translate;
    // engine.call: answered by a mock when the page registered one, otherwise resolved with nothing
    var send = engine.SendMessage;
    engine.SendMessage = function (name, id) {
        if (engine._mocks[name] !== undefined) return send.apply(this, arguments);
        var deferred = engine._ActiveRequests[id];
        delete engine._ActiveRequests[id];
        setTimeout(function () { if (deferred) deferred.resolve(name === "GetLocale" ? (Q.lang || "en") : undefined); }, 0);
    };
    // ObjectMessaging: "FireEventTo<dispatcher>" requests get the matching registered response with known or empty data
    var KNOWN = {
        BootFlowStateQueryResponseEvent: function () { return { StateName: "BootFlowStart" + (Q.state || "Engage") + "StateEvent" }; },
        EventUserGameSettingsResponse: function () { return { CurrentSettings: {} }; },
        EventAthenaLocalPlayerRequestRequiredControllersConnectedResponse: function () { return { ControllerConnected: true }; },
        EventAthenaLoadingScreenStateResponse: function () { return { IsVisible: false }; }
    };
    var payload = function (type) {
        var o = KNOWN[type] ? KNOWN[type]() : {};
        o.__Type = type;
        return o;
    };
    var registered = {};
    var trigger = engine.trigger;
    var respond = function (dispatcher, name) {
        var names = registered[dispatcher] || {};
        var candidates = [name.replace(/Request(?!.*Request)/, "Response"), name.replace(/^Request/, "") + "Response",
                          name.replace(/QueryEvent$/, "QueryResponseEvent"), name.replace(/Request(Event)?$/, "$1"),
                          name.replace(/Request$/, "Change"), name.replace(/Request$/, "Changed")];
        for (var i = 0; i < candidates.length; i++) {
            if (candidates[i] !== name && names[candidates[i]]) {
                trigger.call(engine, "EventFiredOn" + dispatcher, payload(candidates[i]));
                return;
            }
        }
    };
    engine.trigger = function (name, arg) {
        var m = /^RegisterEventWith(.+)$/.exec(name);
        if (m) (registered[m[1]] = registered[m[1]] || {})[arg] = true;
        m = /^FireEventTo(.+)$/.exec(name);
        if (m && typeof arg === "string") setTimeout(function () { respond(m[1], arg); }, 0);
        return trigger.apply(this, arguments);
    };
    window.addEventListener("load", function () {
        engine.translate = translate;
        if (window.Promise !== NativePromise)
            ["resolve", "reject", "all", "race"].forEach(function (k) {
                if (typeof window.Promise[k] !== "function") window.Promise[k] = NativePromise[k].bind(NativePromise);
            });
        var core = window.core, roles = window.roles, menus = window.menus;
        // requests nothing answers resolve with empty data after a moment instead of hanging the page's start
        if (core && core.fireDispatcherRequest) {
            var request = core.fireDispatcherRequest;
            core.fireDispatcherRequest = function (dispatcher, req, resp) {
                var p = request.apply(this, arguments), P = p.constructor;
                var name = resp && resp.getEventName ? resp.getEventName() : "";
                return P.race([p, new P(function (resolve) { setTimeout(function () { resolve(payload(name)); }, 300); })]);
            };
        }
        if (roles && roles.DialogPlaceHolder) {
            var DP = roles.DialogPlaceHolder.prototype, init = DP.initDialog;
            DP.initDialog = function () {
                var p = init.apply(this, arguments), P = p.constructor;
                return P.race([p, new P(function (resolve) { setTimeout(resolve, 1500); })]);
            };
        }
        // settings the game never sent read as false / 0 / ""
        var settings = menus && menus.common && menus.common.settings && menus.common.settings.BaseGameSettings;
        if (settings) {
            [["getBooleanSetting", false], ["getNumberSetting", 0], ["getStringSetting", ""]].forEach(function (g) {
                var get = settings.prototype[g[0]];
                if (get) settings.prototype[g[0]] = function () { try { return get.apply(this, arguments); } catch (e) { return g[1]; } };
            });
        }
        setTimeout(function () {
            engine.trigger("Ready");
            engine.trigger("UIGTFinishedLoading");
        }, 0);
        // ?event=Dispatcher:EventName,... : game events sent once the page listens for them (the in-game menus open on one)
        var sendEvents = Q.event !== undefined ? Q.event : /\/menus\.html$/i.test(location.pathname) ? "HUD:EventPlayerOpenEscapeMenu" : "";
        sendEvents.split(",").forEach(function (ev) {
            var p = ev.indexOf(":"), dispatcher = ev.substr(0, p), name = ev.substr(p + 1), tries = 0;
            if (p <= 0) return;
            var poll = function () {
                if ((registered[dispatcher] || {})[name]) setTimeout(function () { trigger.call(engine, "EventFiredOn" + dispatcher, payload(name)); }, 500);
                else if (++tries < 120) setTimeout(poll, 250);
            };
            poll();
        });
    });
})();
)JS";

const char* kWebRoots[] = {"Athena/Content/WebRoot/", "Athena/Content/uiresources/"};

const char* ContentType(std::string_view path) {
    static const std::pair<const char*, const char*> types[] = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".json", "application/json"}, {".svg", "image/svg+xml"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"}, {".ico", "image/x-icon"},
        {".ttf", "font/ttf"}, {".otf", "font/otf"}, {".woff", "font/woff"}, {".woff2", "font/woff2"}, {".mp4", "video/mp4"},
        {".webm", "video/webm"}, {".ogg", "audio/ogg"}, {".wav", "audio/wav"}, {".mp3", "audio/mpeg"}, {".txt", "text/plain; charset=utf-8"}};
    for (auto& t : types) if (IEndsWith(path, t.first)) return t.second;
    return "application/octet-stream";
}

std::string PercentDecode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
}

struct WebServer {
    std::mutex mutex;
    Api* api = nullptr;
    SOCKET listener = INVALID_SOCKET;
    u16 port = 0;
    std::thread acceptThread;
    std::atomic<int> active{0};
    std::atomic<bool> stopping{false};
    std::string locJs;   // built on the first page
    std::unordered_map<std::string, std::shared_ptr<Bytes>> textures;   // object path -> PNG (null: failed)
};
WebServer g_web;

std::string LocalizationScript(Api& a) {
    std::shared_ptr<Localization> loc = a.localization;
    if (!loc || !loc->Size()) {
        loc = std::make_shared<Localization>();
        if (!loc->Load(a.vfs, "en")) loc.reset();
    }
    JsonWriter w;
    w.BeginObject();
    if (loc)
        for (auto& e : loc->Entries()) w.KV(e.ns + "." + e.key, e.text);
    w.EndObject();
    return "window.__sotLoc = " + w.Take() + ";\n";
}

// Texture2D'/Game/Path/Name.Name' anywhere in the URL (coui:// links and the pages' relative uses of it)
bool TexturePng(Api& a, const std::string& url, std::shared_ptr<Bytes>& out) {
    size_t p = url.find("Texture2D'");
    if (p == std::string::npos) return false;
    p += 10;
    size_t e = url.find('\'', p);
    std::string path = url.substr(p, e == std::string::npos ? std::string::npos : e - p);
    {
        std::lock_guard<std::mutex> lk(g_web.mutex);
        auto it = g_web.textures.find(path);
        if (it != g_web.textures.end()) {
            out = it->second;
            return true;
        }
    }
    auto png = std::make_shared<Bytes>();
    std::string err;
    if (!a.cache || !TextureToPng(*a.cache, path, 0, *png, &err)) {
        Log(LogLevel::Warn, "web UI: texture %s: %s", path.c_str(), err.c_str());
        png.reset();
    }
    std::lock_guard<std::mutex> lk(g_web.mutex);
    g_web.textures[path] = png;
    out = png;
    return true;
}

void SendAll(SOCKET s, const char* data, size_t size) {
    while (size) {
        int n = send(s, data, (int)std::min<size_t>(size, 1 << 20), 0);
        if (n <= 0) return;
        data += n;
        size -= (size_t)n;
    }
}

void Respond(SOCKET s, int status, const char* type, const void* body, size_t size, bool head) {
    const char* text = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Bad Request";
    std::string h = Fmt("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
                        status, text, type, size);
    SendAll(s, h.data(), h.size());
    if (!head && size) SendAll(s, (const char*)body, size);
}

void ServeClient(SOCKET s) {
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
    }
    size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        Respond(s, 400, "text/plain", "", 0, false);
        return;
    }
    std::string method = req.substr(0, sp1), target = req.substr(sp1 + 1, sp2 - sp1 - 1);
    bool head = method == "HEAD";
    if (method != "GET" && !head) {
        Respond(s, 405, "text/plain", "", 0, false);
        return;
    }
    std::string path = PercentDecode(target.substr(0, target.find('?')));
    Api& a = *g_web.api;
    auto notFound = [&] {
        std::string msg = "not in the archives: " + path;
        Respond(s, 404, "text/plain; charset=utf-8", msg.data(), msg.size(), head);
    };
    if (path == "/__sotmodel/shim.js") {
        Respond(s, 200, "text/javascript; charset=utf-8", kShim, strlen(kShim), head);
        return;
    }
    if (path == "/__sotmodel/loc.js") {
        std::string js;
        {
            std::lock_guard<std::mutex> lk(g_web.mutex);
            if (g_web.locJs.empty()) g_web.locJs = LocalizationScript(a);
            js = g_web.locJs;
        }
        Respond(s, 200, "text/javascript; charset=utf-8", js.data(), js.size(), head);
        return;
    }
    std::shared_ptr<Bytes> png;
    if (TexturePng(a, path, png)) {
        if (png) Respond(s, 200, "image/png", png->data(), png->size(), head);
        else notFound();
        return;
    }
    i64 entry = -1;
    if (IStartsWith(path, "/__coui/")) {   // coui://common/... : a file of the web roots
        std::string rest = path.substr(8);
        for (const char* r : kWebRoots)
            if ((entry = a.vfs.Find(r + rest)) >= 0) break;
    } else if (path.size() > 1) {
        entry = a.vfs.Find(path.substr(1));
    }
    Bytes data;
    if (entry < 0 || !a.vfs.Read((u32)entry, data)) {
        notFound();
        return;
    }
    const char* type = ContentType(path);
    bool page = IEndsWith(path, ".html") || IEndsWith(path, ".htm");
    if (page || IEndsWith(path, ".css") || IEndsWith(path, ".js")) {
        std::string text((const char*)data.data(), data.size());
        ReplaceAll(text, "coui://", "/__coui/");
        if (page && text.find("coherent.js") != std::string::npos) {   // a page (not a template the scripts load)
            std::string low = ToLower(text);
            size_t at = low.find("</head>");
            if (at == std::string::npos) at = low.find("<body");
            if (at != std::string::npos)
                // in the game the 3D view shows through; images the game never bound stay empty there
                text.insert(at, "<style>html { background: #1d2226; } img[src=\"\"], img:not([src]) { visibility: hidden; }</style>"
                                "<script src=\"/__sotmodel/loc.js\"></script><script src=\"/__sotmodel/shim.js\"></script>\n");
        }
        Respond(s, 200, type, text.data(), text.size(), head);
        return;
    }
    Respond(s, 200, type, data.data(), data.size(), head);
}

void AcceptLoop() {
    while (!g_web.stopping) {
        SOCKET c = accept(g_web.listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        g_web.active++;
        std::thread([c] {
            ServeClient(c);
            shutdown(c, SD_SEND);
            closesocket(c);
            g_web.active--;
        }).detach();
    }
}

}   // namespace

std::string WebUiUrl(Api& a, std::string_view gamePath, std::string* error) {
    {
        std::lock_guard<std::mutex> lk(g_web.mutex);
        if (g_web.listener == INVALID_SOCKET) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                if (error) *error = "WSAStartup failed";
                return "";
            }
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // this machine only, any free port
            int len = sizeof(addr);
            if (s == INVALID_SOCKET || bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, SOMAXCONN) != 0 ||
                getsockname(s, (sockaddr*)&addr, &len) != 0) {
                if (error) *error = Fmt("can't start the local web server (error %d)", WSAGetLastError());
                if (s != INVALID_SOCKET) closesocket(s);
                return "";
            }
            g_web.api = &a;
            g_web.listener = s;
            g_web.port = ntohs(addr.sin_port);
            g_web.stopping = false;
            g_web.acceptThread = std::thread(AcceptLoop);
            Log(LogLevel::Info, "web UI server on http://127.0.0.1:%u/", g_web.port);
        }
    }
    std::string url = Fmt("http://127.0.0.1:%u/", g_web.port);
    for (char c : gamePath) {
        if (isalnum((unsigned char)c) || strchr("/-_.~", c)) url += c;
        else url += Fmt("%%%02X", (unsigned char)c);
    }
    return url;
}

void StopWebUiServer() {
    {
        std::lock_guard<std::mutex> lk(g_web.mutex);
        if (g_web.listener == INVALID_SOCKET) return;
        g_web.stopping = true;
        closesocket(g_web.listener);
        g_web.listener = INVALID_SOCKET;
    }
    if (g_web.acceptThread.joinable()) g_web.acceptThread.join();
    for (int i = 0; i < 300 && g_web.active > 0; i++) Sleep(10);   // requests in flight still read the archives
    std::lock_guard<std::mutex> lk(g_web.mutex);
    g_web.textures.clear();
    g_web.locJs.clear();
}
