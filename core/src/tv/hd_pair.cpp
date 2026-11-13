// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/hd_pair.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {
namespace hd {

const char* tierName(Tier t) { return t == kSD ? "SD" : t == kHD ? "HD" : t == kUHD ? "UHD" : "?"; }

Tier tierFromResolution(int w, int h) {
    if (w <= 0 && h <= 0) return kUnknown;
    if (h >= 2160 || w >= 3840) return kUHD;
    if (h >= 720 || w >= 1280) return kHD;
    return kSD;
}

Tier tierFromServiceType(int t) {
    switch (t) {
        case 0x11: case 0x19: case 0x1F: return kHD;
        case 0x01: case 0x16: return kSD;
        default: return kUnknown;
    }
}

Tier tierFromStreamType(int st) {
    if (st == 0x02 || st == 0x01) return kSD;
    if (st == 0x24 || st == 0x27) return kHD;
    return kUnknown;
}

// ---- SPS ----
namespace {
struct Bits {
    std::vector<uint8_t> d; size_t pos = 0; bool bad = false;
    unsigned u1() { if (pos >= d.size() * 8) { bad = true; return 0; } const unsigned b = (d[pos >> 3] >> (7 - (pos & 7))) & 1; pos++; return b; }
    unsigned u(int n) { unsigned v = 0; while (n-- > 0) v = v << 1 | u1(); return v; }
    unsigned ue() {
        int z = 0;
        while (!bad && u1() == 0) { if (++z > 31) { bad = true; return 0; } }
        return z ? ((1u << z) - 1) + u(z) : 0;
    }
    int se() { const unsigned k = ue(); return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2); }
    void skip(size_t n) { pos += n; if (pos > d.size() * 8) bad = true; }
};
std::vector<uint8_t> rbsp(const uint8_t* p, size_t n) {
    std::vector<uint8_t> o; o.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && p[i] == 3) { zeros = 0; continue; }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        o.push_back(p[i]);
    }
    return o;
}
void skipScaling(Bits& b, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size && !b.bad; j++) {
        if (next != 0) { next = (last + b.se() + 256) % 256; }
        last = next == 0 ? last : next;
    }
}
} // namespace

bool spsResolution(bool hevc, const uint8_t* nal, size_t n, int& width, int& height) {
    width = height = 0;
    if (!nal) return false;
    if (!hevc) {
        if (n < 5 || (nal[0] & 0x1F) != 7) return false;
        Bits b; b.d = rbsp(nal + 1, n - 1);
        const int profile = (int)b.u(8); b.skip(16);
        b.ue();   // sps id
        int chroma = 1; bool sep = false;
        switch (profile) {
            case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135: {
                chroma = (int)b.ue();
                if (chroma == 3) sep = b.u1() != 0;
                b.ue(); b.ue(); b.u1();
                if (b.u1()) for (int i = 0; i < (chroma != 3 ? 8 : 12) && !b.bad; i++) if (b.u1()) skipScaling(b, i < 6 ? 16 : 64);
                break;
            }
            default: break;
        }
        b.ue();   // log2_max_frame_num
        const unsigned poc = b.ue();
        if (poc == 0) b.ue();
        else if (poc == 1) { b.u1(); b.se(); b.se(); const unsigned c = b.ue(); for (unsigned i = 0; i < c && i < 256 && !b.bad; i++) b.se(); }
        b.ue(); b.u1();   // max_num_ref_frames, gaps
        const unsigned wm = b.ue(), hm = b.ue();
        const unsigned frameOnly = b.u1();
        if (!frameOnly) b.u1();
        b.u1();   // direct_8x8
        unsigned cl = 0, cr = 0, ct = 0, cb = 0;
        if (b.u1()) { cl = b.ue(); cr = b.ue(); ct = b.ue(); cb = b.ue(); }
        if (b.bad || wm > 1000 || hm > 1000) return false;
        const int cat = sep ? 0 : chroma;
        const int cux = cat == 0 || cat == 3 ? 1 : 2;
        const int cuy = (cat == 1 ? 2 : 1) * (frameOnly ? 1 : 2);
        width = (int)((wm + 1) * 16) - cux * (int)(cl + cr);
        height = (int)((2 - frameOnly) * (hm + 1) * 16) - cuy * (int)(ct + cb);
        return width > 0 && height > 0;
    }
    if (n < 16 || ((nal[0] >> 1) & 0x3F) != 33) return false;
    Bits b; b.d = rbsp(nal + 2, n - 2);
    b.skip(4);
    const int maxSub = (int)b.u(3);
    b.skip(1);
    b.skip(96);   // general profile / tier / level
    std::vector<int> subProf(8, 0), subLev(8, 0);
    for (int i = 0; i < maxSub; i++) { subProf[i] = (int)b.u1(); subLev[i] = (int)b.u1(); }
    if (maxSub > 0) for (int i = maxSub; i < 8; i++) b.skip(2);
    for (int i = 0; i < maxSub; i++) { if (subProf[i]) b.skip(88); if (subLev[i]) b.skip(8); }
    b.ue();   // sps id
    const unsigned chroma = b.ue();
    bool sep = false;
    if (chroma == 3) sep = b.u1() != 0;
    const unsigned w = b.ue(), h = b.ue();
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (b.u1()) { cl = b.ue(); cr = b.ue(); ct = b.ue(); cb = b.ue(); }
    if (b.bad || w > 16384 || h > 16384) return false;
    const unsigned cat = sep ? 0 : chroma;
    const int sx = (cat == 1 || cat == 2) ? 2 : 1, sy = cat == 1 ? 2 : 1;
    width = (int)w - sx * (int)(cl + cr);
    height = (int)h - sy * (int)(ct + cb);
    return width > 0 && height > 0;
}

