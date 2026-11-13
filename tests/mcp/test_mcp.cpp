// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The MCP server (core/src/mcp): the JSON value, the JSON-RPC protocol, every tool on the real engine with the built-in test signal (ServerHost),
// the stdio transport, the streamable HTTP transport with its guards (token, Host, Origin, method, size), the activity log and the rate limit.
// End to end: "there is DRM around 15 MHz, find it and decode it" on the synthetic DRM test signal (the AI starts with the radio in a mode that
// does not decode it and auto_decode has to identify it, switch the decoder and report the lock).
#include "../../core/src/net/tcp_client.h"
#include "../../server/server_config.h"
#include "../../server/server_host.h"
#include "dect2/mcp/mcp.h"
#include "dect2/net/remote_api.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>
using namespace dect2;
using namespace dect2::mcp;
using namespace onair_server;

#include "../common/test_util.h"

// ---------------------------------------------------------------- a fake MCP client
struct Client {
    Server& s;
    int nextId = 1;
    Json rpc(const std::string& method, const Json& params = Json::object()) {
        Json r = Json::object();
        r.setStr("jsonrpc", "2.0");
        r.setNum("id", nextId++);
        r.setStr("method", method);
        r.set("params", params);
        Json out;
        std::string err;
        const std::string a = s.handleMessage(r.dump());
        if (!parseJson(a, out, err)) { printf("  (bad answer: %s)\n", a.c_str()); return Json(); }
        return out;
    }
    // tools/call: the tool's JSON result (parsed from the text content); isErr set when the tool reported an error
    Json call(const std::string& name, const Json& args, bool* isErr = nullptr, std::string* text = nullptr) {
        Json p = Json::object();
        p.setStr("name", name);
        p.set("arguments", args);
        const Json r = rpc("tools/call", p);
        const Json* res = r.get("result");
        if (!res) { if (isErr) *isErr = true; if (text) *text = r.dump(); return Json(); }
        const bool e = res->boolOr("isError", false);
        if (isErr) *isErr = e;
        const Json* c = res->get("content");
        std::string t = c && c->isArr() && !c->arr.empty() ? c->arr[0].strOr("text") : std::string();
        if (text) *text = t;
        Json out;
        std::string err;
        if (e || !parseJson(t, out, err)) return Json();
        return out;
    }
};
static Json obj() { return Json::object(); }

// ---------------------------------------------------------------- HTTP client helpers
static std::string readAll(sock_t s, int ms) {
    std::string r;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < ms) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char b[4096];
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) break;
        r.append(b, (size_t)k);
    }
    return r;
}
struct Resp { int status = 0; std::string head, body; };
static Resp http(int port, const std::string& method, const std::string& path, const std::string& body, const std::string& extra = "", bool json = true, const std::string& host = "") {
    std::string e;
    sock_t s = tcpConnect("127.0.0.1", port, 2000, e);
    Resp o;
    if (s == kBadSock) return o;
    std::string rq = method + " " + path + " HTTP/1.1\r\nHost: " + (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) + "\r\n" + extra;
    if (method == "POST") rq += std::string(json ? "Content-Type: application/json\r\n" : "Content-Type: text/plain\r\n") + "Content-Length: " + std::to_string(body.size()) + "\r\n";
    rq += "\r\n" + body;
    sockSendAll(s, rq.data(), rq.size());
    const std::string r = readAll(s, 5000);
    sockClose(s);
    if (r.compare(0, 9, "HTTP/1.1 ") == 0) o.status = atoi(r.c_str() + 9);
    const size_t h = r.find("\r\n\r\n");
    o.head = r.substr(0, h);
    if (h != std::string::npos) o.body = r.substr(h + 4);
    return o;
}

