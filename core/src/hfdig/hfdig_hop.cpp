// Band hopping: see hfdig_hop.h.
#include "dect2/hfdig/hfdig_hop.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {

namespace {
const int kCoord[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};   // 160 80 60 40 30 20 17 15 12 10 m (multiband numbers), minute 0, 2, 4 ... 18
int64_t fdiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}

int hopBand(const HopPlan& p, int64_t utc) {
    const int slotSec = p.slotSec > 0 ? p.slotSec : 120;
    const int64_t slot = fdiv(utc, slotSec);
    const int hour = (int)(((utc % 86400) + 86400) % 86400 / 3600);
    const uint16_t on = p.hours[(size_t)hour];
    if (!on) return -1;
    if (p.coordinated) {
        const int start = (int)(((slot % 10) + 10) % 10);
        for (int k = 0; k < 10; k++) { const int b = kCoord[(start + k) % 10]; if (on >> b & 1) return b; }
        for (int b = 10; b < 16; b++) if (on >> b & 1) return b;   // only bands outside the table (6 m)
        return -1;
    }
    int n = 0;
    for (int b = 0; b < 16; b++) n += on >> b & 1;
    int k = (int)(((slot % n) + n) % n);
    for (int b = 0; b < 16; b++) if (on >> b & 1) { if (k == 0) return b; k--; }
    return -1;
}

int64_t hopNextChange(const HopPlan& p, int64_t utc) {
    const int s = p.slotSec > 0 ? p.slotSec : 120;
    return (fdiv(utc, s) + 1) * s;
}

std::string hopToText(const HopPlan& p) {
    std::string o = std::string("c=") + (p.coordinated ? "1" : "0") + ";h=";
    char b[8];
    for (int h = 0; h < 24; h++) { std::snprintf(b, sizeof b, "%s%x", h ? "," : "", p.hours[(size_t)h]); o += b; }
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
            while (h < 24 && std::getline(hs, x, ',')) p.hours[(size_t)h++] = (uint16_t)std::strtoul(x.c_str(), nullptr, 16);
        }
    }
    return p;
}

}  // namespace dect2
