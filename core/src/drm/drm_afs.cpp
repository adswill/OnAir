// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/drm/drm_afs.h"
#include "dect2/drm/drm_msg.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace dect2 {

namespace {
struct Reader {
    const uint8_t* p; size_t n; size_t bit;
    uint32_t get(int k) { uint32_t v = 0; for (int i = 0; i < k; i++, bit++) v = (v << 1) | (bit / 8 < n ? (p[bit / 8] >> (7 - bit % 8)) & 1u : 0u); return v; }
    int left() const { return (int)(n * 8 - bit); }
};
int otherIdBits(int sys) {
    switch (sys) {
    case 0: case 1: case 3: case 6: case 9: return 24;
    case 4: case 7: case 10: return 16;
    case 11: return 32;
    default: return 0;
    }
}
int freqBits(int sys) { return sys <= 2 ? 16 : 8; }
int weekdayFromMjd(int mjd) { return (mjd + 2 + 700000) % 7; }     // MJD 0 was a Wednesday (2 from Monday)
void addUnique(std::vector<DrmAltFreq>& v, const DrmAltFreq& a) {
    for (auto& x : v)
        if (x.khz == a.khz && x.kind == a.kind && x.shortId == a.shortId && x.dabChannel == a.dabChannel && x.scheduleId == a.scheduleId && x.regionId == a.regionId) { x = a; return; }
    if (v.size() < 128) v.push_back(a);
}
}

std::string drmDabChannelName(int c) {
    if (c < 64 || c > 101) return "";
    if (c >= 96) { char b[8]; std::snprintf(b, sizeof b, "13%c", 'A' + c - 96); return b; }
    const int k = c - 64;
    char b[8]; std::snprintf(b, sizeof b, "%d%c", 5 + k / 4, 'A' + k % 4);
    return b;
}

void DrmAfs::clear() { afs_.clear(); sched_.clear(); ver3_ = ver4_ = ver11_ = -1; }

void DrmAfs::feed(const uint8_t* d, int bytes) {
    int pos = 0;
    while (pos + 2 <= bytes) {
        const int len = d[pos] >> 1, ver = d[pos] & 1, typ = d[pos + 1] >> 4, nib = d[pos + 1] & 15;
        if (len == 0 && ver == 0 && typ == 0 && nib == 0) break;
        if (pos + 2 + len > bytes) break;
        const uint8_t* b = d + pos + 2;
        if (typ == 3) {
            if (ver3_ != ver) { afs_.erase(std::remove_if(afs_.begin(), afs_.end(), [](const DrmAltFreq& a) { return a.shortId < 0; }), afs_.end()); ver3_ = ver; }
            const bool sync = nib & 8, layer = nib & 4, restr = nib & 2, regSch = nib & 1;
            size_t o = 0;
            DrmAltFreq a; a.synchronous = sync; a.enhancement = layer;
            if (restr && (int)o < len) a.shortIdMask = b[o++] >> 4;
            if (regSch && (int)o < len) { a.regionId = b[o] >> 4; a.scheduleId = b[o] & 15; o++; }
            for (; (int)o + 2 <= len; o += 2) {
                const uint32_t v = ((uint32_t)b[o] << 8) | b[o + 1];
                a.modeE = v & 0x8000;
                a.khz = (v & 0x7FFF) * (a.modeE ? 10.0 : 1.0);
                addUnique(afs_, a);
            }
        } else if (typ == 4 && len == 4) {
            // the header's 4 bit field is the Schedule Id, the body: day code 7, start 11, duration 14
            if (ver4_ != ver) { sched_.clear(); ver4_ = ver; }
            Reader r{b, (size_t)len, 0};
            DrmAfsSchedule s; s.id = nib; s.dayCode = (int)r.get(7); s.startMin = (int)r.get(11); s.durationMin = (int)r.get(14);
            bool seen = false;
            for (auto& x : sched_) if (x.id == s.id && x.dayCode == s.dayCode && x.startMin == s.startMin && x.durationMin == s.durationMin) seen = true;
            if (!seen && sched_.size() < 64) sched_.push_back(s);
        } else if (typ == 11 && len >= 1) {
            if (ver11_ != ver) { afs_.erase(std::remove_if(afs_.begin(), afs_.end(), [](const DrmAltFreq& a) { return a.shortId >= 0; }), afs_.end()); ver11_ = ver; }
            // the header nibble: Short Id/Announcement flag 1, field 2, Region/Schedule flag 1
            const bool announcement = nib & 8;
            const int sid = (nib >> 1) & 3;
            const bool regSch = nib & 1;
            Reader r{b, (size_t)len, 0};
            const bool same = r.get(1); r.get(2);
            const int sys = (int)r.get(5);
            if (!announcement && sys <= 11) {
                DrmAltFreq a; a.shortId = sid; a.sameService = same;
                if (regSch) { a.regionId = (int)r.get(4); a.scheduleId = (int)r.get(4); }
                const int ob = otherIdBits(sys);
                if (ob) { a.otherIdValid = true; a.otherId = r.get(ob); }
                a.kind = sys == 0 ? 0 : sys <= 2 ? 1 : sys <= 8 ? 2 : 3;
                const int fb = freqBits(sys);
                bool any = false;
                while (r.left() >= fb) {
                    uint32_t v = r.get(fb);
                    if (sys == 0) { a.modeE = v & 0x8000; a.khz = (v & 0x7FFF) * (a.modeE ? 10.0 : 1.0); }
                    else if (sys <= 2) a.khz = v & 0x7FFF;
                    else if (sys <= 5) { if (v > 204) continue; a.khz = 87500.0 + v * 100.0; }
                    else if (sys <= 8) { if (v > 140) continue; a.khz = 76000.0 + v * 100.0; }
                    else { a.dabChannel = drmDabChannelName((int)v); if (a.dabChannel.empty()) continue; a.khz = 0; }
                    any = true;
                    addUnique(afs_, a);
                }
                if (!any) { a.khz = 0; addUnique(afs_, a); }    // only another service id: the same programme elsewhere, frequency unknown
            }
        }
        pos += 2 + len;
    }
}