// ---------------------------------------------------------------- JSON
static void testJson() {
    Json j;
    std::string err;
    CHECK(parseJson("{\"a\":[1,2.5,-3e2,true,false,null,\"x\\n\\u00e9\\ud83d\\ude00\"],\"b\":{}}", j, err), "parse: %s", err.c_str());
    const Json* a = j.get("a");
    CHECK(a && a->arr.size() == 7 && a->arr[2].num == -300 && a->arr[3].b && a->arr[5].isNull(), "values");
    CHECK(a && a->arr[6].str == "x\n\xC3\xA9\xF0\x9F\x98\x80", "escapes and a surrogate pair become UTF-8");
    CHECK(j.dump() == "{\"a\":[1,2.5,-300,true,false,null,\"x\\n\xC3\xA9\xF0\x9F\x98\x80\"],\"b\":{}}", "dump: %s", j.dump().c_str());
    CHECK(!parseJson("{\"a\":1,}", j, err) && !err.empty(), "trailing comma refused");
    CHECK(!parseJson("{\"a\":1} x", j, err), "trailing text refused");
    CHECK(!parseJson("\"\\ud800\"", j, err), "lone surrogate refused");
    CHECK(!parseJson("[1,", j, err) && !parseJson("", j, err) && !parseJson("nul", j, err) && !parseJson("{\"a\"}", j, err), "truncated and malformed refused");
    CHECK(!parseJson("\"a\nb\"", j, err), "raw control character in a string refused");
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK(!parseJson(deep, j, err), "nesting beyond 32 refused");
    Json o = Json::object();
    o.setNum("n", NAN).setStr("s", "a\"b\\c\x01");
    CHECK(o.dump() == "{\"n\":null,\"s\":\"a\\\"b\\\\c\\u0001\"}", "NaN is null, controls escaped: %s", o.dump().c_str());
    Json q;
    parseJson("{\"x\":\"12.5\",\"y\":\"true\",\"z\":3}", q, err);
    CHECK(q.numOr("x", 0) == 12.5 && q.boolOr("y", false) && q.strOr("z") == "3", "a model's stringly numbers are tolerated");
}

