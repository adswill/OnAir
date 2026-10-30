#include "dect2/results/sessions_json.h"
#include "dect2/session/session_store.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dect2 {

namespace {
std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += (char)c;
    }
    return o;
}
std::string param(const std::string& q, const std::string& key) {
    size_t i = 0;
    while (i <= q.size()) {
        size_t e = q.find('&', i);
        if (e == std::string::npos) e = q.size();
        const std::string kv = q.substr(i, e - i);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.compare(0, eq, key) == 0) {
            std::string v = kv.substr(eq + 1), o;
            for (size_t k = 0; k < v.size(); k++) {
                if (v[k] == '+') o += ' ';
                else if (v[k] == '%' && k + 2 < v.size()) { o += (char)strtol(v.substr(k + 1, 2).c_str(), nullptr, 16); k += 2; }
                else o += v[k];
            }
            return o;
        }
        i = e + 1;
    }
    return std::string();
}
bool number(const std::string& s, int64_t& out, int64_t lo, int64_t hi) {
    if (s.empty() || s.size() > 18 || s.find_first_not_of("0123456789") != std::string::npos) return false;
    out = std::min(hi, std::max(lo, (int64_t)strtoll(s.c_str(), nullptr, 10)));
    return true;
}
}  // namespace

std::string sessionStatusJson() {
    sess::SessionStore& st = sess::global();
    return "{\"gen\":" + std::to_string(st.genAll()) + ",\"dropped\":" + std::to_string(st.dropped()) + ",\"logging\":" + (st.loggingDefault() ? "true" : "false") + "}";
}

std::string sessionsJson(const std::string& query) {
    using namespace sess;
    Query q;
    int64_t v = 0;
    const char* names[] = {"mode", "kind", "offset", "limit", "since"};
    for (const char* n : names) {
        const std::string s = param(query, n);
        if (!s.empty() && !number(s, v, 0, INT64_MAX / 2)) return std::string("{\"error\":\"") + n + " must be a number\"}";
    }
    if (number(param(query, "mode"), v, 0, 65535)) q.mode = (int32_t)v;
    if (number(param(query, "kind"), v, 1, 8)) q.kind = (int)v;
    if (number(param(query, "since"), v, 0, INT64_MAX / 2)) q.since = v;
    q.offset = number(param(query, "offset"), v, 0, 10000000) ? (size_t)v : 0;
    q.limit = number(param(query, "limit"), v, 1, 200) ? (size_t)v : 50;
    q.text = param(query, "q");
    if (q.text.size() > 200) q.text.resize(200);
    q.activeOnly = param(query, "active") == "1";
    const Page pg = global().query(q);
    std::string o = "{\"gen\":" + std::to_string(pg.gen) + ",\"total\":" + std::to_string(pg.total) + ",\"offset\":" + std::to_string(q.offset) + ",\"limit\":" + std::to_string(q.limit) + ",\"sessions\":[";
    bool first = true;
    for (const Session& s : pg.rows) {
        if (!first) o += ',';
        first = false;
        o += "{\"id\":\"" + std::to_string(s.id) + "\",\"mode\":" + std::to_string(s.mode) + ",\"modeName\":\"" + jesc(global().modeName(s.mode)) + "\",\"kind\":\"" + kindName((Kind)s.kind) + "\",\"key\":\"" + jesc(s.key) +
             "\",\"title\":\"" + jesc(s.title) + "\",\"start\":" + std::to_string(s.startWall) + ",\"end\":" + std::to_string(s.endWall) + ",\"durationMs\":" + std::to_string(s.durationMs) +
             ",\"freqHz\":" + std::to_string(s.freqHz) + ",\"verdict\":\"" + verdictName((Verdict)s.verdict) + "\",\"events\":" + std::to_string(s.eventCount) + ",\"active\":" + (s.active() ? "true" : "false") +
             ",\"emergency\":" + ((s.flags & FlagEmergency) ? "true" : "false") + "}";
    }
    return o + "]}";
}

}  // namespace dect2
