#include "web_config.h"
#include "server_config.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace onair_web {

static std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> v;
    std::stringstream ss(s);
    std::string x;
    while (std::getline(ss, x, ',')) {
        size_t a = 0, b = x.size();
        while (a < b && x[a] == ' ') a++;
        while (b > a && x[b - 1] == ' ') b--;
        v.push_back(x.substr(a, b - a));
    }
    return v;
}

WebSettings loadWebSettings(onair_server::ConfigFile& c, bool& madeAdminToken) {
    madeAdminToken = false;
    WebSettings w;
    w.on = c.flag("web", false);
    WebServerConfig& s = w.server;
    s.on = w.on;
    s.port = (int)c.num("web_port", 8073);
    s.bind = c.get("web_bind", c.flag("web_lan", false) ? "0.0.0.0" : "127.0.0.1");
    s.tlsCert = c.get("web_tls_cert", c.get("tls_cert"));
    s.tlsKey = c.get("web_tls_key", c.get("tls_key"));
    s.plainInternet = c.flag("web_plain_internet", false);
    s.trustProxy = c.flag("web_trust_proxy", false);
    s.webRoot = c.get("web_root");
    for (const auto& o : c.getAll("web_origin")) if (!o.empty()) s.origins.push_back(o);
    if (!authModeFromName(c.get("web_auth", "open"), s.auth.mode)) w.warnings.push_back("web_auth must be open, password or invite (using open)");
    s.auth.password = c.get("web_password");
    for (const auto& line : c.getAll("web_invite")) {   // token,label,max_minutes
        if (line.empty()) continue;
        const auto f = splitComma(line);
        if (f.empty() || f[0].size() < 16) { w.warnings.push_back("a web_invite token is shorter than 16 characters and is ignored"); continue; }
        InviteToken t;
        t.token = f[0];
        if (f.size() > 1) t.label = f[1];
        if (f.size() > 2) t.maxMinutes = std::max(0, atoi(f[2].c_str()));
        s.auth.invites.push_back(t);
    }
    s.auth.adminToken = c.get("web_admin_token");
    if (w.on && s.auth.adminToken.size() < 24) {
        s.auth.adminToken = newToken(16);   // 32 hex characters
        c.set("web_admin_token", s.auth.adminToken);
        madeAdminToken = true;
    }
    s.adminLan = c.flag("web_admin_lan", false);
    HubLimits& L = w.limits;
    auto clampi = [&](const char* k, int def, int lo, int hi) { return std::clamp((int)c.num(k, def), lo, hi); };
    L.maxUsers = clampi("web_max_users", L.maxUsers, 1, 1000);
    L.maxPerIp = clampi("web_max_per_ip", L.maxPerIp, 1, 100);
    L.cpuCapPct = std::clamp(c.num("web_cpu_cap_pct", L.cpuCapPct), 5.0, 100.0);
    L.sessionCpuPct = std::clamp(c.num("web_session_cpu_pct", L.sessionCpuPct), 1.0, 100.0);
    L.maxKbpsTotal = clampi("web_max_kbps", L.maxKbpsTotal, 64, 10000000);
    L.maxKbpsUser = clampi("web_user_kbps", L.maxKbpsUser, 32, 100000);
    L.maxDecoders = clampi("web_max_decoders", L.maxDecoders, 0, 256);
    L.idleMin = clampi("web_idle_min", L.idleMin, 0, 100000);
    L.publicMaxMin = clampi("web_public_max_min", L.publicMaxMin, 0, 100000);
    L.memberMaxMin = clampi("web_member_max_min", L.memberMaxMin, 0, 100000);
    L.maxFps = clampi("web_max_fps", L.maxFps, 1, 30);
    L.maxBins = clampi("web_max_bins", L.maxBins, 64, 4096);
    L.spectrum = c.flag("web_spectrum", true);
    L.threads = clampi("web_threads", L.threads, 0, 64);
    s.maxConnections = std::max(16, L.maxUsers * 3 + 8);
    w.logIp = c.flag("web_log_ip", false);
    w.name = c.get("web_name", "OnAir Web");
    w.location = c.get("web_location");
    w.antenna = c.get("web_antenna");
    w.directory.on = c.flag("web_directory", false);
    w.directory.url = c.get("web_directory_url");
    w.directory.publicUrl = c.get("web_public_url");
    w.directory.intervalSec = clampi("web_directory_interval", 300, 60, 3600);
    w.directory.keyFile = c.get("web_directory_key");
    w.directory.loc = c.get("web_directory_loc");
    w.directory.locDecimals = clampi("web_directory_loc_decimals", 2, 0, 5);
    w.directory.freq = c.get("web_directory_freq");
    w.directory.radio = c.get("web_directory_radio");
    w.directory.contact = c.get("web_directory_contact");
    w.directory.desc = c.get("web_directory_desc");
    w.directory.reach = c.flag("web_directory_reach", false);
    w.directory.ack = c.get("web_directory_ack");
    const bool network = s.bind != "127.0.0.1";
    if (network && s.tlsCert.empty()) {
        w.warnings.push_back(s.plainInternet ? "web: plain HTTP for everyone, also from the internet (web_plain_internet=1): passwords and invites travel unencrypted"
                                             : "web: no TLS, so only private (LAN) addresses are let in; set web_tls_cert / web_tls_key (or a reverse proxy) for the internet");
    }
    if (s.auth.mode == AuthMode::Password && s.auth.password.size() < 8) w.warnings.push_back("web_password must have 8 characters or more");
    if (s.auth.mode == AuthMode::Invite && s.auth.invites.empty()) w.warnings.push_back("web_auth=invite but there is no web_invite yet: make one with the admin API");
    return w;
}

void saveWebState(onair_server::ConfigFile& c, const WebPolicy& p, const std::vector<InviteToken>& invites) {
    p.save(c);
    // the invites replace the old web_invite lines (one line each)
    std::string first;
    std::vector<std::string> lines;
    for (const auto& t : invites) {
        std::string label = t.label;
        label.erase(std::remove(label.begin(), label.end(), ','), label.end());
        label.erase(std::remove(label.begin(), label.end(), '\n'), label.end());
        lines.push_back(t.token + "," + label + "," + std::to_string(t.maxMinutes));
    }
    c.set("web_invite", lines.empty() ? std::string() : lines[0]);
    for (size_t i = 1; i < lines.size(); i++) {
        // ConfigFile::set keeps one line per key: further invites are appended as extra lines through parse()
        std::string t = c.text();
        if (!t.empty() && t.back() != '\n') t += '\n';
        c.parse(t + "web_invite=" + lines[i] + "\n");
    }
}

} // namespace onair_web
