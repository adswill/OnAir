// The optional public name on the map ("Your name on the map"). Pure, so it can be tested. The same rules as web/map/src/by-name.js;
// the server checks again and never trusts this. Empty = anonymous. Never filled in by the app: only what the user typed.
#pragma once
#include <string>

namespace mapname {

// 0 = fine (name holds the trimmed result, "" for none), 1 = length, 2 = characters, 3 = web or e-mail address, 4 = reserved or not allowed.
inline int check(const std::string& in, std::string& name) {
    size_t a = 0, b = in.size();
    while (a < b && (in[a] == ' ' || in[a] == '\t' || in[a] == '\n' || in[a] == '\r')) a++;
    while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t' || in[b - 1] == '\n' || in[b - 1] == '\r')) b--;
    std::string s;
    for (size_t i = a; i < b; i++) {
        if (in[i] == ' ' && !s.empty() && s.back() == ' ') continue;
        s += in[i];
    }
    name.clear();
    if (s.empty()) return 0;
    if (s.size() < 2 || s.size() > 24) return 1;
    std::string low, fold;
    for (unsigned char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-' || c == '.';
        if (!ok) return 2;
        char l = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        low += l;
        switch (l) { case '0': l = 'o'; break; case '1': l = 'i'; break; case '3': l = 'e'; break; case '4': l = 'a'; break; case '5': l = 's'; break; default: break; }
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) fold += l;
    }
    if (low.find("http") != std::string::npos || low.find("www") != std::string::npos) return 3;
    for (size_t i = 1; i + 2 < low.size(); i++)   // x.yz : a dot between an alphanumeric and two letters
        if (low[i] == '.' && low[i - 1] != ' ' && low[i - 1] != '.' && low[i + 1] >= 'a' && low[i + 1] <= 'z' && low[i + 2] >= 'a' && low[i + 2] <= 'z') return 3;
    static const char* const kReserved[] = {"admin", "administrator", "onair", "support", "moderator", "staff", "official", "system", "root", "owner", "operator", "server", "anonymous", "webmaster", "mapteam"};
    for (const char* r : kReserved) {
        const std::string rs = r;
        if (fold.compare(0, rs.size(), rs) == 0 && fold.size() <= rs.size() + 2) return 4;
    }
    static const char* const kBanned[] = {"nigger", "nigga", "faggot", "kike", "chink", "tranny", "retard", "nazi", "hitler"};
    for (const char* w : kBanned) if (fold.find(w) != std::string::npos) return 4;
    name = s;
    return 0;
}

// JSON string escaping is done by the caller; a checked name only holds [A-Za-z0-9 _.-].
inline bool valid(const std::string& in) { std::string n; return check(in, n) == 0; }

} // namespace mapname
