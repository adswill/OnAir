// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MCP (Model Context Protocol) server: lets an AI assistant that speaks MCP (Claude Desktop, Claude Code, any MCP client, a local LLM behind one)
// drive OnAir: list radios, tune, scan a band, identify signals, start a decoder, read what it decoded. A thin layer over the remote-control host
// (core/include/dect2/net/remote_api.h) and the engine's signal identifier, so it works in the app and in onair-server alike.
//
// Transports: stdio (newline-delimited JSON-RPC 2.0 on stdin / stdout, for a client that starts the program) and streamable HTTP (POST /mcp on a
// RemoteApiServer whose host is an McpHttpHost: localhost by default, optional token). docs/MCP.md has the setup, the tool list and the safety rules.
//
// Safety: no transmit tool exists unless Options::allowTx is on (a separate switch, off by default); there is no tool for the file system, a shell,
// keys, credentials or settings; calls are rate limited; every call is written to the activity log that the app shows the user.
#pragma once
#include "dect2/net/remote_api.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
class Engine;

namespace mcp {

// ---- a small JSON value (full JSON, unlike parseFlatJson)
struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj };
    Kind kind = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;   // keys in order

    Json() = default;
    static Json null() { return Json(); }
    static Json boolean(bool v) { Json j; j.kind = Bool; j.b = v; return j; }
    static Json number(double v) { Json j; j.kind = Num; j.num = v; return j; }
    static Json string(std::string v) { Json j; j.kind = Str; j.str = std::move(v); return j; }
    static Json array() { Json j; j.kind = Arr; return j; }
    static Json object() { Json j; j.kind = Obj; return j; }

    bool isNull() const { return kind == Null; }
    bool isObj() const { return kind == Obj; }
    bool isArr() const { return kind == Arr; }
    bool isStr() const { return kind == Str; }
    bool isNum() const { return kind == Num; }
    bool isBool() const { return kind == Bool; }
    const Json* get(const std::string& key) const;       // nullptr when not an object or no such key
    Json& set(const std::string& key, Json v);           // object: replace or append
    Json& push(Json v) { arr.push_back(std::move(v)); return *this; }
    double numOr(const std::string& key, double d) const;
    std::string strOr(const std::string& key, const std::string& d = std::string()) const;
    bool boolOr(const std::string& key, bool d) const;
    bool has(const std::string& key) const { return get(key) != nullptr; }
    std::string dump() const;                              // compact JSON
    // Convenience builders
    Json& setNum(const std::string& k, double v) { return set(k, number(v)); }
    Json& setStr(const std::string& k, const std::string& v) { return set(k, string(v)); }
    Json& setBool(const std::string& k, bool v) { return set(k, boolean(v)); }
};

// Parses one JSON value (depth at most 32, text at most 4 MB). false: malformed, `err` says where.
bool parseJson(const std::string& text, Json& out, std::string& err);

// ---- the server
struct Options {
    bool allowTx = false;                // the transmit tools exist only when the user turned this on (a separate switch, off by default)
    int maxCallsPerMinute = 120;         // tool calls; beyond that the call fails with a rate-limit error
    std::string serverName = "onair";
    std::string serverVersion = "3.6";
    size_t maxResultBytes = 48 * 1024;   // a tool result is cut to this (with a note) so a small local model is not flooded
    double scanDwellScale = 1.0;         // tests shorten the capture time; 1 = the identifier's own
    int decodeWaitSec = 30;              // auto_decode: how long to wait for a lock (a call may ask for less or more, up to 120)
};

struct Activity {
    double time = 0;                     // seconds since the epoch
    std::string tool;                    // tool name, or "resources/read", "prompts/get"
    std::string args;                    // the arguments, compact JSON, cut to 300 characters
    std::string outcome;                 // one line: "ok", or the error
    bool ok = true;
    uint64_t seq = 0;
};

class Server {
public:
    // host: the control surface (statusJson, setFrequencyHz, ...); engine: for the signal identifier (nullptr: scan / identify tools say they are not available)
    Server(RemoteApiHost& host, Engine* engine, Options o = Options());
    ~Server();

    void setOptions(const Options& o);
    Options options() const;

    // One JSON-RPC message (a request, a notification or a response from the client) in, the answer out. Empty: nothing to send back (a notification).
    // Never throws. Thread safe: tool calls run one at a time.
    std::string handleMessage(const std::string& text);

    // The tools as MCP lists them (name, description, inputSchema), for tests and docs
    Json toolsList() const;
    // Calls a tool directly (tests, the GUI's "try it"): the same path as tools/call
    Json callTool(const std::string& name, const Json& args);

    // Activity log (newest last), what the app shows the user. since: only entries with seq > since.
    std::vector<Activity> activity(uint64_t since = 0) const;
    uint64_t calls() const;
    void setActivitySink(std::function<void(const Activity&)> f);   // called for every entry (stderr line in the server, the window in the app)
    void clearActivity();

    // Is a client connected (an initialize was seen in the last minutes)? For the app's status line.
    bool clientSeen(std::string* name = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// stdio transport: reads newline-delimited JSON-RPC from `in`, writes the answers (one line each) to `out`; returns at end of input.
void runStdio(Server& s, std::istream& in, std::ostream& out);

// A RemoteApiHost that carries only /mcp (RemoteApiServer::configure with port, optional token): the streamable HTTP transport.
class McpHttpHost : public RemoteApiHost {
public:
    explicit McpHttpHost(Server& s) : s_(s) {}
    std::string statusJson() override { return "{}"; }
    std::string devicesJson() override { return "[]"; }
    std::string modesJson() override { return "[]"; }
    std::string telemetryJson(uint64_t& seq) override { seq = 0; return "{}"; }
    std::string spectrumJson(int, uint64_t& seq) override { seq = 0; return "{}"; }
    bool start(int, std::string& e) override { e = "not supported"; return false; }
    bool stop(std::string& e) override { e = "not supported"; return false; }
    bool setMode(int, const std::string&, std::string& e) override { e = "not supported"; return false; }
    bool setFrequencyHz(double, std::string& e) override { e = "not supported"; return false; }
    bool setSampleRateHz(double, std::string& e) override { e = "not supported"; return false; }
    bool setGainDb(double, std::string& e) override { e = "not supported"; return false; }
    int handleMcp(const std::string& method, const std::string& body, std::string& out) override;
    bool mcpOnly() const override { return true; }
private:
    Server& s_;
};

// The connection snippets the app and the docs show: Claude Desktop (stdio and HTTP via mcp-remote), Claude Code, and a generic client.
// exePath: the program that speaks MCP on stdio (onair-server --mcp-stdio); url: the HTTP endpoint; token: may be empty.
// exePath non-empty: Claude Desktop starts that program on stdio; else it bridges to the HTTP endpoint with the mcp-remote helper (Claude Desktop speaks stdio only).
std::string snippetClaudeDesktop(const std::string& exePath, const std::string& url, const std::string& token);
std::string snippetClaudeCode(const std::string& url, const std::string& token);
std::string snippetGeneric(const std::string& url, const std::string& token, const std::string& exePath);

} // namespace mcp
} // namespace dect2