// ---- names ----
namespace {
bool isRegion(const std::string& t) {
    static const char* const r[] = {"south", "north", "east", "west", "midlands", "london", "scotland", "wales", "ireland", "yorkshire", "yorks", "anglia",
                                    "ulster", "ni", "sued", "nord", "ost", "west", "regional"};
    for (const char* x : r) if (t == x) return true;
    return false;
}
} // namespace

NameInfo normalizeName(const std::string& name) {
    NameInfo o;
    std::string s;
    for (size_t i = 0; i < name.size(); i++) {
        const unsigned char c = (unsigned char)name[i];
        if (c == '+' && i > 0 && name[i + 1] >= '0' && name[i + 1] <= '9' && name[i - 1] != ' ') s += ' ';   // "ITV2+1"
        if (std::isalnum(c) || c == '+' || c >= 0x80) s += (char)std::tolower(c);
        else s += ' ';
    }
    std::vector<std::string> tok;
    std::istringstream is(s);
    for (std::string t; is >> t;) tok.push_back(t);
    std::vector<std::string> keep;
    for (std::string t : tok) {
        if (t == "hd" || t == "hd+" || t == "fhd" || t == "hdtv" || t == "hd+") { o.marker = std::max(o.marker, kHD); continue; }
        if (t == "uhd" || t == "4k" || t == "uhdtv" || t == "2160p") { o.marker = kUHD; continue; }
        if (t == "sd" || t == "hevc" || t == "h265" || t == "+") continue;
        if (t.size() >= 2 && t[0] == '+' && std::isdigit((unsigned char)t[1])) {   // +1, +2, +24, +25
            o.timeshift = std::atoi(t.c_str() + 1);
            if (o.timeshift == 0) o.timeshift = 1;
            continue;
        }
        if (t.size() >= 4 && t.compare(t.size() - 2, 2, "hd") == 0 && !std::isspace((unsigned char)t[0])) {   // "rtlhd", "tf1hd"
            const std::string head = t.substr(0, t.size() - 2);
            if (head.size() >= 2 && (std::isdigit((unsigned char)head.back()) || head.size() >= 3)) { o.marker = std::max(o.marker, kHD); t = head; }
        }
        keep.push_back(t);
    }
    while (keep.size() > 1 && isRegion(keep.back())) keep.pop_back();
    for (const std::string& t : keep) o.key += t;
    return o;
}

bool lcnPairs(int sd, int hd) {
    if (sd <= 0 || hd <= 0) return false;
    return hd - sd == 100 || hd - sd == 1000;
}

// ---- pairing ----
Tier Service::tier() const {
    if (const Tier r = tierFromResolution(width, height)) return r;
    const Tier byType = tierFromServiceType(serviceType), byName = normalizeName(name).marker;
    if (byType == kUnknown && byName == kUnknown) {
        const Tier st = tierFromStreamType(streamType);
        if (st != kUnknown) return st;
        return videoCodec == "HEVC" ? kHD : videoCodec == "MPEG-2" ? kSD : kUnknown;
    }
    if (byType == kSD && byName >= kHD) return byName;   // the name says HD, a mis-signalled type does not matter
    return std::max(byType, byName);
}

namespace {
bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
bool sameMux(const Service& a, const Service& b) { return std::fabs(a.freqMhz - b.freqMhz) < 0.01; }

// how well `hdv` is the better version of `sd`; 0 = not at all
Pairing score(const Service& sd, const Service& hdv, const std::vector<Override>& table) {
    Pairing p;
    const NameInfo ns = normalizeName(sd.name), nh = normalizeName(hdv.name);
    for (const Override& o : table) {
        if (o.sd.empty() || normalizeName(o.sd).key != ns.key) continue;
        if (o.hd == "-") return p;   // never pair
        if (ieq(o.hd, hdv.name)) { p.score = 1000; p.reason = "pairing table"; return p; }
    }
    if (sd.linkSid && sd.linkSid == hdv.sid && (!sd.linkTsid || sd.linkTsid == hdv.tsid) && (!sd.linkOnid || sd.linkOnid == hdv.onid)) { p.score = 100; p.reason = "linkage"; return p; }
    const bool name = !ns.key.empty() && ns.key == nh.key && ns.timeshift == nh.timeshift;
    const bool lcn = lcnPairs(sd.lcn, hdv.lcn);
    const bool mux = sameMux(sd, hdv);
    if (name) { p.score = 60 + (lcn ? 30 : 0) + (mux ? 5 : 0); p.reason = lcn ? "same name and LCN + 100" : "same name"; }
    else if (lcn && mux && !sd.provider.empty() && sd.provider == hdv.provider && ns.timeshift == nh.timeshift) { p.score = 60; p.reason = "LCN + 100 in the same multiplex"; }
    return p;
}

Pairing best(const std::vector<Service>& v, size_t i, bool wantHd, const std::vector<Override>& table) {
    Pairing b;
    if (i >= v.size()) return b;
    const Tier mine = v[i].tier();
    for (size_t j = 0; j < v.size(); j++) {
        if (j == i) continue;
        const Tier tj = v[j].tier();
        Pairing p;
        if (wantHd) {
            if (tj < kHD || tj <= mine) continue;
            p = score(v[i], v[j], table);
        } else {
            if (mine < kHD || tj >= mine || tj == kUnknown) continue;
            p = score(v[j], v[i], table);
        }
        if (p.score < 60) continue;
        if (p.score > b.score || (p.score == b.score && b.index >= 0 && sameMux(v[i], v[j]) && !sameMux(v[i], v[b.index]))) { b = p; b.index = (int)j; }
    }
    return b;
}
} // namespace

