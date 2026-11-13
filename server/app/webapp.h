// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web 3.9: the whole receiver in a browser (docs/WEB-3.9.md). Mounted under /app on the web server (web_server.h AppHandler):
//   /app/                 the page (web/onair-app: vanilla JS, no build step)
//   /app/v1/...           the JSON API (status, modes, schema, panel, devices, settings, login) - docs/WEB-3.9.md lists it
//   /app/v1/ws            one WebSocket per viewer: status, the mode's panel, spectrum rows and audio
// It drives ServerHost (the headless server's receiver: every mode, the same engine as the desktop app) and never touches the disk itself:
// settings go through ServerHost::editConfig, which writes the settings file.
#pragma once
#include "server_host.h"
#include "web/web_server.h"
#include "webapp_auth.h"
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace onair_app {

struct AppOptions {
    std::string root;        // the page's folder (index.html, app.js, ...); empty: a placeholder page
    std::string name;        // shown in the page (web_name)
    std::string version;
    bool tlsAvailable = false;
    std::function<bool(const std::string& modeId)> publicModeAllowed;   // viewers see this mode's results (the hub's policy); empty: every mode
};

// One setting of the Settings page: key in the settings file, group, label, help, kind, default; restart: takes effect at the next start
struct SettingDef { const char* key, *group, *label, *help, *kind, *def; bool restart; };
const std::vector<SettingDef>& settingDefs();

class WebApp : public onair_web::AppHandler {
public:
    WebApp(onair_server::ServerHost& host, onair_server::ConfigFile& cfg, AppOptions o);
    bool handle(const onair_web::AppRequest& r, onair_web::AppReply& out) override;
    bool isWebSocketPath(const std::string& path) const override { return path == "/app/v1/ws"; }
    void webSocket(onair_web::AppWs& ws) override;
    Sessions& sessions() { return sessions_; }
    void setPublicModeAllowed(std::function<bool(const std::string& modeId)> f) { std::lock_guard<std::mutex> g(mu_); opt_.publicModeAllowed = std::move(f); }
    int viewers() const { return viewers_.load(); }
    int connections() const { return conns_.load(); }
    void tick(double now);   // once a second: session expiry
    static AccessConfig accessFrom(const onair_server::ConfigFile& c);
    // The settings page's model: every SettingDef with its current value, as JSON
    std::string settingsJson();
    // Applies a {key: value} object: validates, writes the settings file, applies the live ones; false with why
    bool applySettings(const std::map<std::string, dect2::JsonVal>& body, std::string& why, std::vector<std::string>& changed);

private:
    onair_server::ServerHost& host_;
    onair_server::ConfigFile& cfg_;
    AppOptions opt_;
    Sessions sessions_;
    std::atomic<int> viewers_{0}, conns_{0};
    std::mutex mu_;   // cfg_ reads, opt_

    Who who(const onair_web::AppRequest& r);
    bool api(const onair_web::AppRequest& r, onair_web::AppReply& out);
    bool serveStatic(const onair_web::AppRequest& r, onair_web::AppReply& out);
    std::string infoJson(const Who& w, bool tls);
    std::string statusJson();
    bool control(const std::string& what, const std::map<std::string, dect2::JsonVal>& body, std::string& json, int& status);
    bool viewerMayWatch(std::string& why);
    static void json(onair_web::AppReply& out, int status, const std::string& body) { out.status = status; out.body = body; out.type = "application/json"; }
    static void err(onair_web::AppReply& out, int status, const std::string& msg);
};

std::string jsonStr(const std::string& s);   // with quotes

} // namespace onair_app
