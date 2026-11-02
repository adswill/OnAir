// MCP server: JSON-RPC 2.0 dispatch (initialize, tools, resources, prompts, ping), the activity log, the rate limit, the stdio transport and the
// streamable HTTP host. The tools are in mcp_tools.cpp.
#include "dect2/engine/engine.h"
#include "dect2/mcp/mcp.h"
#include "mcp_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace dect2 {
namespace mcp {

namespace {

double epochNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
double steadyNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

const char* kVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

Json rpcError(const Json& id, int code, const std::string& msg) {
    Json e = Json::object();
    e.setNum("code", code);
    e.setStr("message", msg);
    Json r = Json::object();
    r.setStr("jsonrpc", "2.0");
    r.set("id", id);
    r.set("error", e);
    return r;
}

Json rpcResult(const Json& id, Json result) {
    Json r = Json::object();
    r.setStr("jsonrpc", "2.0");
    r.set("id", id);
    r.set("result", std::move(result));
    return r;
}

Json textContent(const std::string& t) {
    Json c = Json::object();
    c.setStr("type", "text");
    c.setStr("text", t);
    return c;
}

std::string clip(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n) + "..."; }

} // namespace

struct Server::Impl {
    RemoteApiHost& host;
    Engine* eng;
    Options opt;
    mutable std::mutex mu;          // options, activity, rate window
    std::mutex toolMu;              // one tool call at a time
    std::deque<Activity> log;
    uint64_t seq = 0, calls = 0;
    std::deque<double> window;      // times of the recent calls
    std::function<void(const Activity&)> sink;
    double clientAt = -1e9;
    std::string clientName, protocol = "2025-06-18";

    Impl(RemoteApiHost& h, Engine* e, Options o) : host(h), eng(e), opt(std::move(o)) {}

    void record(const std::string& tool, const Json& args, bool ok, const std::string& outcome) {
        Activity a;
        a.time = epochNow();
        a.tool = tool;
        a.args = clip(args.isNull() ? std::string() : args.dump(), 300);
        a.ok = ok;
        a.outcome = clip(outcome, 300);
        std::function<void(const Activity&)> s;
        {
            std::lock_guard<std::mutex> lk(mu);
            a.seq = ++seq;
            log.push_back(a);
            while (log.size() > 500) log.pop_front();
            s = sink;
        }
        if (s) s(a);
    }

    bool rateOk() {
        std::lock_guard<std::mutex> lk(mu);
        const double t = steadyNow();
        while (!window.empty() && t - window.front() > 60) window.pop_front();
        if ((int)window.size() >= std::max(1, opt.maxCallsPerMinute)) return false;
        window.push_back(t);
        return true;
    }

    Options options() const { std::lock_guard<std::mutex> lk(mu); return opt; }

    // ---- tools
    Json toolJson(const Tool& t) const {
        Json o = Json::object();
        o.setStr("name", t.name);
        o.setStr("title", t.title);
        o.setStr("description", t.description);
        o.set("inputSchema", t.schema);
        Json an = Json::object();
        an.setStr("title", t.title);
        an.setBool("readOnlyHint", t.readOnly);
        an.setBool("destructiveHint", t.destructive);
        an.setBool("idempotentHint", t.idempotent);
        an.setBool("openWorldHint", false);
        o.set("annotations", an);
        return o;
    }

