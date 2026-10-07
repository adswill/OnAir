// DRM (ETSI ES 201 980 V4.3.1): OFDM parameters, pilot, FAC and SDC cell structure, code rate tables.
#include "dect2/drm_defs.h"
#include "drm_internal.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace dect2 { namespace drm {

namespace {
constexpr double kPi = 3.14159265358979323846;

const ModeParams kModes[5] = {
    // name, Tu, Tg (units of T), Ns, frames, x, y, k0, sdc symbols, D        Tables 2, 47, 58, 60, clause 8.5.3.1, clause 7.6
    {'A', 288, 32, 15, 3, 4, 5, 2, 2, 5},
    {'B', 256, 64, 15, 3, 2, 3, 1, 2, 5},
    {'C', 176, 64, 20, 3, 2, 2, 1, 3, 5},
    {'D', 112, 88, 24, 3, 1, 3, 1, 3, 5},
    {'E', 27, 3, 40, 4, 4, 4, 2, 5, 6},
};

// Table 49: carrier range per spectrum occupancy 0..5 (kmin, kmax); {0, 0} where the mode does not have the occupancy
const int kRange[5][6][2] = {
    {{2, 102}, {2, 114}, {-102, 102}, {-114, 114}, {-98, 314}, {-110, 350}},
    {{1, 91}, {1, 103}, {-91, 91}, {-103, 103}, {-87, 279}, {-99, 311}},
    {{0, 0}, {0, 0}, {0, 0}, {-69, 69}, {0, 0}, {-67, 213}},
    {{0, 0}, {0, 0}, {0, 0}, {-44, 44}, {0, 0}, {-43, 135}},
    {{-106, 106}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
};

// Table 59: carriers with a further power boost (amplitude 2 instead of sqrt 2), four per occupancy
const int kBoost[5][6][4] = {
    {{2, 6, 98, 102}, {2, 6, 110, 114}, {-102, -98, 98, 102}, {-114, -110, 110, 114}, {-98, -94, 310, 314}, {-110, -106, 346, 350}},
    {{1, 3, 89, 91}, {1, 3, 101, 103}, {-91, -89, 89, 91}, {-103, -101, 101, 103}, {-87, -85, 277, 279}, {-99, -97, 309, 311}},
    {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {-69, -67, 67, 69}, {0, 0, 0, 0}, {-67, -65, 211, 213}},
    {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {-44, -43, 43, 44}, {0, 0, 0, 0}, {-43, -42, 134, 135}},
    {{-106, -102, 102, 106}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
};

const double kOccKhz[2][6] = {{4.5, 5, 9, 10, 18, 20}, {100, 0, 0, 0, 0, 0}};

int posMod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }

cf32 phasor(int phase1024, double amp) {
    const double a = 2 * kPi * (double)posMod(phase1024, 1024) / 1024.0;
    return cf32((float)(amp * std::cos(a)), (float)(amp * std::sin(a)));
}

bool isBoosted(int mode, int occ, int k) {
    if (occ < 0 || occ > 5) return false;
    for (int i = 0; i < 4; i++) if (kBoost[mode][occ][i] == k) return true;
    return false;
}

bool isGainPos(const ModeParams& mp, int s, int k) {
    const int n = s % mp.gy;
    return posMod(k - mp.gk0 - n * mp.gx, mp.gx * mp.gy) == 0;
}

int gainPhase(int mode, const ModeParams& mp, int s, int k) {   // clause 8.4.4.3.1
    const int n = s % mp.gy, m = s / mp.gy;
    const int p = (k - mp.gk0 - n * mp.gx) / (mp.gx * mp.gy);
    const GainTables& g = kGainTables[mode];
    const int idx = n * g.cols + m;
    if (mode != kModeE) return posMod(4 * g.z[idx] + p * g.w[idx] + p * p * (1 + s) * g.q, 1024);
    return posMod(p * p * g.w[idx] + p * g.z[idx] + g.qm[idx], 1024);
}

bool lookup(const RefTable& t, int key, int& phase) {
    for (int i = 0; i < t.n; i++) if (t.data[i][0] == key) { phase = t.data[i][1]; return true; }
    return false;
}
} // namespace

const ModeParams& modeParams(int mode) { return kModes[std::max(0, std::min(4, mode))]; }

bool occupancyValid(int mode, int occ) {
    if (mode < 0 || mode > 4 || occ < 0 || occ > 5) return false;
    return kRange[mode][occ][0] != 0 || kRange[mode][occ][1] != 0;
}
void carrierRange(int mode, int occ, int& kmin, int& kmax) { kmin = kRange[mode][occ][0]; kmax = kRange[mode][occ][1]; }
double occupancyKhz(int mode, int occ) { return kOccKhz[mode == kModeE ? 1 : 0][occ]; }
bool carrierUnused(int mode, int k) {
    switch (mode) {
    case kModeA: return k >= -1 && k <= 1;
    case kModeB: case kModeC: case kModeD: return k == 0;
    default: return false;
    }
}

bool pilotRef(int mode, int occ, int s, int k, cf32& ref, int* kind) {
    const ModeParams& mp = modeParams(mode);
    int ph = 0;
    int kd = 0;
    double amp = std::sqrt(2.0);
    if (mode != kModeE && lookup(kFreqRefs[mode], k, ph)) {                // clause 8.4.2.2: the tones stay continuous from symbol to symbol
        if (mode == kModeD && (k == 7 || k == 21) && (s & 1)) ph += 512;
        kd = kCellFreqRef;
    } else if (s == 0 && lookup(kTimeRefs[mode], k, ph)) {
        kd = kCellTimeRef;
    } else if (isGainPos(mp, s, k)) {
        ph = gainPhase(mode, mp, s, k);
        if (isBoosted(mode, occ, k)) amp = 2.0;
        kd = kCellGainRef;
    } else {
        return false;
    }
    ref = phasor(ph, amp);
    if (kind) *kind = kd;
    return true;
}

namespace {
std::shared_ptr<Layout> build(int mode, int occ) {
    auto L = std::make_shared<Layout>();
    const ModeParams& mp = modeParams(mode);
    L->mode = mode; L->occ = occ; L->ns = mp.ns; L->frames = mp.frames;
    carrierRange(mode, occ, L->kmin, L->kmax);
    const int W = L->width(), nsym = mp.ns * mp.frames;
    L->type.assign((size_t)nsym * W, kCellNone);
    L->pilots.assign((size_t)nsym, {});
    // pilots and unused carriers
    for (int gs = 0; gs < nsym; gs++) {
        const int s = gs % mp.ns, frame = gs / mp.ns;
        for (int k = L->kmin; k <= L->kmax; k++) {
            if (carrierUnused(mode, k)) continue;
            cf32 r; int kd = 0;
            if (pilotRef(mode, occ, s, k, r, &kd)) {
                L->type[(size_t)gs * W + (k - L->kmin)] = (uint8_t)kd;
                L->pilots[gs].push_back({(int16_t)k, (uint8_t)kd, r});
            } else if (mode == kModeE && ((frame == 0 && s == 4) || (frame == 3 && s == 39))) {   // clause 8.4.5: AFS reference cells (power gain 1)
                for (int i = 0; i < 54; i++) if (kAfsRefs[i][0] == k) {
                    const int ph = kAfsRefs[i][frame == 0 ? 1 : 2];
                    L->type[(size_t)gs * W + (k - L->kmin)] = kCellAfsRef;
                    L->pilots[gs].push_back({(int16_t)k, (uint8_t)kCellAfsRef, phasor(ph, 1.0)});
                }
            }
        }
        // AFS cells that coincide with gain references take the gain reference amplitude; the phases agree (checked by the tests)
    }
    // FAC: in every transmission frame
    for (int i = 0; i < kFacCells[mode].n; i++) L->facCells.push_back({(int16_t)kFacCells[mode].data[i][0], (int16_t)kFacCells[mode].data[i][1]});
    L->nFac = (int)L->facCells.size();
    for (int f = 0; f < mp.frames; f++)
        for (const auto& c : L->facCells) {
            uint8_t& t = L->type[(size_t)(f * mp.ns + c.first) * W + (c.second - L->kmin)];
            if (t == kCellNone) t = kCellFac;
        }
    // SDC and MSC: every other cell that is used, by symbol and carrier
    for (int gs = 0; gs < nsym; gs++)
        for (int k = L->kmin; k <= L->kmax; k++) {
            if (carrierUnused(mode, k)) continue;
            uint8_t& t = L->type[(size_t)gs * W + (k - L->kmin)];
            if (t == kCellFac) continue;
            if (t != kCellNone) continue;
            if (gs < mp.sdcSymbols) { t = kCellSdc; L->sdcCells.push_back({(int16_t)gs, (int16_t)k}); }
            else { t = kCellMsc; L->mscCells.push_back({(int16_t)gs, (int16_t)k}); }
        }
    L->nSdc = (int)L->sdcCells.size();
    L->nSfa = (int)L->mscCells.size();
    L->nMux = L->nSfa / mp.frames;
    return L;
}
} // namespace

std::shared_ptr<const Layout> layout(int mode, int occ) {
    if (!occupancyValid(mode, occ)) return nullptr;
    static std::mutex mu;
    static std::shared_ptr<const Layout> cache[5][6];
    std::lock_guard<std::mutex> lk(mu);
    auto& c = cache[mode][occ];
    if (!c) c = build(mode, occ);
    return c;
}

// ---- channel coding parameters

namespace {
struct RateSet { int n; Rate r[3]; int lcm; };
// Table 29 (mode E 4-QAM), Table 30 (A..D 16-QAM), Table 31 (mode E 16-QAM), Table 32 (A..D 64-QAM)
const RateSet kQam4E[4] = {{1, {{1, 4}}, 4}, {1, {{1, 3}}, 3}, {1, {{2, 5}}, 5}, {1, {{1, 2}}, 2}};
const RateSet kQam16[2] = {{2, {{1, 3}, {2, 3}}, 3}, {2, {{1, 2}, {3, 4}}, 4}};
const RateSet kQam16E[4] = {{2, {{1, 6}, {1, 2}}, 6}, {2, {{1, 4}, {4, 7}}, 28}, {2, {{1, 3}, {2, 3}}, 3}, {2, {{1, 2}, {3, 4}}, 4}};
const RateSet kQam64[4] = {{3, {{1, 4}, {1, 2}, {3, 4}}, 4}, {3, {{1, 3}, {2, 3}, {4, 5}}, 15}, {3, {{1, 2}, {3, 4}, {7, 8}}, 8}, {3, {{2, 3}, {4, 5}, {8, 9}}, 45}};

const RateSet* rateSet(int mode, int qamBits, int pl) {
    if (mode == kModeE) {
        if (qamBits == 2 && pl >= 0 && pl < 4) return &kQam4E[pl];
        if (qamBits == 4 && pl >= 0 && pl < 4) return &kQam16E[pl];
        return nullptr;
    }
    if (qamBits == 4 && pl >= 0 && pl < 2) return &kQam16[pl];
    if (qamBits == 6 && pl >= 0 && pl < 4) return &kQam64[pl];
    return nullptr;
}
} // namespace

Rate mscRate(int mode, int qamBits, int pl, int level) {
    const RateSet* r = rateSet(mode, qamBits, pl);
    if (!r || level < 0 || level >= r->n) return {1, 2};
    return r->r[level];
}
int mscRyLcm(int mode, int qamBits, int pl) { const RateSet* r = rateSet(mode, qamBits, pl); return r ? r->lcm : 0; }
int mscProtLevels(int mode, int qamBits) {
    if (mode == kModeE) return qamBits == 2 || qamBits == 4 ? 4 : 0;
    return qamBits == 4 ? 2 : qamBits == 6 ? 4 : 0;
}

int sdcQamBits(int mode, int sdcMode) { return mode == kModeE ? 2 : (sdcMode == 0 ? 4 : 2); }
int sdcLevels(int mode, int sdcMode) { return sdcQamBits(mode, sdcMode) / 2; }

int sdcDataBytes(int mode, int occ, int sdcMode) {
    auto L = layout(mode, occ);
    if (!L) return 0;
    const int n = L->nSdc;
    int bits;                                   // L_SDC of clause 7.2.1.1 with the rates of Tables 36 to 38
    if (mode == kModeE) {
        const int ry = sdcMode == 0 ? 2 : 4;
        bits = (2 * n - 12) / ry;                // one level, rate 1/ry
    } else if (sdcMode == 0) {
        const int m = (2 * n - 12) / 3;          // 16-QAM: rates 1/3 and 2/3
        bits = m + 2 * m;
    } else {
        bits = (2 * n - 12) / 2;                 // 4-QAM, rate 1/2
    }
    return (bits - 4 - 16) / 8;                  // minus the AFS index and the CRC
}

}} // namespace dect2::drm
