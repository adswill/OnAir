// Band hopping: see hfdig_hop.h.
#include "dect2/hfdig/hfdig_hop.h"
#include "dect2/common/diag.h"
#include "dect2/ham/ham_station.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_mbsel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {

namespace {
const int kCoord[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};   // 160 80 60 40 30 20 17 15 12 10 m (multiband numbers), minute 0, 2, 4 ... 18
int64_t fdiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}

namespace {
// the bands really on in an hour: user frequencies that are off, of another mode or no longer in the list are dropped
uint32_t effectiveMask(const HopPlan& p, uint32_t on, int mode) {
    const int n = (int)std::min<size_t>(p.user.size(), (size_t)kHopUserMax);
    for (int b = kHopUserBase; b < 32; b++) {
        const int i = b - kHopUserBase;
        const bool keep = i < n && p.user[(size_t)i].enabled && p.user[(size_t)i].hz > 0 && (mode < 0 || p.user[(size_t)i].mode == mode);
        if (!keep) on &= ~(1u << b);
    }
    return on;
}
int pickInTurn(uint32_t on, int from, int64_t slot) {
    int n = 0;
    for (int b = from; b < 32; b++) n += on >> b & 1;
    if (n == 0) return -1;
    int k = (int)(((slot % n) + n) % n);
    for (int b = from; b < 32; b++) if (on >> b & 1) { if (k == 0) return b; k--; }
    return -1;
}
std::string escape(const std::string& s) {
    std::string o; char b[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == ' ' || c == '.' || c == '_' || c == '-') o += (char)c;
        else { std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) { o += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += s[i];
    }
    return o;
}
}  // namespace

int hopBand(const HopPlan& p, int64_t utc, int mode) {
    const int slotSec = p.slotSec > 0 ? p.slotSec : 120;
    const int64_t slot = fdiv(utc, slotSec);
    const int hour = (int)(((utc % 86400) + 86400) % 86400 / 3600);
    const uint32_t on = effectiveMask(p, p.hours[(size_t)hour], mode);
    if (!on) return -1;
    if (p.coordinated) {
        const int start = (int)(((slot % 10) + 10) % 10);
        for (int k = 0; k < 10; k++) { const int b = kCoord[(start + k) % 10]; if (on >> b & 1) return b; }
        return pickInTurn(on, 10, slot);   // only bands outside the table (6 m, the user's own): in turn
    }
    return pickInTurn(on, 0, slot);
}

int64_t hopNextChange(const HopPlan& p, int64_t utc) {
    const int s = p.slotSec > 0 ? p.slotSec : 120;
    return (fdiv(utc, s) + 1) * s;
}

double hopDialHz(const HopPlan& p, int band, int mode) {
    if (band >= 0 && band < kMbBands) return mbDialHz(band, mode);
    const int i = band - kHopUserBase;
    return i >= 0 && i < (int)p.user.size() ? p.user[(size_t)i].hz : 0;
}

std::string hopBandName(const HopPlan& p, int band) {
    if (band >= 0 && band < kMbBands) return mbBand(band).name;
    const int i = band - kHopUserBase;
    if (i < 0 || i >= (int)p.user.size()) return "";
    const HopUserFreq& u = p.user[(size_t)i];
    char b[32]; std::snprintf(b, sizeof b, "%.4f MHz", u.hz / 1e6);
    return u.label.empty() ? std::string(b) : u.label + " (" + b + ")";
}

std::string hopAddUser(HopPlan& p, const HopUserFreq& f) {
    if (!(f.hz >= 100e3 && f.hz <= 1.3e9)) return DIAG_TEXT(420136, "The frequency %.0f Hz is outside 100 kHz to 1.3 GHz, so it was not added to the band hopping list.", f.hz);
    if (f.mode < 0 || f.mode >= kFtxModes) return DIAG_TEXT(417905, "The mode %d is not one of the weak-signal modes, so the frequency was not added to the band hopping list.", f.mode);
    for (size_t i = 0; i < p.user.size(); i++)
        if (p.user[i].mode == f.mode && std::fabs(p.user[i].hz - f.hz) < 1.0) { p.user[i].label = f.label; p.user[i].enabled = f.enabled; return ""; }
    if ((int)p.user.size() >= kHopUserMax) return DIAG_TEXT(863109, "The band hopping list already holds %d frequencies of your own, so delete one before adding another.", kHopUserMax);
    p.user.push_back(f);
    const uint32_t bit = 1u << (kHopUserBase + (int)p.user.size() - 1);
    for (auto& h : p.hours) h |= bit;       // a new entry is on in every hour; the user narrows it down in the table
    return "";
}