    Json callTool(const std::string& name, const Json& args) {
        const Options o = options();
        const Tool* tool = nullptr;
        for (const Tool& t : allTools()) if (t.name == name) { tool = &t; break; }
        Json res = Json::object();
        auto fail = [&](const std::string& m) {
            Json arr = Json::array();
            arr.push(textContent(m));
            res.set("content", arr);
            res.setBool("isError", true);
            record(name, args, false, m);
            return res;
        };
        if (!tool || (tool->needsTx && !o.allowTx)) return fail(tool ? "Transmit control is switched off in OnAir (Settings > AI assistant > Allow transmit)." : "Unknown tool '" + name + "'.");
        if (!args.isNull() && !args.isObj()) return fail("arguments must be an object");
        if (!rateOk()) return fail("Rate limit: more than " + std::to_string(o.maxCallsPerMinute) + " tool calls in a minute. Wait a little.");
        std::lock_guard<std::mutex> tl(toolMu);
        ToolCtx ctx{host, eng, o};
        try {
            Json out = tool->fn(ctx, args.isNull() ? Json::object() : args);
            std::string txt = out.dump();
            bool cut = false;
            if (txt.size() > o.maxResultBytes) { txt = txt.substr(0, o.maxResultBytes) + "... [result cut at " + std::to_string(o.maxResultBytes) + " bytes; ask for less, e.g. a smaller limit]"; cut = true; }
            Json arr = Json::array();
            arr.push(textContent(txt));
            res.set("content", arr);
            if (!cut && out.isObj()) res.set("structuredContent", out);
            res.setBool("isError", false);
            record(name, args, true, "ok");
        } catch (const ToolError& e) {
            return fail(e.what());
        } catch (const std::exception& e) {
            return fail(std::string("internal error: ") + e.what());
        }
        return res;
    }

    // ---- resources
    Json resourcesList() const {
        Json arr = Json::array();
        auto add = [&](const char* uri, const char* name, const char* desc, const char* mime) {
            Json r = Json::object();
            r.setStr("uri", uri); r.setStr("name", name); r.setStr("description", desc); r.setStr("mimeType", mime);
            arr.push(r);
        };
        add("onair://guide", "OnAir guide for AI assistants", "How to use the OnAir tools: the typical flow, limits and safety notes.", "text/plain");
        add("onair://status", "Receiver status", "Running, radio, mode, frequency, sample rate, gain.", "application/json");
        add("onair://modes", "Decoder modes", "All decoder modes with number, id, name, band and known frequencies.", "application/json");
        add("onair://activity", "AI activity log", "The tool calls an AI made in this session (what the user sees in OnAir).", "application/json");
        Json o = Json::object();
        o.set("resources", arr);
        return o;
    }

    Json templatesList() const {
        Json arr = Json::array();
        Json t = Json::object();
        t.setStr("uriTemplate", "onair://mode/{id}");
        t.setStr("name", "Mode information");
        t.setStr("description", "One decoder mode: number, id, band, known frequencies. {id} is an id, name or number from list_modes.");
        t.setStr("mimeType", "application/json");
        arr.push(t);
        Json t2 = Json::object();
        t2.setStr("uriTemplate", "onair://bandplan/{mhz}");
        t2.setStr("name", "Band plan at a frequency");
        t2.setStr("description", "What the built-in band plans say about a frequency in MHz.");
        t2.setStr("mimeType", "application/json");
        arr.push(t2);
        Json o = Json::object();
        o.set("resourceTemplates", arr);
        return o;
    }

    bool readResource(const std::string& uri, Json& out, std::string& err) {
        ToolCtx ctx{host, eng, options()};
        std::string text, mime = "application/json";
        if (uri == "onair://guide") { text = guideText(); mime = "text/plain"; }
        else if (uri == "onair://status") text = hostJson(host.statusJson()).dump();
        else if (uri == "onair://modes") {
            std::lock_guard<std::mutex> tl(toolMu);
            text = modesTable(ctx).dump();
        } else if (uri == "onair://activity") {
            Json arr = Json::array();
            std::lock_guard<std::mutex> lk(mu);
            for (const Activity& a : log) {
                Json j = Json::object();
                j.setNum("time", a.time); j.setStr("tool", a.tool); j.setStr("args", a.args); j.setBool("ok", a.ok); j.setStr("outcome", a.outcome);
                arr.push(j);
            }
            text = arr.dump();
        } else if (uri.compare(0, 13, "onair://mode/") == 0) {
            bool f = false;
            std::lock_guard<std::mutex> tl(toolMu);
            const Json j = resourceModeInfo(ctx, uri.substr(13), f);
            if (!f) { err = "no such mode"; return false; }
            text = j.dump();
        } else if (uri.compare(0, 16, "onair://bandplan") == 0 && uri.size() > 17) {
            char* e = nullptr;
            const double mhz = strtod(uri.c_str() + 17, &e);
            if (!e || *e || !std::isfinite(mhz)) { err = "the frequency must be a number in MHz"; return false; }
            text = bandPlanFor(mhz * 1e6).dump();
        } else { err = "unknown resource"; return false; }
        Json c = Json::object();
        c.setStr("uri", uri);
        c.setStr("mimeType", mime);
        c.setStr("text", text);
        Json arr = Json::array();
        arr.push(c);
        out = Json::object();
        out.set("contents", arr);
        return true;
    }

