#include "dect2/goes/emwin_parse.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace dect2 {

namespace {
std::vector<std::string> lines(const std::string& t) { std::vector<std::string> o; std::string s; for (char c : t) { if (c == '\n') { o.push_back(s); s.clear(); } else if (c != '\r') s += c; } if (!s.empty()) o.push_back(s); return o; }
bool isWmo(const std::string& l) {
    // TTAAii CCCC YYGGgg, optionally BBB
    if (l.size() < 18 || !std::isalpha((unsigned char)l[0]) || !std::isalpha((unsigned char)l[1])) return false;
    return std::isalnum((unsigned char)l[2]) && std::isalnum((unsigned char)l[3]) && std::isdigit((unsigned char)l[4]) && std::isdigit((unsigned char)l[5]) && l[6] == ' ' && std::isalpha((unsigned char)l[7]) && l[11] == ' ' && std::isdigit((unsigned char)l[12]) && std::isdigit((unsigned char)l[13]);
}
void addUgc(std::vector<std::string>& out, const std::string& line) {
    // SSFNNN>NNN-NNN-DDHHMM- with the state and F/Z carried over
    std::string state; char kind = 0;
    size_t i = 0;
    while (i < line.size()) {
        size_t j = line.find('-', i);
        const std::string tok = line.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? line.size() : j + 1;
        if (tok.empty()) continue;
        if (tok.size() == 6 && std::isdigit((unsigned char)tok[0])) continue;   // the expiry time at the end
        std::string body = tok;
        if (body.size() >= 3 && std::isalpha((unsigned char)body[0]) && std::isalpha((unsigned char)body[1]) && (body[2] == 'Z' || body[2] == 'C')) { state = body.substr(0, 2); kind = body[2]; body = body.substr(3); }
        if (!kind) continue;
        size_t gt = body.find('>');
        if (gt == std::string::npos) { if (body.size() == 3) out.push_back(state + kind + body); }
        else { int a = atoi(body.substr(0, gt).c_str()), b = atoi(body.substr(gt + 1).c_str()); for (int n = a; n <= b && n - a < 300; n++) { char s[10]; snprintf(s, sizeof s, "%s%c%03d", state.c_str(), kind, n); out.push_back(s); } }
    }
}
}  // namespace

EmwinBulletin emwinParse(const std::string& text) {
    EmwinBulletin b;
    auto ls = lines(text);
    size_t i = 0;
    while (i < ls.size() && !isWmo(ls[i])) i++;
    if (i >= ls.size()) return b;
    b.ok = true; b.wmoHeader = ls[i]; b.station = ls[i].substr(7, 4); i++;
    if (i < ls.size()) { std::string a = ls[i]; while (!a.empty() && a.back() == ' ') a.pop_back(); if (a.size() >= 4 && a.size() <= 6 && a.find(' ') == std::string::npos) { b.awips = a; b.productCategory = a.substr(0, 3); i++; } }
    for (; i < ls.size(); i++) {
        const std::string& l = ls[i];
        if (l.empty()) continue;
        if (b.headline.empty() && l.size() >= 6 && l.size() < 60 && std::all_of(l.begin(), l.end(), [](char c) { return !std::islower((unsigned char)c); }) && l.find('-') == std::string::npos && l.find('/') == std::string::npos && l.find("LAT") == std::string::npos) b.headline = l;
        if (l.size() > 3 && l[0] == '/') {
            size_t p = 0;
            while ((p = l.find('/', p)) != std::string::npos) {
                size_t q = l.find('/', p + 1);
                if (q == std::string::npos) break;
                const std::string v = l.substr(p + 1, q - p - 1);
                if (v.size() >= 40 && v[0] != ' ' && v[1] == '.' && v.find('.', 2) != std::string::npos) {
                    EmwinVtec e;
                    std::stringstream ss(v); std::string f[7]; for (int k = 0; k < 7; k++) std::getline(ss, f[k], '.');
                    e.action = f[1]; e.office = f[2]; e.phenomenon = f[3]; e.significance = f[4]; e.eventNo = atoi(f[5].c_str());
                    size_t dash = f[6].find('-'); if (dash != std::string::npos) { e.begin = f[6].substr(0, dash); e.end = f[6].substr(dash + 1); }
                    b.vtec.push_back(e);
                }
                p = q;
            }
        }
        if (l.size() > 8 && std::isalpha((unsigned char)l[0]) && std::isalpha((unsigned char)l[1]) && (l[2] == 'Z' || l[2] == 'C') && std::isdigit((unsigned char)l[3]) && l.find('-') != std::string::npos) addUgc(b.ugc, l);
        if (l.rfind("LAT...LON", 0) == 0) {
            std::vector<int> nums; std::string acc = l.substr(9);
            for (size_t j = i + 1; j < ls.size() && !ls[j].empty() && ls[j][0] == ' '; j++) acc += " " + ls[j];
            std::stringstream ss(acc); std::string tok; while (ss >> tok) { if (std::all_of(tok.begin(), tok.end(), [](char c) { return std::isdigit((unsigned char)c); })) nums.push_back(atoi(tok.c_str())); }
            for (size_t n = 0; n + 1 < nums.size(); n += 2) b.polygon.emplace_back(nums[n] / 100.0, -nums[n + 1] / 100.0);
        }
    }
    if (!b.vtec.empty()) { const std::string& s = b.vtec[0].significance; b.severity = s == "W" ? 3 : s == "A" ? 2 : 1; }
    else if (b.productCategory == "TOR" || b.productCategory == "SVR" || b.productCategory == "FFW") b.severity = 3;
    return b;
}

}  // namespace dect2
