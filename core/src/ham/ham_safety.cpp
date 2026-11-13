// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The safety layer of the ham transmitter: country band plans, duty-cycle limits, the TX audit log. See ham_safety.h.
#include "dect2/ham/ham_safety.h"
#include "dect2/bandplan/bandplan.h"
#include "dect2/ham/ham.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

namespace dect2::ham {

namespace {
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

const bandplan::Plan* planFor(const std::string& country) {
    if (country.empty()) return nullptr;
    const std::string u = upper(country), l = lower(country);
    for (const auto& p : bandplan::builtinPlans())
        for (const auto& c : p.countries) if (upper(c) == u) return &p;
    for (const auto& p : bandplan::builtinPlans()) if (lower(p.id) == l) return &p;
    return nullptr;
}
}  // namespace

std::vector<HamPlanBand> hamCountryBands(const std::string& country) {
    std::vector<HamPlanBand> out;
    const bandplan::Plan* p = planFor(country);
    if (!p) return out;
    for (const bandplan::Band* b : p->bandsFor("ham")) {
        for (const auto& u : hamBands()) {   // a plan band only counts where the ITU table also has an amateur allocation
            const double lo = std::max(b->startHz, u.loHz), hi = std::min(b->endHz, u.hiHz);
            if (hi > lo) out.push_back({b->name, lo, hi});
        }
    }
    std::sort(out.begin(), out.end(), [](const HamPlanBand& a, const HamPlanBand& b) { return a.loHz < b.loHz; });
    return out;
}

bool hamCountryKnown(const std::string& country) { return planFor(country) != nullptr; }
std::string hamCountryPlanName(const std::string& country) { const auto* p = planFor(country); return p ? p->name : std::string(); }

bool hamSignalInCountryBand(const std::string& country, double dialHz, double audioHz, double bwHz, std::string& why) {
    why.clear();
    if (!std::isfinite(dialHz) || !std::isfinite(audioHz) || !std::isfinite(bwHz)) { why = "the frequency is not a number"; return false; }
    if (!hamCountryKnown(country)) { why = "no band plan is known for \"" + country + "\": choose your country"; return false; }
    const auto bands = hamCountryBands(country);
    if (bands.empty()) { why = "the band plan of " + hamCountryPlanName(country) + " lists no amateur band"; return false; }
    const double lo = dialHz + audioHz, hi = lo + std::max(0.0, bwHz);
    for (const auto& b : bands) if (lo >= b.loHz && hi <= b.hiHz) return true;
    why = "the frequency is outside the amateur bands of " + hamCountryPlanName(country);
    return false;
}

HamDutyLimit hamDutyLimit(int mode) {
    HamDutyLimit l;
    if (mode < 0) { l.windowSec = 600; l.maxFraction = 0.5; l.maxBurstSec = 200; return l; }
    l.maxFraction = 0.6;
    l.maxBurstSec = slotPeriod(mode);
    l.windowSec = mode == 3 ? 1200 : 600;   // WSPR: 2 minute slots, a longer window
    return l;
}

double hamDutyUsed(const std::vector<std::pair<double, double>>& log, double now, double windowSec) {
    double s = 0;
    const double from = now - windowSec;
    for (const auto& b : log) {
        const double a = std::max(b.first, from), e = std::min(b.first + b.second, now);
        if (e > a) s += e - a;
    }
    return s;
}

// ---- audit ----
namespace {
std::mutex gAuditMu;
std::deque<TxAuditEntry> gAudit;
std::string gAuditFile;
}  // namespace

std::string txAuditClean(const std::string& text) {
    std::string o;
    for (unsigned char c : text) {
        if (o.size() >= 160) break;
        if (c == '\n' || c == '\r' || c == '\t') o += ' ';
        else if (c >= 32 && c != 127 && c != '"') o += (char)c;
        else if (c == '"') o += '\'';
    }
    return o;
}

std::string txAuditLine(const TxAuditEntry& e) {
    char t[32] = "0000-00-00T00:00:00Z";
    const time_t s = (time_t)std::floor(e.utc);
    struct tm g{};
#ifdef _WIN32
    gmtime_s(&g, &s);
#else
    gmtime_r(&s, &g);
#endif
    strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%SZ", &g);
    char b[512];
    snprintf(b, sizeof b, "%s %s %s %s %.0f+%.0f Hz %.0f Hz %.1f s %s \"%s\"%s%s", t, e.path.c_str(), e.mode.c_str(), e.result.c_str(), e.dialHz, e.audioHz,
             e.bwHz, e.seconds, e.call.empty() ? "-" : e.call.c_str(), txAuditClean(e.message).c_str(), e.reason.empty() ? "" : " : ",
             txAuditClean(e.reason).c_str());
    return b;
}

void txAuditSetFile(const std::string& path) { std::lock_guard<std::mutex> l(gAuditMu); gAuditFile = path; }
std::string txAuditFile() { std::lock_guard<std::mutex> l(gAuditMu); return gAuditFile; }

void txAuditAdd(const TxAuditEntry& in) {
    TxAuditEntry e = in;
    e.message = txAuditClean(e.message);
    e.reason = txAuditClean(e.reason);
    std::lock_guard<std::mutex> l(gAuditMu);
    gAudit.push_back(e);
    while (gAudit.size() > 1000) gAudit.pop_front();
    if (!gAuditFile.empty()) {
        if (FILE* f = fopen(gAuditFile.c_str(), "ab")) {
            const std::string line = txAuditLine(e) + "\n";
            fwrite(line.data(), 1, line.size(), f);
            fclose(f);
        }
    }
}

std::vector<TxAuditEntry> txAuditRecent(size_t max) {
    std::lock_guard<std::mutex> l(gAuditMu);
    const size_t n = std::min(max, gAudit.size());
    return std::vector<TxAuditEntry>(gAudit.end() - (std::ptrdiff_t)n, gAudit.end());
}

void txAuditClear() { std::lock_guard<std::mutex> l(gAuditMu); gAudit.clear(); }

}  // namespace dect2::ham