    // ---- prompts
    struct PromptDef { const char* name; const char* title; const char* desc; std::vector<std::pair<const char*, std::pair<const char*, bool>>> args; };
    static const std::vector<PromptDef>& prompts() {
        static const std::vector<PromptDef> p = {
            {"find_and_decode", "Find a signal and decode it", "Find a kind of signal near a frequency and start decoding it.",
             {{"signal", {"What to look for, e.g. DRM, ADS-B, pagers, a ham repeater.", true}}, {"frequency_mhz", {"Roughly where, in MHz.", false}}}},
            {"survey_band", "Survey a band", "List and identify everything in a frequency range.",
             {{"start_mhz", {"Lower edge, MHz.", true}}, {"stop_mhz", {"Upper edge, MHz.", true}}}},
            {"why_not_decoding", "Why is nothing decoding?", "Diagnose why the receiver is not decoding and suggest fixes.", {}},
        };
        return p;
    }
    Json promptsList() const {
        Json arr = Json::array();
        for (const PromptDef& d : prompts()) {
            Json p = Json::object();
            p.setStr("name", d.name); p.setStr("title", d.title); p.setStr("description", d.desc);
            Json a = Json::array();
            for (const auto& x : d.args) {
                Json q = Json::object();
                q.setStr("name", x.first); q.setStr("description", x.second.first); q.setBool("required", x.second.second);
                a.push(q);
            }
            p.set("arguments", a);
            arr.push(p);
        }
        Json o = Json::object();
        o.set("prompts", arr);
        return o;
    }
    bool getPrompt(const std::string& name, const Json& args, Json& out, std::string& err) {
        std::string text;
        if (name == "find_and_decode") {
            const std::string sig = args.strOr("signal"), f = args.strOr("frequency_mhz");
            if (sig.empty()) { err = "argument 'signal' is required"; return false; }
            text = "Use the OnAir tools to find " + sig + (f.empty() ? "" : " around " + f + " MHz") + " and decode it. Start with get_status, then call auto_decode" +
                   (f.empty() ? "" : " with frequency_mhz " + f) + " and mode set to the matching id from list_modes. If it does not lock, call get_receiver_health, fix what it suggests (gain, sample rate, frequency) and try again. "
                   "When it locks, summarise what you decoded (get_decoder_state, get_messages) and tell me how reliable it is.";
        } else if (name == "survey_band") {
            const std::string a = args.strOr("start_mhz"), b = args.strOr("stop_mhz");
            if (a.empty() || b.empty()) { err = "arguments 'start_mhz' and 'stop_mhz' are required"; return false; }
            text = "Survey " + a + " - " + b + " MHz with OnAir: call get_status, set a wide sample rate if the radio allows, then scan_band. Report each signal with its frequency, bandwidth and what it most likely is (and how sure the identifier is), "
                   "and use search_frequencies / get_band_plan to say what is expected there. Offer to decode the interesting ones.";
        } else if (name == "why_not_decoding") {
            text = "OnAir is not decoding anything. Call get_status and get_receiver_health, look at get_spectrum, and explain in plain words what is wrong and what I should change (antenna, gain, frequency, sample rate, mode). Apply safe fixes yourself (gain, sample rate) and tell me what you changed.";
        } else { err = "unknown prompt"; return false; }
        Json m = Json::object();
        m.setStr("role", "user");
        m.set("content", textContent(text));
        Json arr = Json::array();
        arr.push(m);
        out = Json::object();
        out.setStr("description", name);
        out.set("messages", arr);
        return true;
    }