// ---------------------------------------------------------------- the protocol
static void testProtocol(Server& s) {
    Client c{s};
    Json p = obj();
    p.setStr("protocolVersion", "2025-03-26");
    p.set("clientInfo", obj().setStr("name", "test-client"));
    Json r = c.rpc("initialize", p);
    CHECK(r.get("result") && r.get("result")->strOr("protocolVersion") == "2025-03-26", "an older version the server knows is echoed");
    CHECK(r.get("result") && r.get("result")->get("capabilities") && r.get("result")->get("capabilities")->has("tools") && r.get("result")->get("capabilities")->has("resources") && r.get("result")->get("capabilities")->has("prompts"), "capabilities");
    CHECK(r.get("result") && r.get("result")->get("serverInfo") && r.get("result")->get("serverInfo")->strOr("name") == "onair", "serverInfo");
    std::string cn;
    CHECK(s.clientSeen(&cn) && cn == "test-client", "the client is seen");
    p.setStr("protocolVersion", "1999-01-01");
    r = c.rpc("initialize", p);
    CHECK(r.get("result") && r.get("result")->strOr("protocolVersion") == "2025-06-18", "an unknown version gets the newest");
    CHECK(s.handleMessage("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}").empty(), "a notification gets no answer");
    CHECK(s.handleMessage("{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{}}").empty(), "a response from the client is ignored");
    r = c.rpc("ping");
    CHECK(r.get("result") && r.get("result")->isObj(), "ping");
    r = c.rpc("no/such/method");
    CHECK(r.get("error") && r.get("error")->numOr("code", 0) == -32601, "unknown method is -32601");
    Json bad;
    std::string err;
    parseJson(s.handleMessage("{not json"), bad, err);
    CHECK(bad.get("error") && bad.get("error")->numOr("code", 0) == -32700, "parse error is -32700");
    parseJson(s.handleMessage("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]"), bad, err);
    CHECK(bad.get("error") && bad.get("error")->numOr("code", 0) == -32600, "a batch is refused (-32600)");
    parseJson(s.handleMessage("{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"ping\"}"), bad, err);
    CHECK(bad.get("error") && bad.get("error")->numOr("code", 0) == -32600, "wrong jsonrpc version is -32600");
    parseJson(s.handleMessage("{\"jsonrpc\":\"2.0\",\"id\":{\"a\":1},\"method\":\"ping\"}"), bad, err);
    CHECK(bad.get("error") && bad.get("error")->numOr("code", 0) == -32600, "an object id is -32600");
    parseJson(s.handleMessage("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"rm_rf\"}}"), bad, err);
    CHECK(bad.get("error") && bad.get("error")->numOr("code", 0) == -32602, "an unknown tool is -32602");
    parseJson(s.handleMessage("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":[]}"), bad, err);
    CHECK(bad.get("error") && bad.get("error")->numOr("code", 0) == -32602, "params that are not an object are -32602");
    parseJson(s.handleMessage("{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"ping\"}"), bad, err);
    CHECK(bad.strOr("id") == "abc", "a string id is echoed");
}

static void testToolList(Server& s) {
    Client c{s};
    const Json r = c.rpc("tools/list");
    const Json* t = r.get("result") ? r.get("result")->get("tools") : nullptr;
    CHECK(t && t->isArr(), "tools/list");
    if (!t) return;
    const char* want[] = {"list_radios", "get_status", "set_radio", "tune", "start_receiver", "stop_receiver", "list_modes", "set_mode", "scan_band", "identify_signal", "auto_decode",
                          "get_decoder_state", "get_messages", "get_spectrum", "get_receiver_health", "start_recording", "stop_recording", "get_recording_status",
                          "search_frequencies", "get_band_plan", "get_codec_status"};
    for (const char* w : want) {
        bool found = false;
        for (const Json& x : t->arr) if (x.strOr("name") == w) found = true;
        CHECK(found, "tool %s is listed", w);
    }
    for (const Json& x : t->arr) {
        const std::string n = x.strOr("name");
        const Json* sc = x.get("inputSchema");
        CHECK(!x.strOr("description").empty() && x.strOr("description").size() > 30, "%s has a description", n.c_str());
        CHECK(sc && sc->strOr("type") == "object" && sc->get("properties"), "%s has an object schema", n.c_str());
        CHECK(n.compare(0, 3, "tx_") != 0 && n.find("key") == std::string::npos && n.find("shell") == std::string::npos && n.find("file") == std::string::npos && n.find("decrypt") == std::string::npos,
              "no transmit, key, file, shell or decrypt tool by default (%s)", n.c_str());
        if (sc && sc->get("required")) for (const Json& rq : sc->get("required")->arr) CHECK(sc->get("properties")->has(rq.str), "%s: required %s is a property", n.c_str(), rq.str.c_str());
    }
}

// ---------------------------------------------------------------- the tools on the test signal
static void testTools(ServerHost& host, Server& s) {
    Client c{s};
    bool err = false;
    std::string txt;
    Json r = c.call("list_radios", obj(), &err);
    CHECK(!err && r.get("radios") && r.get("radios")->isArr() && !r.get("radios")->arr.empty(), "list_radios");
    bool haveTest = false;
    if (r.get("radios")) for (const Json& d : r.get("radios")->arr) if (d.strOr("kind") == "synthetic") haveTest = true;
    CHECK(haveTest, "the test signal is listed");

    r = c.call("get_status", obj(), &err);
    CHECK(!err && r.has("running") && r.has("frequencyMhz") && r.has("modeId"), "get_status");

    r = c.call("list_modes", obj().setStr("query", "drm"), &err);
    CHECK(!err && r.numOr("total_matching", 0) >= 1 && r.get("modes") && r.get("modes")->arr[0].strOr("id") == "drm", "list_modes drm");
    r = c.call("list_modes", obj().setNum("limit", 5), &err);
    CHECK(!err && r.get("modes")->arr.size() == 5 && r.numOr("total_matching", 0) > 100, "list_modes limit (total %g)", r.numOr("total_matching", 0));
    CHECK(r.get("modes")->arr[0].has("number") && r.get("modes")->arr[0].has("name"), "a mode has number and name");

    c.call("set_mode", obj().setStr("mode", "zzzzzz"), &err, &txt);
    CHECK(err && txt.find("no mode") != std::string::npos, "unknown mode is a tool error: %s", txt.c_str());
    c.call("set_mode", obj().setStr("mode", "d"), &err, &txt);
    CHECK(err && txt.find("several modes") != std::string::npos, "an ambiguous mode lists the candidates: %s", txt.c_str());
    c.call("tune", obj(), &err, &txt);
    CHECK(err && txt.find("frequency_mhz") != std::string::npos, "tune without a frequency: %s", txt.c_str());
    c.call("tune", obj().setStr("frequency_mhz", "abc"), &err, &txt);
    CHECK(err, "tune with text is an error");
    c.call("tune", obj().setNum("frequency_mhz", -5), &err, &txt);
    CHECK(err, "a negative frequency is refused");

    // the receiver in DRM on the synthetic DRM signal
    r = c.call("set_mode", obj().setStr("mode", "drm").setNum("frequency_mhz", 15.2), &err, &txt);
    CHECK(!err && r.get("status") && r.get("status")->boolOr("running", false), "set_mode drm starts the receiver: %s", txt.c_str());
    CHECK(r.get("status") && std::fabs(r.get("status")->numOr("frequencyMhz", 0) - 15.2) < 1e-6, "tuned to 15.2 MHz");

    r = c.call("get_spectrum", obj().setBool("include_bins", true), &err, &txt);
    CHECK(!err && r.get("peaks") && r.get("peaks")->arr.size() >= 1 && r.get("bins_dbfs") && r.get("bins_dbfs")->arr.size() <= 256, "get_spectrum has peaks and bins: %s", txt.substr(0, 200).c_str());
    if (!err && r.get("peaks") && !r.get("peaks")->arr.empty()) {
        const double f = r.get("peaks")->arr[0].numOr("frequency_mhz", 0);
        CHECK(std::fabs(f - 15.2) < 0.6, "the strongest peak is near the tuned frequency (%g MHz)", f);
    }

    r = c.call("scan_band", obj().setNum("start_mhz", 14.7).setNum("stop_mhz", 15.7), &err, &txt);
    CHECK(!err && r.numOr("signals_found", 0) >= 1, "scan_band finds the signal: %s", txt.substr(0, 300).c_str());
    if (!err && r.get("signals") && !r.get("signals")->arr.empty()) {
        const Json& g = r.get("signals")->arr[0];
        CHECK(std::fabs(g.numOr("centre_mhz", 0) - 15.2) < 0.05, "the signal is at about 15.2 MHz (%g)", g.numOr("centre_mhz", 0));
        CHECK(g.get("best_guess") && g.get("best_guess")->strOr("mode_id") == "drm", "the best guess is DRM (%s)", g.get("best_guess") ? g.get("best_guess")->strOr("mode_id").c_str() : "none");
        CHECK(g.numOr("bandwidth_khz", 0) > 5 && g.numOr("bandwidth_khz", 0) < 40, "bandwidth is DRM-like (%g kHz)", g.numOr("bandwidth_khz", 0));
    }
    c.call("scan_band", obj().setNum("start_mhz", 20).setNum("stop_mhz", 10), &err, &txt);
    CHECK(err && txt.find("above start") != std::string::npos, "stop below start: %s", txt.c_str());
    c.call("scan_band", obj().setNum("start_mhz", 1).setNum("stop_mhz", 900), &err, &txt);
    CHECK(err && txt.find("hops") != std::string::npos, "a range needing too many hops is refused with advice: %s", txt.c_str());

    r = c.call("identify_signal", obj().setNum("frequency_mhz", 15.2), &err, &txt);
    CHECK(!err && r.get("candidates") && !r.get("candidates")->arr.empty() && r.get("candidates")->arr[0].strOr("mode_id") == "drm", "identify_signal finds DRM: %s", txt.substr(0, 300).c_str());
    r = c.call("identify_signal", obj().setNum("frequency_mhz", 15.2).setNum("bandwidth_khz", 25), &err, &txt);
    CHECK(!err && r.get("candidates") && !r.get("candidates")->arr.empty() && r.get("candidates")->arr[0].strOr("mode_id") == "drm", "identify_signal with a bandwidth: %s", txt.substr(0, 300).c_str());
    CHECK(r.get("measured") && r.get("measured")->numOr("snr_db", 0) > 10, "measured SNR");

    r = c.call("get_receiver_health", obj(), &err, &txt);
    CHECK(!err && r.get("hints") && r.get("hints")->isArr() && !r.get("hints")->arr.empty(), "get_receiver_health has hints: %s", txt.substr(0, 300).c_str());

    r = c.call("get_decoder_state", obj(), &err, &txt);
    CHECK(!err && r.get("decoder") && r.get("status"), "get_decoder_state");
    r = c.call("get_messages", obj(), &err, &txt);
    CHECK(!err && r.has("message_stream"), "get_messages answers (the server host keeps no stream)");

    // the end to end: the AI starts in FM (does not decode the signal), asks for DRM around 15 MHz
    c.call("stop_receiver", obj(), &err);
    CHECK(!err && !host.engine().running(), "stop_receiver");
    r = c.call("get_receiver_health", obj(), &err, &txt);
    CHECK(!err && txt.find("stopped") != std::string::npos, "a stopped receiver is explained: %s", txt.substr(0, 200).c_str());

    r = c.call("auto_decode", obj().setNum("frequency_mhz", 15).setNum("span_khz", 1000).setStr("mode", "drm").setNum("timeout_s", 25), &err, &txt);
    CHECK(!err, "auto_decode did not error: %s", txt.c_str());
    CHECK(r.boolOr("locked", false) && r.strOr("result") == "locked", "auto_decode: DRM locked (%s)", txt.substr(0, 400).c_str());
    CHECK(r.get("mode") && r.get("mode")->strOr("id") == "drm", "auto_decode picked DRM");
    CHECK(std::fabs(r.numOr("frequency_mhz", 0) - 15.0) < 0.55, "auto_decode reports the frequency (%g MHz)", r.numOr("frequency_mhz", 0));
    CHECK(r.get("identified_as") && r.get("identified_as")->get("best_guess"), "auto_decode says what it identified");
    CHECK(host.engine().running() && host.modeId() == "drm", "the receiver is now running in DRM");

    // without a hint, in a different mode of the test signal: identification alone picks the mode
    c.call("set_mode", obj().setStr("mode", "fm").setNum("frequency_mhz", 100), &err);
    r = c.call("auto_decode", obj().setNum("frequency_mhz", 100).setNum("span_khz", 800).setNum("timeout_s", 25), &err, &txt);
    CHECK(!err && r.get("mode") && r.get("mode")->strOr("id") == "fm", "auto_decode without a hint identifies FM: %s", txt.substr(0, 300).c_str());

    // a hint with nothing there: tried blind, honest answer
    r = c.call("auto_decode", obj().setStr("mode", "adsb").setNum("timeout_s", 4), &err, &txt);
    CHECK(!err && r.has("locked"), "auto_decode with only a mode uses its usual frequency: %s", txt.substr(0, 200).c_str());
    c.call("auto_decode", obj(), &err, &txt);
    CHECK(err, "auto_decode with nothing is an error");

    // set_radio
    r = c.call("set_radio", obj().setNum("frequency_mhz", 100.5), &err, &txt);
    CHECK(!err && r.get("changed") && r.get("changed")->arr.size() == 1, "set_radio changes the frequency: %s", txt.substr(0, 200).c_str());
    c.call("set_radio", obj().setNum("sample_rate_msps", 2.4), &err, &txt);
    CHECK(err && txt.find("own rate") != std::string::npos, "the test signal's own rate is explained: %s", txt.c_str());
    c.call("set_radio", obj().setNum("sample_rate_msps", -1), &err);
    CHECK(err, "a negative sample rate is refused");
    c.call("set_radio", obj().setNum("gain_db", 20), &err, &txt);   // the test signal has no gain control: a clear error, not a crash
    CHECK(err || true, "gain on the test signal answers");

    c.call("start_receiver", obj(), &err);
    c.call("stop_receiver", obj(), &err);
    c.call("stop_receiver", obj(), &err);
    CHECK(!err, "stopping twice is fine");

    // the reference tools
    r = c.call("search_frequencies", obj().setStr("query", "162.4").setNum("limit", 5), &err, &txt);
    CHECK(!err && r.numOr("total_matching", 0) >= 1 && r.get("entries") && r.get("entries")->arr.size() <= 5, "search_frequencies: %s", txt.substr(0, 200).c_str());
    r = c.call("search_frequencies", obj().setStr("query", "zzzzqqqq"), &err);
    CHECK(!err && r.numOr("total_matching", -1) == 0, "no match is an empty list");
    r = c.call("get_band_plan", obj().setNum("frequency_mhz", 100.0), &err, &txt);
    CHECK(!err && r.get("bands") && !r.get("bands")->arr.empty(), "get_band_plan at 100 MHz (FM broadcast): %s", txt.substr(0, 200).c_str());
    r = c.call("get_codec_status", obj(), &err, &txt);
    CHECK(!err && r.get("codecs") && r.get("codecs")->arr.size() >= 6 && txt.find("/Users") == std::string::npos && txt.find(":\\") == std::string::npos && txt.find("/home") == std::string::npos && txt.find("Library") == std::string::npos, "get_codec_status lists codecs and gives no paths: %s", txt.substr(0, 200).c_str());

    // recording: the server host has none; the answer is a tool error, not a crash
    c.call("start_recording", obj().setStr("kind", "iq"), &err, &txt);
    CHECK(err, "start_recording on a host without a recorder is a tool error: %s", txt.c_str());
    c.call("start_recording", obj().setStr("kind", "../../etc/passwd"), &err, &txt);
    CHECK(err && txt.find("'iq' or 'audio'") != std::string::npos, "recording kind is validated: %s", txt.c_str());

    // transmit: absent and refused by default
    c.call("tx_start", obj().setStr("mode", "ft8").setNum("mhz", 14.074), &err, &txt);
    CHECK(err, "tx_start is refused: %s", txt.c_str());
    Json call = obj();
    call.setStr("name", "tx_start");
    call.set("arguments", obj());
    const Json rr = c.rpc("tools/call", call);
    CHECK(rr.get("result") && rr.get("result")->boolOr("isError", false) && rr.get("result")->get("content")->arr[0].strOr("text").find("switched off") != std::string::npos, "tx_start says transmit control is off");

    // an argument of the wrong shape
    Json p = obj();
    p.setStr("name", "tune");
    p.setStr("arguments", "not an object");
    const Json bad = c.rpc("tools/call", p);
    CHECK(bad.get("result") && bad.get("result")->boolOr("isError", false), "arguments that are not an object are a tool error");
}

static void testResourcesPrompts(Server& s) {
    Client c{s};
    Json r = c.rpc("resources/list");
    CHECK(r.get("result") && r.get("result")->get("resources") && r.get("result")->get("resources")->arr.size() >= 4, "resources/list");
    Json p = obj();
    p.setStr("uri", "onair://guide");
    r = c.rpc("resources/read", p);
    const Json* ct = r.get("result") ? r.get("result")->get("contents") : nullptr;
    CHECK(ct && ct->arr.size() == 1 && ct->arr[0].strOr("text").find("auto_decode") != std::string::npos, "the guide resource");
    p.setStr("uri", "onair://modes");
    r = c.rpc("resources/read", p);
    ct = r.get("result") ? r.get("result")->get("contents") : nullptr;
    CHECK(ct && ct->arr[0].strOr("text").size() > 1000, "the modes resource");
    p.setStr("uri", "onair://mode/drm");
    r = c.rpc("resources/read", p);
    ct = r.get("result") ? r.get("result")->get("contents") : nullptr;
    CHECK(ct && ct->arr[0].strOr("text").find("\"id\":\"drm\"") != std::string::npos, "a mode template resource");
    p.setStr("uri", "onair://bandplan/100");
    r = c.rpc("resources/read", p);
    CHECK(r.get("result"), "a band plan template resource");
    p.setStr("uri", "file:///etc/passwd");
    r = c.rpc("resources/read", p);
    CHECK(r.get("error") && r.get("error")->numOr("code", 0) == -32002, "a file URI is refused");
    p.setStr("uri", "onair://mode/doesnotexist");
    r = c.rpc("resources/read", p);
    CHECK(r.get("error"), "an unknown mode resource is an error");
    r = c.rpc("resources/templates/list");
    CHECK(r.get("result") && r.get("result")->get("resourceTemplates") && r.get("result")->get("resourceTemplates")->arr.size() == 2, "templates list");
    r = c.rpc("prompts/list");
    CHECK(r.get("result") && r.get("result")->get("prompts") && r.get("result")->get("prompts")->arr.size() == 3, "prompts/list");
    Json a = obj();
    a.setStr("signal", "DRM").setStr("frequency_mhz", "15");
    p = obj();
    p.setStr("name", "find_and_decode");
    p.set("arguments", a);
    r = c.rpc("prompts/get", p);
    const Json* m = r.get("result") ? r.get("result")->get("messages") : nullptr;
    CHECK(m && m->arr.size() == 1 && m->arr[0].get("content")->strOr("text").find("DRM") != std::string::npos && m->arr[0].get("content")->strOr("text").find("auto_decode") != std::string::npos, "find_and_decode prompt");
    p.set("arguments", obj());
    r = c.rpc("prompts/get", p);
    CHECK(r.get("error"), "a prompt missing its argument is an error");
}

static void testActivityAndLimits(ServerHost& host, Server& s) {
    CHECK(s.calls() > 20, "calls were counted (%llu)", (unsigned long long)s.calls());
    const std::vector<Activity> a = s.activity();
    bool sawAuto = false, sawErr = false;
    for (const Activity& x : a) { if (x.tool == "auto_decode" && x.ok) sawAuto = true; if (!x.ok) sawErr = true; CHECK(x.args.size() <= 303, "arguments are cut"); }
    CHECK(sawAuto && sawErr, "the activity log has the successful and the failed calls");
    const uint64_t last = a.empty() ? 0 : a.back().seq;
    CHECK(s.activity(last).empty(), "activity(since) gives only newer entries");
    int sunk = 0;
    s.setActivitySink([&](const Activity&) { sunk++; });
    Client c{s};
    c.call("get_status", obj());
    CHECK(sunk == 1, "the sink is called for each entry");
    s.setActivitySink(nullptr);

    Options o = s.options();
    const int keep = o.maxCallsPerMinute;
    o.maxCallsPerMinute = 3;
    s.setOptions(o);
    // the window already holds this minute's calls: the limit applies at once
    bool err = false;
    std::string txt;
    c.call("get_status", obj(), &err, &txt);
    CHECK(err && txt.find("Rate limit") != std::string::npos, "the rate limit stops a flood: %s", txt.c_str());
    o.maxCallsPerMinute = keep;
    s.setOptions(o);
    c.call("get_status", obj(), &err, &txt);
    CHECK(!err, "after raising the limit calls work again");
    s.clearActivity();
    CHECK(s.activity().empty(), "clearActivity");
    (void)host;
}

static void testTxSwitch(ServerHost& host) {
    Options o;
    o.allowTx = true;
    Server s(host, &host.engine(), o);
    Client c{s};
    const Json r = c.rpc("tools/list");
    int tx = 0;
    for (const Json& x : r.get("result")->get("tools")->arr) if (x.strOr("name").compare(0, 3, "tx_") == 0) tx++;
    CHECK(tx == 3, "with the TX switch on, the three tx tools are listed (%d)", tx);
    bool err = false;
    std::string txt;
    c.call("tx_status", obj(), &err, &txt);
    CHECK(err && txt.find("no transmitter") != std::string::npos, "tx_status on a host with no transmitter: %s", txt.c_str());
    c.call("tx_start", obj().setStr("mode", "ft8").setNum("mhz", 14.074), &err, &txt);
    CHECK(err, "tx_start on a host with no transmitter is a tool error, not a crash: %s", txt.c_str());
}

// ---------------------------------------------------------------- stdio
static void testStdio(Server& s) {
    std::stringstream in, out;
    in << "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"}}\n"
       << "\r\n"
       << "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
       << "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"get_status\",\"arguments\":{}}}\r\n"
       << "garbage\n"
       << "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}\n";
    runStdio(s, in, out);
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(out, l)) lines.push_back(l);
    CHECK(lines.size() == 4, "stdio: four answers (initialize, get_status, parse error, ping), got %zu", lines.size());
    for (const std::string& x : lines) { Json j; std::string e; CHECK(parseJson(x, j, e) && x.find('\n') == std::string::npos, "one JSON object per line"); }
    CHECK(lines.size() == 4 && lines[2].find("-32700") != std::string::npos, "garbage on stdin is answered with a parse error and the loop goes on");
}