void hopRemoveUser(HopPlan& p, int index) {
    if (index < 0 || index >= (int)p.user.size()) return;
    p.user.erase(p.user.begin() + index);
    const int b = kHopUserBase + index;
    for (auto& h : p.hours) {
        const uint32_t low = h & ((1u << b) - 1u);          // the bits below the removed one stay
        const uint32_t high = b + 1 < 32 ? (h >> (b + 1)) << b : 0;   // the later entries move down one place
        h = low | high;
    }
}

std::string hopUserWarning(double hz, int mode) {
    char f[32]; std::snprintf(f, sizeof f, "%.4f MHz", hz / 1e6);
    bool inBand = false;
    for (int r = 1; r <= 3 && !inBand; r++) inBand = ham::segmentAt(r, hz) != nullptr;
    if (!inBand) return std::string(f) + " is not inside an amateur band of the IARU plans: receiving is fine, but check your licence before you transmit here.";
    if (mode >= 0 && mode < kMbModes) {
        const int b = mbBandOf(hz);
        const double d = b >= 0 ? mbDialHz(b, mode) : 0;
        if (d > 0 && std::fabs(hz - d) > 5000) {
            char m[160]; std::snprintf(m, sizeof m, " is away from the usual %s frequency of this band (%.4f MHz): fine for a special event or an expedition, but check the band plan before you transmit.", ftxModeName(mode), d / 1e6);
            return std::string(f) + m;
        }
    }
    return "";
}

std::string hopToText(const HopPlan& p) {
    std::string o = std::string("c=") + (p.coordinated ? "1" : "0") + ";h=";
    char b[16];
    for (int h = 0; h < 24; h++) { std::snprintf(b, sizeof b, "%s%x", h ? "," : "", p.hours[(size_t)h]); o += b; }
    if (!p.user.empty()) {
        o += ";u=";
        for (size_t i = 0; i < p.user.size(); i++) {
            const HopUserFreq& u = p.user[i];
            std::snprintf(b, sizeof b, "%.0f:%d:%d:", u.hz, u.mode, u.enabled ? 1 : 0);
            o += (i ? "|" : "") + std::string(b) + escape(u.label);
        }
    }
    return o;
}

HopPlan hopFromText(const std::string& s) {
    HopPlan p;
    std::stringstream ss(s);
    std::string kv;
    while (std::getline(ss, kv, ';')) {
        if (kv.rfind("c=", 0) == 0) p.coordinated = kv.substr(2) == "1";
        else if (kv.rfind("h=", 0) == 0) {
            std::stringstream hs(kv.substr(2));
            std::string x;
            int h = 0;
            while (h < 24 && std::getline(hs, x, ',')) p.hours[(size_t)h++] = (uint32_t)std::strtoul(x.c_str(), nullptr, 16);
        } else if (kv.rfind("u=", 0) == 0) {
            std::stringstream us(kv.substr(2));
            std::string e;
            while (std::getline(us, e, '|') && (int)p.user.size() < kHopUserMax) {
                HopUserFreq u;
                std::stringstream es(e);
                std::string a, m, en, lab;
                if (!std::getline(es, a, ':') || !std::getline(es, m, ':') || !std::getline(es, en, ':')) continue;
                std::getline(es, lab);
                u.hz = std::strtod(a.c_str(), nullptr); u.mode = std::atoi(m.c_str()); u.enabled = en == "1"; u.label = unescape(lab);
                if (u.hz >= 100e3 && u.hz <= 1.3e9 && u.mode >= 0 && u.mode < kFtxModes) p.user.push_back(u);
                else p.user.push_back(HopUserFreq{0, 0, "", false});   // keep the numbering of the plan's bits; hz 0 is never hopped to
            }
        }
    }
    return p;
}

}  // namespace dect2