    // ---- dispatch
    std::string handle(const std::string& text) {
        Json msg;
        std::string perr;
        if (!parseJson(text, msg, perr)) return rpcError(Json(), -32700, "Parse error: " + perr).dump();
        if (msg.isArr()) return rpcError(Json(), -32600, "Batches are not supported").dump();
        if (!msg.isObj()) return rpcError(Json(), -32600, "Invalid request").dump();
        const Json* idp = msg.get("id");
        const Json id = idp ? *idp : Json();
        const bool isNotification = idp == nullptr;
        const Json* mp = msg.get("method");
        if (!mp) {   // a response from the client to a request we never made: ignore
            if (msg.has("result") || msg.has("error")) return std::string();
            return rpcError(id, -32600, "Invalid request: no method").dump();
        }
        if (!mp->isStr() || msg.strOr("jsonrpc") != "2.0") return rpcError(id, -32600, "Invalid request: jsonrpc must be \"2.0\" and method a string").dump();
        const std::string method = mp->str;
        if (idp && !(id.isStr() || id.isNum())) return rpcError(Json(), -32600, "Invalid request: id must be a string or a number").dump();
        const Json* pp = msg.get("params");
        const Json params = pp ? *pp : Json::object();
        if (pp && !pp->isObj()) return isNotification ? std::string() : rpcError(id, -32602, "params must be an object").dump();

        if (isNotification) {   // notifications/initialized, notifications/cancelled ... nothing to answer
            return std::string();
        }
        if (method == "initialize") {
            std::string want = params.strOr("protocolVersion");
            std::string use = kVersions[0];
            for (const char* v : kVersions) if (want == v) use = v;
            Json info = Json::object();
            const Options o = options();
            info.setStr("name", o.serverName);
            info.setStr("title", "OnAir software-defined radio");
            info.setStr("version", o.serverVersion);
            Json caps = Json::object();
            Json tools = Json::object(); tools.setBool("listChanged", false);
            Json res = Json::object(); res.setBool("subscribe", false); res.setBool("listChanged", false);
            Json pr = Json::object(); pr.setBool("listChanged", false);
            caps.set("tools", tools); caps.set("resources", res); caps.set("prompts", pr);
            Json r = Json::object();
            r.setStr("protocolVersion", use);
            r.set("capabilities", caps);
            r.set("serverInfo", info);
            r.setStr("instructions", "OnAir drives a software-defined radio. Read the onair://guide resource first. Typical: get_status, then auto_decode {frequency_mhz, mode}. All your calls are shown to the user.");
            {
                std::lock_guard<std::mutex> lk(mu);
                clientAt = steadyNow();
                protocol = use;
                const Json* ci = params.get("clientInfo");
                clientName = ci ? ci->strOr("name") : std::string();
            }
            record("initialize", params.get("clientInfo") ? *params.get("clientInfo") : Json(), true, "protocol " + use);
            return rpcResult(id, r).dump();
        }
        { std::lock_guard<std::mutex> lk(mu); clientAt = steadyNow(); }
        if (method == "ping") return rpcResult(id, Json::object()).dump();
        if (method == "tools/list") {
            Json arr = Json::array();
            const Options o = options();
            for (const Tool& t : allTools()) if (!t.needsTx || o.allowTx) arr.push(toolJson(t));
            Json r = Json::object();
            r.set("tools", arr);
            return rpcResult(id, r).dump();
        }
        if (method == "tools/call") {
            const std::string name = params.strOr("name");
            if (name.empty()) return rpcError(id, -32602, "params.name is required").dump();
            bool known = false;
            for (const Tool& t : allTools()) if (t.name == name) known = true;
            if (!known) return rpcError(id, -32602, "Unknown tool: " + name).dump();
            const Json* a = params.get("arguments");
            return rpcResult(id, callTool(name, a ? *a : Json::object())).dump();
        }
        if (method == "resources/list") return rpcResult(id, resourcesList()).dump();
        if (method == "resources/templates/list") return rpcResult(id, templatesList()).dump();
        if (method == "resources/read") {
            const std::string uri = params.strOr("uri");
            Json out;
            std::string err;
            if (!readResource(uri, out, err)) { record("resources/read", params, false, err); return rpcError(id, -32002, err + ": " + uri).dump(); }
            record("resources/read", params, true, uri);
            return rpcResult(id, out).dump();
        }
        if (method == "prompts/list") return rpcResult(id, promptsList()).dump();
        if (method == "prompts/get") {
            Json out;
            std::string err;
            const Json* a = params.get("arguments");
            if (!getPrompt(params.strOr("name"), a ? *a : Json::object(), out, err)) return rpcError(id, -32602, err).dump();
            record("prompts/get", params, true, params.strOr("name"));
            return rpcResult(id, out).dump();
        }
        if (method == "logging/setLevel" || method == "completion/complete") return rpcResult(id, Json::object()).dump();
        return rpcError(id, -32601, "Method not found: " + method).dump();
    }
};

