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
const char* kShim =   // two literals: MSVC limits one string literal piece to 16 KB
R"JS(
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
        frames[id] = { raf: raf(run), timer: setTimeout(run, 20) };
        return id;
    };
    window.cancelAnimationFrame = function (id) { if (frames[id]) dropFrame(id); };
    // what Coherent's browser adds: toArray on element lists
    [window.NodeList, window.HTMLCollection].forEach(function (c) {
        if (c && !c.prototype.toArray) c.prototype.toArray = function () { return Array.prototype.slice.call(this); };
    });
    var NativePromise = (async function () {})().constructor;   // window.Promise may already be Bluebird
    // the HUD (window.Promise native after Bluebird.noConflict) uses Coherent's old DOM-Promise form,
    // new Promise(function (resolver) { resolver.fulfill(v); }), and wraps Promise itself (without fulfill) unless PromiseResolver exists
    if (window.Promise === NativePromise) {
        window.PromiseResolver = window.PromiseResolver || function PromiseResolver() {};
        window.Promise = class extends NativePromise {
            constructor(executor) {
                super(function (resolve, reject) {
                    resolve.fulfill = resolve.resolve = resolve;
                    resolve.reject = reject;
                    return executor(resolve, reject);
                });
            }
        };
    }
    // the HUD takes "Promise.try || Bluebird.try" and calls it unbound: Coherent had no Promise.try, newer browsers do (and need "this")
    if (typeof Promise.try === "function") {
        var nativeTry = Promise.try;
        Promise.try = function () { return nativeTry.apply(typeof this === "function" ? this : NativePromise, arguments); };
    }
    var yes = function () { return true; }, no = function () { return false; };
    // the game's feature flags (Features.json is not in the archives): all on except the unfinished grey-tile front end and the
    // developer button; ?features=Name,-Name turns flags on / off
    var FEATURES = { UIDynamicFrontend: false, UIEnableDebugFeatureButton: false };
    (Q.features || "").split(",").forEach(function (f) { if (f) FEATURES[f.replace(/^-/, "")] = f[0] !== "-"; });
    window.featureConfig = window.featureConfig || {
        isFeatureEnabled: function (name) { return FEATURES[name] !== false; }, isPC: yes, isWin64: yes, hasKeyboard: yes, hasMouse: yes,
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
    // sample data for what the game would send (responses, and PUSH events sent once the page listens for them)
    var GAMERTAG = Q.name || "Pirate";
    var KNOWN = {
        BootFlowStateQueryResponseEvent: function () { return { StateName: stateEvent() }; },
        EventUserGameSettingsResponse: function () { return { CurrentSettings: {} }; },
        EventAthenaLocalPlayerRequestRequiredControllersConnectedResponse: function () { return { ControllerConnected: true }; },
        EventAthenaLoadingScreenStateResponse: function () { return { IsVisible: false }; },
        EventAthenaLocalPlayerControllerTypeChange: function () { return { ControllerType: Q.gamepad ? 0 : 1 }; },
        PlayerInfoResponse: function () { return { GamerTag: GAMERTAG, Title: "Legendary Pirate" }; },
        GamerTagUpdated: function () { return { GamerTag: GAMERTAG }; },
        EventCompanyDataResponse: function () { return { CompanyData: [] }; },
        EventGetAllKeybindingsResponse: function () { return keyBindings(); },
        EventScreenSizeInfo: function () {
            return { ScreenWidth: 1920, ScreenHeight: 1080, WindowWidth: 1920, WindowHeight: 1080, AutoDetectedMaxVerticalResolution: 2160,
                     AutoDetectedMachineSpec: 3, IsFullScreen: true };
        },
        EventCompanyProgressData: function () { return { CompanyProgress: [] }; },
        BootFlowErrorMessageQueryResponseEvent: function () {
            return { ErrorMessage: Q.error || "The servers are not reachable from this preview: it runs the game's interface without the game." };
        },
        SetFrontEndSelection: function () { return { IsAdventureActive: true, IsArenaActive: true, IsTutorialActive: true, IsCreativeActive: false }; },
        SetContestShipTypes: function () {
            return { IsGalleonEnabled: true, IsBrigantineEnabled: true, IsSloopEnabled: true, IsSloopDuoEnabled: true, IsMixedGalleonBrigantineEnabled: false };
        },
        CrewUpdateEvent: function () {   // ?crew=N members (1..4), this player the captain
            var n = Math.max(1, Math.min(4, parseInt(Q.crew, 10) || 1)), members = [];
            for (var i = 0; i < n; i++)
                members.push({ GamerTag: i ? "Crewmate " + i : GAMERTAG, CurrentPlayer: !i, Captain: !i, ReservedForPlayerThatDroppedConnection: false });
            return { CrewDefinition: { CrewMembers: members } };
        },
        RequestLobbyUIEvent: function () { return { SessionType: shipType(), SessionVisibility: 1 }; },
        SessionTypeSelectionResponseEvent: function () { return { SessionType: shipType() }; },
        UpdateWalletBalanceHUDEvent: function () {
            return { Balances: [{ CurrencyId: 1, Balance: 125000, DisplayBalance: "125,000" }, { CurrencyId: 2, Balance: 40, DisplayBalance: "40" },
                                { CurrencyId: 3, Balance: 1500, DisplayBalance: "1,500" }] };
        }
    };
)JS"
R"JS(    // default key bindings: Config/DefaultInput.ini has the keys per input action / axis, DefaultGame.ini's Virtual*BindingDefinitions
    // the settings' binding names (labels) over those actions
    var configText = function (path) {
        return fetch(path).then(function (r) { return r.ok ? r.text() : ""; }).catch(function () { return ""; });
    };
    var bindingsData = null;
    var keyBindings = function () {
        if (!bindingsData) bindingsData = NativePromise.all([configText("/Athena/Config/DefaultInput.ini"), configText("/Athena/Config/DefaultGame.ini")]).then(function (t) {
            var keys = {};   // action / axis+ / axis- -> [keyboard key, gamepad key]
            t[0].split("\n").forEach(function (line) {
                var m = /^\+?(Action|Axis)Mappings=\((?:Action|Axis)Name="([^"]+)",Key=(\w+)(?:.*Scale=(-?[\d.]+))?/.exec(line.trim());
                if (!m) return;
                var name = m[2] + (m[1] === "Axis" ? (parseFloat(m[4]) < 0 ? "-" : "+") : "");
                var pad = /^Gamepad_/.test(m[3]) ? 1 : 0;
                (keys[name] = keys[name] || [])[pad] = keys[name][pad] || m[3];
            });
            var action = [], axis = [];
            t[1].split("\n").forEach(function (line) {
                var m = /^\+Virtual(Action|Axis)BindingDefinitions=\((?:Action|Axis)Names=\(([^)]*)\).*BindingName="([^"]+)"(?:,Scale=(-?[\d.]+))?/.exec(line.trim());
                if (!m) return;
                var isAxis = m[1] === "Axis", names = m[2].split(",").map(function (n) { return n.replace(/"/g, "") + (isAxis ? (parseFloat(m[4]) < 0 ? "-" : "+") : ""); });
                var key = function (pad) {
                    for (var i = 0; i < names.length; i++) if (keys[names[i]] && keys[names[i]][pad]) return keys[names[i]][pad];
                    return "None";
                };
                (isAxis ? axis : action).push({ BindingName: m[3], KeyName: key(0), ControllerType: 1 }, { BindingName: m[3], KeyName: key(1), ControllerType: 0 });
            });
            return { ActionBindings: action, AxisBindings: axis, DefaultKeyboardLayout: 0 };
        });
        return bindingsData.then(function (d) { return JSON.parse(JSON.stringify(d)); });
    };
    var shipType = function () {   // ?ship=sloop / brigantine / galleon
        return { sloop: 1, brigantine: 2, galleon: 3 }[(Q.ship || "").toLowerCase()] || (Math.max(1, Math.min(4, parseInt(Q.crew, 10) || 1)) > 2 ? 3 : 1);
    };
    var PUSH = ["GamerTagUpdated", "UpdateWalletBalanceHUDEvent", "EventAthenaLocalPlayerControllerTypeChange", "SetFrontEndSelection",
                "SetContestShipTypes", "CrewUpdateEvent"];
    if (/Lobby/.test(Q.state || "")) PUSH.push("RequestLobbyUIEvent");
    var stateEvent = function () {
        var st = Q.state || "Engage";
        return /Event(_\w+)?$/.test(st) ? st : (st === "WaitForEntryLevel" || st === "LoadIntoGame") ? "BootFlowStart" + st + "Event" : "BootFlowStart" + st + "StateEvent";
    };
    var payload = function (type) {
        var o = KNOWN[type] ? KNOWN[type]() : {};
        if (o && typeof o.then === "function") return o.then(function (v) { v.__Type = type; return v; });
        if (Q.debug) console.log("SOT " + (KNOWN[type] ? "data " : "empty ") + type);
        o.__Type = type;
        return o;
    };
    var registered = {};
    var fire = function (dispatcher, type) {
        NativePromise.resolve(payload(type)).then(function (p) { trigger.call(engine, "EventFiredOn" + dispatcher, p); });
    };
    var trigger = engine.trigger;
    var respond = function (dispatcher, name) {
        var names = registered[dispatcher] || {};
        var candidates = [name.replace(/Request(?!.*Request)/, "Response"), name.replace(/^Request/, "") + "Response",
                          name.replace(/QueryEvent$/, "QueryResponseEvent"), name.replace(/Request(Event)?$/, "$1"),
                          name.replace(/Request$/, "Change"), name.replace(/Request$/, "Changed"), name.replace(/Request$/, "Data")];
        if (name === "RequestCurrentCrewSessionEvent") candidates.unshift("CrewUpdateEvent");
        if (Q.debug) console.log("SOT request " + dispatcher + ":" + name);
        for (var i = 0; i < candidates.length; i++) {
            if (candidates[i] !== name && names[candidates[i]]) {
                fire(dispatcher, candidates[i]);
                return;
            }
        }
    };
    engine.trigger = function (name, arg) {
        var m = /^RegisterEventWith(.+)$/.exec(name);
        if (m) {
            var first = !(registered[m[1]] || {})[arg];
            (registered[m[1]] = registered[m[1]] || {})[arg] = true;
            if (first && PUSH.indexOf(arg) >= 0) {
                var d = m[1];
                setTimeout(function () { fire(d, arg); }, 50);
            }
        }
        m = /^FireEventTo(.+)$/.exec(name);
        if (m && typeof arg === "string") setTimeout(function () { respond(m[1], arg); }, 0);
        return trigger.apply(this, arguments);
    };
    window.addEventListener("load", function () {
        engine.translate = translate;
        // the pages run Bluebird on animation frames (one promise step per frame, the start takes minutes in a browser):
        // microtasks as in any browser
        [window.Promise, window.Bluebird].forEach(function (B) {
            if (B && typeof B.setScheduler === "function") B.setScheduler(queueMicrotask.bind(window));
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
        // components start one after another: one waiting for data that never comes would stall the rest of the dialog
        var templates = window.templates;
        if (templates && templates.TemplateMountLifecycle) {
            var LP = templates.TemplateMountLifecycle.prototype;
            var limit = function (P, value, mount, what) {
                if (!value || typeof value.then !== "function") return value;
                return new P(function (resolve, reject) {
                    var t = setTimeout(function () {
                        if (Q.debug) console.log("SOT " + what + " timeout " + (mount.constructor && mount.constructor.name));
                        resolve();
                    }, 2000);
                    value.then(function (v) { clearTimeout(t); resolve(v); }, function (e) { clearTimeout(t); reject(e); });
                });
            };
            LP.recursivelyInit = function (mount) {
                var self = this, P = window.Promise;
                return P.try(function () { return limit(P, mount.preInit(), mount, "preInit"); })
                    .then(function () { return self.recursivelyInitGroup(mount.mounts); })
                    .then(function () { return limit(P, mount.init(), mount, "init"); });
            };
        }
        // settings the game never sent start as false / 0 / "" / no changed key bindings (stored, so they can be changed)
        var settings = menus && menus.common && menus.common.settings && menus.common.settings.BaseGameSettings;
        var numberDefault = function (name) {   // usual values rather than the lowest of every list / slider
            var fixed = { FieldOfView: 90, BackBufferCount: 2, ResolutionScaling: 100, ParticleEmitterQuality: 2, MouseSensitivity: 50, ControllerSensitivity: 50 };
            if (fixed[name] !== undefined) return fixed[name];
            if (/Volume$/.test(name)) return 80;
            if (/(Detail|Quality)$/.test(name)) return 3;
            return 0;
        };
        if (settings) {
            [["getBooleanSetting", false], ["getNumberSetting", 0], ["getStringSetting", ""], ["getKeyBindingSetting", null]].forEach(function (g) {
                var get = settings.prototype[g[0]];
                if (get) settings.prototype[g[0]] = function (name) {
                    try { return get.apply(this, arguments); } catch (e) {
                        if (!this.currentSettings) this.currentSettings = {};
                        return this.currentSettings[name] = g[1] === null ? [] : g[0] === "getNumberSetting" ? numberDefault(name) : g[1];
                    }
                };
            });
            ["setBooleanSetting", "setFloatSetting", "setIntegerSetting", "setStringSetting", "setKeyBindingSetting"].forEach(function (n) {
                var set = settings.prototype[n];
                if (set) settings.prototype[n] = function (name, value) {
                    if (!this.currentSettings) this.currentSettings = {};
                    if (typeof this.currentSettings[name] !== typeof value) this.currentSettings[name] = value;
                    return set.apply(this, arguments);
                };
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
                if ((registered[dispatcher] || {})[name]) setTimeout(function () { fire(dispatcher, name); }, 500);
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