Pairing findHd(const std::vector<Service>& v, size_t i, const std::vector<Override>& t) { return best(v, i, true, t); }
Pairing findSd(const std::vector<Service>& v, size_t i, const std::vector<Override>& t) { return best(v, i, false, t); }

// ---- catalogue ----
void Catalog::update(double f, const std::vector<Service>& now) {
    if (now.empty()) return;
    std::vector<Service> keep;
    for (const Service& s : svcs_) if (std::fabs(s.freqMhz - f) >= 0.01) keep.push_back(s);
    std::vector<Service> merged = now;   // a size seen while playing is kept when the new list does not know it
    for (Service& n : merged) {
        n.freqMhz = f;
        for (const Service& s : svcs_)
            if (std::fabs(s.freqMhz - f) < 0.01 && s.sid == n.sid && n.width <= 0 && s.width > 0) { n.width = s.width; n.height = s.height; }
    }
    svcs_ = keep;
    svcs_.insert(svcs_.end(), merged.begin(), merged.end());
}

void Catalog::setResolution(double f, int sid, int w, int h) {
    const int i = find(f, sid);
    if (i >= 0 && w > 0 && h > 0) { svcs_[i].width = w; svcs_[i].height = h; }
}

int Catalog::find(double f, int sid) const {
    for (size_t i = 0; i < svcs_.size(); i++) if (std::fabs(svcs_[i].freqMhz - f) < 0.01 && svcs_[i].sid == sid) return (int)i;
    return -1;
}

namespace {
std::string clean(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
std::vector<std::string> split(const std::string& l) {
    std::vector<std::string> o; size_t p = 0;
    for (;;) { const size_t q = l.find('\t', p); if (q == std::string::npos) { o.push_back(l.substr(p)); break; } o.push_back(l.substr(p, q - p)); p = q + 1; }
    return o;
}
} // namespace

std::string Catalog::encode() const {
    std::string o;
    char b[160];
    for (const Service& s : svcs_) {
        snprintf(b, sizeof b, "%.4f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t", s.freqMhz, s.onid, s.tsid, s.sid, s.lcn, s.serviceType, s.streamType, s.width, s.height, s.linkOnid, s.linkTsid, s.linkSid);
        o += b + clean(s.name) + "\t" + clean(s.provider) + "\t" + clean(s.videoCodec) + "\n";
    }
    return o;
}

void Catalog::decode(const std::string& str) {
    svcs_.clear();
    std::istringstream is(str);
    for (std::string l; std::getline(is, l);) {
        const auto f = split(l);
        if (f.size() < 15) continue;
        Service s;
        s.freqMhz = std::atof(f[0].c_str()); s.onid = std::atoi(f[1].c_str()); s.tsid = std::atoi(f[2].c_str()); s.sid = std::atoi(f[3].c_str());
        s.lcn = std::atoi(f[4].c_str()); s.serviceType = std::atoi(f[5].c_str()); s.streamType = std::atoi(f[6].c_str());
        s.width = std::atoi(f[7].c_str()); s.height = std::atoi(f[8].c_str());
        s.linkOnid = std::atoi(f[9].c_str()); s.linkTsid = std::atoi(f[10].c_str()); s.linkSid = std::atoi(f[11].c_str());
        s.name = f[12]; s.provider = f[13]; s.videoCodec = f[14];
        svcs_.push_back(s);
    }
}

std::string encodeOverrides(const std::vector<Override>& v) {
    std::string o;
    for (const Override& x : v) if (!x.sd.empty()) o += clean(x.sd) + "\t" + clean(x.hd) + "\n";
    return o;
}

std::vector<Override> decodeOverrides(const std::string& s) {
    std::vector<Override> o;
    std::istringstream is(s);
    for (std::string l; std::getline(is, l);) {
        const auto f = split(l);
        if (f.size() >= 2 && !f[0].empty()) o.push_back({f[0], f[1]});
    }
    return o;
}

} // namespace hd
} // namespace dect2