Server::Server(RemoteApiHost& host, Engine* engine, Options o) : p_(new Impl(host, engine, std::move(o))) {}
Server::~Server() = default;
void Server::setOptions(const Options& o) { std::lock_guard<std::mutex> lk(p_->mu); p_->opt = o; }
Options Server::options() const { return p_->options(); }
std::string Server::handleMessage(const std::string& text) {
    try { return p_->handle(text); }
    catch (const std::exception& e) { return rpcError(Json(), -32603, std::string("Internal error: ") + e.what()).dump(); }
}
Json Server::toolsList() const {
    Json arr = Json::array();
    const Options o = p_->options();
    for (const Tool& t : allTools()) if (!t.needsTx || o.allowTx) arr.push(p_->toolJson(t));
    return arr;
}
Json Server::callTool(const std::string& name, const Json& args) { return p_->callTool(name, args); }
std::vector<Activity> Server::activity(uint64_t since) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Activity> v;
    for (const Activity& a : p_->log) if (a.seq > since) v.push_back(a);
    return v;
}
uint64_t Server::calls() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->seq; }
void Server::setActivitySink(std::function<void(const Activity&)> f) { std::lock_guard<std::mutex> lk(p_->mu); p_->sink = std::move(f); }
void Server::clearActivity() { std::lock_guard<std::mutex> lk(p_->mu); p_->log.clear(); }
bool Server::clientSeen(std::string* name) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (steadyNow() - p_->clientAt > 300) return false;
    if (name) *name = p_->clientName;
    return true;
}

void runStdio(Server& s, std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const std::string r = s.handleMessage(line);
        if (!r.empty()) { out << r << '\n'; out.flush(); }
    }
}

int McpHttpHost::handleMcp(const std::string& method, const std::string& body, std::string& out) {
    if (method == "GET") { out = "this server has no event stream; POST JSON-RPC to /mcp"; return 405; }   // allowed by the spec: no SSE stream offered
    if (method == "DELETE") { out.clear(); return 200; }                                                     // no sessions to end
    if (body.empty()) { out = "{\"error\":\"empty body\"}"; return 400; }
    out = s_.handleMessage(body);
    return out.empty() ? 202 : 200;
}

std::string snippetClaudeDesktop(const std::string& exePath, const std::string& url, const std::string& token) {
    auto esc = [](const std::string& s) { std::string o; for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; } return o; };
    if (!exePath.empty())
        return "{\n  \"mcpServers\": {\n    \"onair\": {\n      \"command\": \"" + esc(exePath) + "\",\n      \"args\": [\"--mcp-stdio\"]\n    }\n  }\n}\n";
    std::string a = "\"-y\", \"mcp-remote\", \"" + esc(url) + "\"";
    if (!token.empty()) a += ", \"--header\", \"Authorization:Bearer " + esc(token) + "\"";
    return "{\n  \"mcpServers\": {\n    \"onair\": {\n      \"command\": \"npx\",\n      \"args\": [" + a + "]\n    }\n  }\n}\n";
}

std::string snippetClaudeCode(const std::string& url, const std::string& token) {
    std::string c = "claude mcp add --transport http onair " + url;
    if (!token.empty()) c += " --header \"Authorization: Bearer " + token + "\"";
    return c + "\n";
}

std::string snippetGeneric(const std::string& url, const std::string& token, const std::string& exePath) {
    std::string s = "Streamable HTTP endpoint: " + url + "\n";
    if (!token.empty()) s += "Header: Authorization: Bearer " + token + "\n";
    if (!exePath.empty()) s += "Or stdio: command \"" + exePath + "\" with argument --mcp-stdio\n";
    s += "Protocol: MCP 2025-06-18 (also 2025-03-26, 2024-11-05), JSON-RPC 2.0, tools / resources / prompts.\n";
    return s;
}

} // namespace mcp
} // namespace dect2