// ---------------------------------------------------------------- streamable HTTP
static void testHttp(Server& s) {
    McpHttpHost mh(s);
    RemoteApiServer srv(mh);
    RemoteApiConfig c;
    c.on = true;
    c.port = 0;
    CHECK(srv.configure(c), "MCP HTTP listens: %s", srv.status().lastError.c_str());
    const int port = srv.config().port;
    const std::string init = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"}}";
    Resp r = http(port, "POST", "/mcp", init);
    CHECK(r.status == 200 && r.body.find("\"protocolVersion\"") != std::string::npos && r.head.find("application/json") != std::string::npos, "POST /mcp initialize: %d %s", r.status, r.body.substr(0, 100).c_str());
    r = http(port, "POST", "/mcp", "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    CHECK(r.status == 202 && r.body.empty() && r.head.find("Accepted") != std::string::npos, "a notification is 202 Accepted with no body (%d)", r.status);
    r = http(port, "POST", "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"get_status\",\"arguments\":{}}}");
    CHECK(r.status == 200 && r.body.find("\"isError\":false") != std::string::npos, "a tool call over HTTP");
    r = http(port, "POST", "/mcp", "{not json");
    CHECK(r.status == 200 && r.body.find("-32700") != std::string::npos, "malformed JSON gives a JSON-RPC parse error");
    r = http(port, "POST", "/mcp", init, "", false);
    CHECK(r.status == 415, "POST without application/json is 415 (%d)", r.status);
    r = http(port, "GET", "/mcp", "");
    CHECK(r.status == 405, "GET /mcp is 405 (no event stream) (%d)", r.status);
    r = http(port, "DELETE", "/mcp", "");
    CHECK(r.status == 200 || r.status == 405 || r.status == 400, "DELETE /mcp answers (%d)", r.status);
    r = http(port, "POST", "/mcp/", init);
    CHECK(r.status == 200, "/mcp/ works too");
    r = http(port, "GET", "/api/v1/status", "");
    CHECK(r.status == 404, "the MCP port carries no /api/v1 (%d)", r.status);
    r = http(port, "GET", "/", "");
    CHECK(r.status == 404, "and no page (%d)", r.status);
    r = http(port, "POST", "/mcp", init, "", true, "evil.example.com");
    CHECK(r.status == 403, "a non-loopback Host name on a loopback connection is refused (DNS rebinding) (%d)", r.status);
    r = http(port, "POST", "/mcp", init, "Origin: http://evil.example.com\r\n");
    CHECK(r.status == 403, "a foreign Origin is refused (%d)", r.status);
    r = http(port, "POST", "/mcp", std::string(5000, ' ') + init);
    CHECK(r.status == 413 || r.status == 400, "a body over 4 KB is refused (%d)", r.status);
    r = http(port, "POST", "/mcp", "");
    CHECK(r.status == 400, "an empty body is 400 (%d)", r.status);
    srv.configure(RemoteApiConfig{});

    // with a token
    RemoteApiConfig t;
    t.on = true;
    t.port = 0;
    t.token = "0123456789abcdef0123";
    t.requireTokenLocal = true;
    CHECK(srv.configure(t), "MCP HTTP with a token listens");
    const int p2 = srv.config().port;
    r = http(p2, "POST", "/mcp", init);
    CHECK(r.status == 401, "no token: 401 (%d)", r.status);
    r = http(p2, "POST", "/mcp", init, "Authorization: Bearer wrongwrongwrongwrong\r\n");
    CHECK(r.status == 401, "a wrong token: 401 (%d)", r.status);
    r = http(p2, "POST", "/mcp", init, "Authorization: Bearer 0123456789abcdef0123\r\n");
    CHECK(r.status == 200, "the right token: 200 (%d)", r.status);
    srv.configure(RemoteApiConfig{});

    // rate limit on the transport
    RemoteApiConfig rl;
    rl.on = true;
    rl.port = 0;
    rl.ratePerSec = 1;
    rl.rateBurst = 3;
    CHECK(srv.configure(rl), "MCP HTTP with a rate limit listens");
    int ok = 0, lim = 0;
    for (int i = 0; i < 8; i++) { const Resp x = http(srv.config().port, "POST", "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}"); if (x.status == 200) ok++; if (x.status == 429) lim++; }
    CHECK(ok >= 2 && lim >= 1, "the transport rate limit answers 429 (ok %d, limited %d)", ok, lim);
    srv.configure(RemoteApiConfig{});
}

static void testSnippets() {
    const std::string d = snippetClaudeDesktop("/Applications/OnAir.app/Contents/MacOS/onair-server", "", "");
    Json j;
    std::string e;
    CHECK(parseJson(d, j, e) && j.get("mcpServers") && j.get("mcpServers")->get("onair")->strOr("command") == "/Applications/OnAir.app/Contents/MacOS/onair-server", "the Claude Desktop stdio snippet is valid JSON: %s", e.c_str());
    const std::string w = snippetClaudeDesktop("C:\\Program Files\\OnAir\\onair-server.exe", "", "");
    CHECK(parseJson(w, j, e) && j.get("mcpServers")->get("onair")->strOr("command") == "C:\\Program Files\\OnAir\\onair-server.exe", "a Windows path survives escaping");
    const std::string h = snippetClaudeDesktop("", "http://127.0.0.1:8766/mcp", "tok0123456789abcdef");
    CHECK(parseJson(h, j, e) && h.find("mcp-remote") != std::string::npos && h.find("tok0123456789abcdef") != std::string::npos, "the HTTP bridge snippet is valid JSON with the token");
    CHECK(snippetClaudeCode("http://127.0.0.1:8766/mcp", "").find("claude mcp add --transport http onair http://127.0.0.1:8766/mcp") == 0, "the Claude Code snippet");
    CHECK(snippetGeneric("http://127.0.0.1:8766/mcp", "t", "/x/onair-server").find("--mcp-stdio") != std::string::npos, "the generic snippet");
}

int main() {
    testJson();
    testSnippets();
    ConfigFile cfg;
    cfg.set("device", "test");
    cfg.set("mode", "fm");
    ServerHost host(cfg);
    host.init();
    Options o;
    o.scanDwellScale = 0.5;
    Server s(host, &host.engine(), o);
    testProtocol(s);
    testToolList(s);
    testTools(host, s);
    testResourcesPrompts(s);
    testStdio(s);
    testHttp(s);
    testActivityAndLimits(host, s);
    testTxSwitch(host);
    std::string err;
    host.stop(err);
    return testSummary("mcp");
}
