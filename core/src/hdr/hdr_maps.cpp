#include "dect2/hdr/hdr_maps.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace dect2 {
namespace hdr {

namespace {
bool stampAt(const std::string& s, size_t pos) {   // YYYYMMDD_HHMM
    if (pos + 13 > s.size() || s[pos + 8] != '_') return false;
    for (size_t i = 0; i < 13; i++) if (i != 8 && !std::isdigit((unsigned char)s[pos + i])) return false;
    return true;
}
}

bool parseTrafficName(const std::string& n, int& x, int& y, std::string& stamp) {
    if (n.rfind("TMT_", 0) != 0) return false;
    // ..._x_y_YYYYMMDD_HHMM
    for (size_t i = 4; i + 18 <= n.size(); i++) {
        if (n[i] == '_' && n[i + 1] >= '1' && n[i + 1] <= '3' && n[i + 2] == '_' && n[i + 3] >= '1' && n[i + 3] <= '3' && n[i + 4] == '_' && stampAt(n, i + 5)) {
            x = n[i + 1] - '1'; y = n[i + 3] - '1'; stamp = n.substr(i + 5, 13); return true;
        }
    }
    return false;
}

bool parseRadarName(const std::string& n, std::string& id, std::string& stamp) {
    if (n.rfind("DWRO_", 0) != 0) return false;
    // DWRO_id_xxx_YYYYMMDD_HHMM...: the last stamp in the name
    for (size_t i = n.size(); i-- > 5;) {
        if (i + 13 <= n.size() && n[i - 1] == '_' && stampAt(n, i)) {
            stamp = n.substr(i, 13);
            const std::string body = n.substr(5, i - 6);      // id_xxx
            const size_t u = body.rfind('_');
            if (u == std::string::npos) return false;
            id = body.substr(0, u);
            return true;
        }
    }
    return false;
}

bool parseRadarInfo(const std::string& t, std::string& id, double c[4]) {
    bool haveId = false, haveC = false;
    std::stringstream ss(t); std::string l;
    while (std::getline(ss, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.rfind("DWR_Area_ID=\"", 0) == 0 && l.size() > 14) { id = l.substr(13, l.size() - 14); haveId = true; }
        if (l.rfind("Coordinates", 0) == 0) {
            const size_t a = l.find('('), b = l.find(')', a), d = b == std::string::npos ? b : l.find('(', b), e = d == std::string::npos ? d : l.find(')', d);
            if (e == std::string::npos) continue;
            auto pair = [](const std::string& s, double& v1, double& v2) { const size_t k = s.find(','); if (k == std::string::npos) return false; v1 = atof(s.substr(0, k).c_str()); v2 = atof(s.substr(k + 1).c_str()); return true; };
            if (pair(l.substr(a + 1, b - a - 1), c[0], c[1]) && pair(l.substr(d + 1, e - d - 1), c[2], c[3])) haveC = true;
        }
    }
    return haveId && haveC;
}

bool TrafficMap::addTile(int x, int y, const std::string& stamp, const uint8_t* rgba, int w, int h) {
    if (x < 0 || x > 2 || y < 0 || y > 2 || w != 200 || h != 200) return false;
    auto& set = sets_[stamp];
    if (set.empty()) set.resize(9);
    set[(size_t)x * 3 + (size_t)y].have = true;
    set[(size_t)x * 3 + (size_t)y].px.assign(rgba, rgba + 200 * 200 * 4);
    for (auto& t : set) if (!t.have) return false;
    // a complete set: x is the row and y the column of the 600 x 600 picture (as nrsc5-gui pastes them)
    out_.w = 600; out_.h = 600; out_.stamp = stamp; out_.rgba.assign(600 * 600 * 4, 255);
    for (int tx = 0; tx < 3; tx++) for (int ty = 0; ty < 3; ty++) {
        const auto& t = set[(size_t)tx * 3 + (size_t)ty];
        for (int r = 0; r < 200; r++) memcpy(&out_.rgba[(size_t)(tx * 200 + r) * 600 * 4 + (size_t)(ty * 200) * 4], &t.px[(size_t)r * 200 * 4], 200 * 4);
    }
    ready_ = true;
    // older sets are of no use any more
    for (auto it = sets_.begin(); it != sets_.end();) if (it->first < stamp) it = sets_.erase(it); else ++it;
    return true;
}

int TrafficMap::tilesOfCurrent() const { if (sets_.empty()) return 0; int n = 0; for (auto& t : sets_.rbegin()->second) n += t.have; return n; }

void radarComposite(const uint8_t* rgba, int w, int h, MapImage& out) {
    out.w = w; out.h = h; out.rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t* p = rgba + i * 4; uint8_t* o = &out.rgba[i * 4];
        for (int c = 0; c < 3; c++) o[c] = (uint8_t)(p[c] + (40u * (255u - p[3])) / 255u);   // colour is premultiplied by alpha: over a dark grey
        o[3] = 255;
    }
}

}  // namespace hdr
}  // namespace dect2
