// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SAME / EAS protocol layer on top of the Rust crate rust/onair_same: the functions of same_proto.h; same_proto.cpp keeps the
// county table (state) and the file loader.
#include "dect2/same/same_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/same.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace dect2 {
namespace same {

static_assert(CountryUs == 0 && CountryCanada == 1, "Country values are mirrored in rust/onair_same/src/locate.rs");

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t m = 0, l = 0;
        onair_same_struct_sizes(&m, &l);
        dect2::rsAbiCheck("onair_same", {m, l}, {sizeof(OnairSameMsg), sizeof(OnairSameLoc)});
    }
} abiCheck;

inline const uint8_t* u8(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }
inline const char* cs(const uint8_t* p) { return reinterpret_cast<const char*>(p); }

// Runs a Rust text writer with a buffer that grows until the text fits
template <class F> std::string text(F f) {
    std::string s(256, '\0');
    for (;;) {
        const size_t n = f((uint8_t*)&s[0], s.size());
        if (n != SIZE_MAX) { s.resize(n); return s; }
        s.assign(s.size() * 4, '\0');
    }
}
} // namespace

const char* originatorName(const std::string& org) { return cs(onair_same_originator_name(u8(org), org.size())); }
const char* eventName(const std::string& code) { return cs(onair_same_event_name(u8(code), code.size())); }
const char* pcaName(int d) { return cs(onair_same_pca_name(d)); }
const char* stateName(int fips, Country c) { return cs(onair_same_state_name(fips, (int)c)); }
std::string stateAbbr(int fips, Country c) { return cs(onair_same_state_abbr(fips, (int)c)); }
bool stateCentre(int fips, Country c, double& lat, double& lon) {
    double a = 0, b = 0;
    if (!onair_same_state_centre(fips, (int)c, &a, &b)) return false;
    lat = a; lon = b;
    return true;
}
bool validChar(uint8_t c) { return onair_same_valid_char((uint32_t)c); }

std::string trimMessage(const std::string& b) {
    size_t start = 0;
    const size_t n = onair_same_trim_message(u8(b), b.size(), &start);
    return n ? b.substr(start, n) : std::string();
}

Location locate(const std::string& code, Country c) {
    Location l;
    l.code = code;
    // the county name of the user's table, when the code is six digits with a county part
    std::string county;
    if (code.size() == 6) {
        bool dig = true;
        for (char ch : code) if (ch < '0' || ch > '9') dig = false;
        if (dig) {
            const int st = (code[1] - '0') * 10 + (code[2] - '0'), co = (code[3] - '0') * 100 + (code[4] - '0') * 10 + (code[5] - '0');
            if (co) county = countyNameOf(st, co);
        }
    }
    OnairSameLoc info;
    l.text = text([&](uint8_t* out, size_t cap) { return onair_same_locate(u8(code), code.size(), (int)c, u8(county), county.size(), &info, out, cap); });
    if (!info.ok) return l;
    l.pca = info.pca; l.state = info.state; l.county = info.county;
    l.pcaName = pcaName(l.pca);
    if (info.region_known) l.stateName = stateName(l.state, c);
    l.hasPos = info.has_pos != 0;
    l.lat = info.lat; l.lon = info.lon;
    l.countyName = county;
    return l;
}

Message parse(const std::string& text, Country c) {
    Message m;
    m.raw = text;
    OnairSameMsg r;
    onair_same_parse(u8(text), text.size(), &r);
    m.valid = r.valid != 0;
    m.eom = r.eom != 0;
    m.err = cs(r.err);
    if (r.org_set) m.org = text.substr(5, 3);
    if (r.event_set) m.event = text.substr(9, 3);
    for (uint32_t k = 0; k < r.nloc; k++) m.locs.push_back(locate(text.substr(13 + 7 * (size_t)k, 6), c));
    m.purgeMin = r.purge_min;
    m.issueDay = r.issue_day; m.issueHour = r.issue_hour; m.issueMin = r.issue_min;
    if (r.call_set) m.callsign = text.substr(r.call_start, r.call_len);
    if (r.valid && !r.eom) { m.orgName = cs(r.org_name); m.eventName = cs(r.event_name); m.eventKind = cs(r.event_kind); }
    return m;
}

int64_t issueEpoch(int year, int day, int hour, int minute) { return onair_same_issue_epoch(year, day, hour, minute); }
std::string issueText(int year, int day, int hour, int minute) {
    return text([&](uint8_t* out, size_t cap) { return onair_same_issue_text(year, day, hour, minute, out, cap); });
}
std::string purgeText(int minutes) {
    return text([&](uint8_t* out, size_t cap) { return onair_same_purge_text(minutes, out, cap); });
}

std::map<int, std::string> parseCountyCsv(const std::string& t) {
    std::string out(t.size() * 2 + 16, '\0'), scratch(t.size() + 1, '\0');
    const size_t n = onair_same_read_county_csv(u8(t), t.size(), (uint8_t*)&out[0], out.size(), (uint8_t*)&scratch[0], scratch.size());
    std::map<int, std::string> rows;
    if (n == SIZE_MAX) return rows;
    for (size_t at = 0; at + 8 <= n;) {
        uint32_t key, len;
        memcpy(&key, &out[at], 4); memcpy(&len, &out[at + 4], 4);   // little endian on every platform we build for
        rows[(int)key] = out.substr(at + 8, len);
        at += 8 + len;
    }
    return rows;
}

} // namespace same
} // namespace dect2
