// Model Context Protocol server over stdio (newline-delimited JSON-RPC 2.0).
//   SOTModel.exe --mcp
// Register it in an MCP client (Claude Desktop / Claude Code / Cursor ...) as a stdio server; the tools are the Api tools.
#include "app/Api.h"
#include "core/Json.h"
#include <windows.h>

namespace {
HANDLE g_in, g_out;

bool ReadLine(std::string& line) {
    static std::string buf;
    for (;;) {
        size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        char tmp[65536];
        DWORD got = 0;
        if (!ReadFile(g_in, tmp, sizeof(tmp), &got, nullptr) || got == 0) return false;
        buf.append(tmp, got);
    }
}

void Send(const std::string& s) {
    std::string msg = s + "\n";
    DWORD w;
    WriteFile(g_out, msg.data(), (DWORD)msg.size(), &w, nullptr);
    FlushFileBuffers(g_out);
}

std::string IdJson(const JsonValue* id) { return id ? id->Dump() : "null"; }

void Reply(const JsonValue* id, const std::string& resultJson) { Send("{\"jsonrpc\":\"2.0\",\"id\":" + IdJson(id) + ",\"result\":" + resultJson + "}"); }

void ReplyError(const JsonValue* id, int code, const std::string& msg) {
    JsonValue m;
    m.type = JsonValue::Type::String;
    m.str = msg;
    Send("{\"jsonrpc\":\"2.0\",\"id\":" + IdJson(id) + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + m.Dump() + "}}");
}

std::string Quote(const std::string& s) {
    JsonValue v;
    v.type = JsonValue::Type::String;
    v.str = s;
    return v.Dump();
}
}

int RunMcp() {
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    Settings settings;
    settings.Load();
    Api api(settings);
    std::string line;
    while (ReadLine(line)) {
        if (line.empty()) continue;
        JsonValue msg;
        std::string perr;
        if (!JsonValue::Parse(line, msg, &perr)) { ReplyError(nullptr, -32700, "parse error: " + perr); continue; }
        const JsonValue* id = msg.Get("id");
        std::string method = msg.GetStr("method");
        const JsonValue* params = msg.Get("params");
        static const JsonValue empty = [] { JsonValue v; v.type = JsonValue::Type::Object; return v; }();
        if (!params) params = &empty;

        if (method == "initialize") {
            std::string version = params->GetStr("protocolVersion", "2024-11-05");
            Reply(id, "{\"protocolVersion\":" + Quote(version) +
                          ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"sotmodel\",\"version\":\"0.1.0\"},"
                          "\"instructions\":" + Quote("Sea of Thieves asset explorer. Start with 'info' or 'search', inspect with 'get_package' / "
                                                      "'get_properties', look at assets with preview_* tools, write files with 'export'. "
                                                      "Paths look like Athena/Content/Models/.../name.uasset.") + "}");
        } else if (method == "notifications/initialized" || method.rfind("notifications/", 0) == 0) {
            // no response to notifications
        } else if (method == "ping") {
            Reply(id, "{}");
        } else if (method == "tools/list") {
            std::string r = "{\"tools\":[";
            bool first = true;
            for (auto& t : api.Tools()) {
                if (!first) r += ",";
                first = false;
                r += "{\"name\":" + Quote(t.name) + ",\"description\":" + Quote(t.description) + ",\"inputSchema\":" + t.schema + "}";
            }
            r += "]}";
            Reply(id, r);
        } else if (method == "tools/call") {
            std::string name = params->GetStr("name");
            const JsonValue* args = params->Get("arguments");
            ApiResult res = api.Call(name, args ? *args : empty);
            std::string content = "[{\"type\":\"text\",\"text\":" + Quote(res.json) + "}";
            for (auto& im : res.images)
                content += ",{\"type\":\"image\",\"mimeType\":" + Quote(im.mime) + ",\"data\":\"" + Base64(im.data.data(), im.data.size()) + "\"}";
            content += "]";
            Reply(id, "{\"content\":" + content + ",\"isError\":" + (res.ok ? "false" : "true") + "}");
        } else if (method == "resources/list") {
            Reply(id, "{\"resources\":[]}");
        } else if (method == "prompts/list") {
            Reply(id, "{\"prompts\":[]}");
        } else if (id) {
            ReplyError(id, -32601, "method not found: " + method);
        }
    }
    return 0;
}