bool drmScheduleActive(const DrmAfsSchedule& s, int wd, int minute) {
    for (int d = 0; d < 7; d++) {
        if (!(s.dayCode & (0x40 >> d))) continue;
        // minutes since the most recent start on weekday d
        int ago = ((wd - d + 7) % 7) * 1440 + minute - s.startMin;
        if (ago < 0) ago += 7 * 1440;
        if (ago < s.durationMin) return true;
    }
    return false;
}

void DrmAfs::applySchedule(int year, int month, int day, int hour, int minute) {
    if (year == 0) { for (auto& a : afs_) { a.activeNow = true; a.hasSchedule = false; } return; }
    const int wd = weekdayFromMjd(drm::dateToMjd(year, month, day));
    for (auto& a : afs_) {
        a.hasSchedule = false; a.activeNow = true;
        if (!a.scheduleId) continue;
        bool active = false;
        for (const auto& s : sched_) if (s.id == a.scheduleId) { a.hasSchedule = true; active = active || drmScheduleActive(s, wd, hour * 60 + minute); }
        if (a.hasSchedule) a.activeNow = active;
    }
}

std::vector<uint8_t> drmAfsEntityBody3(bool synchronous, const std::vector<uint32_t>& kHz, int regionId, int scheduleId, int restrictMask) {
    const bool regSch = regionId || scheduleId, restr = restrictMask >= 0;
    std::vector<uint8_t> v;
    v.push_back((uint8_t)(((synchronous ? 8 : 0) | (restr ? 2 : 0) | (regSch ? 1 : 0)) << 4));   // sdcPutEntity takes the first nibble from the high half
    if (restr) v.push_back((uint8_t)(restrictMask << 4));
    if (regSch) v.push_back((uint8_t)((regionId << 4) | scheduleId));
    for (uint32_t f : kHz) { v.push_back((uint8_t)(f >> 8)); v.push_back((uint8_t)f); }
    return v;
}
std::vector<uint8_t> drmAfsEntityBody4(int id, int day, int start, int dur) {
    const uint32_t w = ((uint32_t)day << 25) | ((uint32_t)start << 14) | (uint32_t)dur;
    return {(uint8_t)(id << 4), (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
}
std::vector<uint8_t> drmAfsEntityBody11(int shortId, int sys, bool same, uint32_t other, int otherBits, const std::vector<uint32_t>& codes, int fb) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)(shortId << 5));
    v.push_back((uint8_t)((same ? 0x80 : 0) | sys));
    for (int i = otherBits - 8; i >= 0; i -= 8) v.push_back((uint8_t)(other >> i));
    for (uint32_t c : codes) { if (fb == 16) v.push_back((uint8_t)(c >> 8)); v.push_back((uint8_t)c); }
    return v;
}

// ---- the station book

namespace {
void unite(std::vector<double>& a, const std::vector<double>& b) { for (double x : b) if (std::find(a.begin(), a.end(), x) == a.end()) a.push_back(x); std::sort(a.begin(), a.end()); }
std::string clean(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
std::string listText(const std::vector<double>& v) { std::string s; char b[32]; for (double x : v) { std::snprintf(b, sizeof b, "%s%.1f", s.empty() ? "" : ",", x); s += b; } return s; }
std::vector<double> listParse(const std::string& s) { std::vector<double> v; std::stringstream t(s); std::string i; while (std::getline(t, i, ',')) if (!i.empty()) v.push_back(atof(i.c_str())); return v; }
}

void DrmStationBook::note(const DrmBookEntry& e) {
    if (!e.id) return;
    DrmBookEntry& d = e_[e.id];
    d.id = e.id; d.audio = e.audio;
    if (!e.label.empty()) d.label = e.label;
    if (!e.language.empty()) d.language = e.language;
    if (!e.country.empty()) d.country = e.country;
    if (!e.programme.empty()) d.programme = e.programme;
    if (!e.text.empty()) d.text = e.text;
    unite(d.heardKhz, e.heardKhz); unite(d.altKhz, e.altKhz);
    d.bestSnr = std::max(d.bestSnr, e.bestSnr);
    d.lastHeard = std::max(d.lastHeard, e.lastHeard);
    if (e_.size() > 2000) { auto oldest = e_.begin(); for (auto it = e_.begin(); it != e_.end(); ++it) if (it->second.lastHeard < oldest->second.lastHeard) oldest = it; e_.erase(oldest); }
}

std::vector<DrmBookEntry> DrmStationBook::newestFirst() const {
    std::vector<DrmBookEntry> v; v.reserve(e_.size());
    for (const auto& kv : e_) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), [](const DrmBookEntry& a, const DrmBookEntry& b) { return a.lastHeard > b.lastHeard; });
    return v;
}

bool DrmStationBook::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) { DIAG(733185, "The DRM station book %s could not be opened for writing, so the stations heard are not saved.", path.c_str()); return false; }
    f << "OnAirDrmBook1\n";
    for (const auto& kv : e_) {
        const auto& e = kv.second;
        f << e.id << '\t' << (e.audio ? 1 : 0) << '\t' << e.bestSnr << '\t' << e.lastHeard << '\t' << clean(e.label) << '\t' << clean(e.language) << '\t' << clean(e.country) << '\t'
          << clean(e.programme) << '\t' << clean(e.text) << '\t' << listText(e.heardKhz) << '\t' << listText(e.altKhz) << '\n';
    }
    if (!f) DIAG(122458, "Writing the DRM station book %s failed (the disk may be full), so the saved list may be incomplete.", path.c_str());
    return (bool)f;
}

bool DrmStationBook::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string l;
    if (!std::getline(f, l) || l != "OnAirDrmBook1") { DIAG_DEBUG(609406, "The DRM station book %s does not start with the expected OnAirDrmBook1 header, so it is not loaded.", path.c_str()); return false; }
    e_.clear();
    while (std::getline(f, l)) {
        std::vector<std::string> c; std::stringstream t(l); std::string i;
        while (std::getline(t, i, '\t')) c.push_back(i);
        if (c.size() < 9) continue;
        while (c.size() < 11) c.push_back("");
        DrmBookEntry e; e.id = (uint32_t)strtoul(c[0].c_str(), nullptr, 10); e.audio = c[1] == "1"; e.bestSnr = (float)atof(c[2].c_str()); e.lastHeard = atoll(c[3].c_str());
        e.label = c[4]; e.language = c[5]; e.country = c[6]; e.programme = c[7]; e.text = c[8]; e.heardKhz = listParse(c[9]); e.altKhz = listParse(c[10]);
        if (e.id) e_[e.id] = e;
    }
    return true;
}

} // namespace dect2
