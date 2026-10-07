// DMR link layer of the receiver, see dmr_link.h.
#include "dmr_link.h"
#include "dect2/dmr_dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace dmr {

namespace {

constexpr double kSlotSamples = 1440.0;
constexpr double kDecodeLag = 1316.0;             // a burst can be read this many samples after its first symbol
constexpr double kNominalPeriod = 2880.0;
constexpr float kCorrThreshold = 0.70f;
constexpr int kMaxSyncDiff = 4;
constexpr double kMaxOffsetHz = 10000.0;          // a signal further from the tuned frequency than this is the neighbouring channel, not ours

const uint8_t kIdleInfo[12] = {0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91};

int familyOf(int sync) { return syncIsBs(sync) ? 0 : syncIsDirect(sync) ? 2 : 1; }

const char* voiceName(int pos) {
    static const char* n[6] = {"Voice A", "Voice B", "Voice C", "Voice D", "Voice E", "Voice F"};
    return pos >= 0 && pos < 6 ? n[pos] : "Voice";
}

const char* activityName(int a) {
    switch (a) {
    case 0: return "none";
    case 2: return "group CSBK";
    case 3: return "individual CSBK";
    case 8: return "group voice";
    case 9: return "individual voice";
    case 10: return "individual data";
    case 11: return "group data";
    case 12: return "emergency group voice";
    case 13: return "emergency individual voice";
    default: return "reserved";
    }
}

std::string idText(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "%u", v);
    return b;
}

} // namespace

Link::Link() {
    // templates of the voice sync of each family (the data syncs are their complement)
    const int pv[5] = {kSyncBsVoice, kSyncMsVoice, kSyncRc, kSyncDm1Voice, kSyncDm2Voice};
    const int pd[5] = {kSyncBsData, kSyncMsData, -1, kSyncDm1Data, kSyncDm2Data};
    for (int p = 0; p < 5; p++) {
        patVoice_[p] = pv[p];
        patData_[p] = pd[p];
        int8_t s[24];
        syncSymbols(pv[p], s);
        for (int j = 0; j < 24; j++) tmpl_[p][j] = s[j] > 0 ? 1.f : -1.f;
    }
    reset();
}

void Link::reset() {
    uint64_t seq = tel_.seq;
    memset(ring_, 0, sizeof ring_);
    n_ = 0;
    memset(rho_, 0, sizeof rho_);
    for (auto& t : tr_) t = Track();
    globalCal_ = Cal();
    ncoDelta_ = 0;
    nco_ = 0;
    tel_ = DmrTelemetry();
    tel_.seq = seq;
    eye_.clear();
    cc_ = -1;
    linkFamily_ = -1;
    snrEma_ = 0; snrValid_ = false;
    berNum_ = berDen_ = 0;
    lastLockSample_ = 0;
    lastSlot_[0] = DmrSlot(); lastSlot_[1] = DmrSlot();
    cachInfo_.clear();
    shortLcHave_ = 0;
    symPpm_ = 0;
    devHz_ = 1944;
}

float Link::interp(double pos) const {
    const int64_t i = (int64_t)std::floor(pos);
    const float t = (float)(pos - (double)i);
    const float a = at((uint64_t)(i - 1)), b = at((uint64_t)i), c = at((uint64_t)(i + 1)), d = at((uint64_t)(i + 2));
    // Catmull-Rom spline through b and c
    return b + 0.5f * t * (c - a + t * (2.f * a - 5.f * b + 4.f * c - d + t * (3.f * (b - c) + d - a)));
}

double Link::cfoResidualHz() const {
    for (const Track& t : tr_) if (t.used && t.confirmed && t.cal.valid) return t.cal.off;
    return globalCal_.valid ? globalCal_.off : 0;      // the last measurement, kept in step with the oscillator
}

bool Link::locked() const {
    for (const Track& t : tr_) if (t.used && t.confirmed) return true;
    return false;
}

void Link::shiftOffset(double hz) {
    nco_ += hz;
    for (Track& t : tr_) if (t.used && t.cal.valid) t.cal.off -= hz;
    if (globalCal_.valid) globalCal_.off -= hz;
}

// ---------------------------------------------------------------------------------------------------- sync search

void Link::push(float y) {
    ring_[n_ & kMask] = y;
    n_++;
    if (n_ >= 244) searchSync();
    runTracks();
}

void Link::searchSync() {
    const uint64_t e = n_ - 1;
    float w[24];
    float sum = 0;
    for (int j = 0; j < 24; j++) { w[j] = at(e - 230 + (uint64_t)(10 * j)); sum += w[j]; }
    const float m = sum / 24.f;
    float q = 0;
    for (int j = 0; j < 24; j++) { const float d = w[j] - m; q += d * d; }
    for (int p = 0; p < 5; p++) {
        float rho = 0;
        if (q > 1e-3f) {
            float c = 0, s = 0;
            for (int j = 0; j < 24; j++) { c += tmpl_[p][j] * w[j]; s += tmpl_[p][j]; }
            const float cov = c - m * s, dt = 24.f - s * s / 24.f;
            rho = cov / std::sqrt(q * dt);
        }
        float* h = rho_[p];
        h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = h[4]; h[4] = rho;
        // the value two samples back (h[2]) is the middle of five: look for a peak of |rho| there, so that the samples needed to interpolate exist
        const float prev = std::fabs(h[1]), mid = std::fabs(h[2]), next = std::fabs(h[3]);
        if (mid >= kCorrThreshold && mid >= prev && mid > next) {
            const float denom = prev - 2 * mid + next;
            double delta = denom < -1e-6f ? 0.5 * (prev - next) / denom : 0.0;
            delta = std::max(-0.5, std::min(0.5, delta));
            Cand c;
            c.pat = p;
            c.voice = h[2] > 0;
            c.pos = (double)(e - 2) + delta;       // the position of the last sync symbol
            c.rho = h[2];
            if (!c.voice && patData_[p] < 0) continue;
            onCandidate(c);
        }
    }
}

bool Link::verifySync(int sync, double last, double& A, double& b, int& diff) const {
    int8_t t[24];
    syncSymbols(sync, t);
    float v[24];
    double mv = 0, mt = 0;
    for (int j = 0; j < 24; j++) { v[j] = interp(last - 230.0 + 10.0 * j); mv += v[j]; mt += t[j] > 0 ? 1.0 : -1.0; }
    mv /= 24; mt /= 24;
    double cov = 0, vt = 0;
    for (int j = 0; j < 24; j++) { const double tj = (t[j] > 0 ? 1.0 : -1.0) - mt; cov += tj * (v[j] - mv); vt += tj * tj; }
    if (vt < 1e-9) return false;
    A = cov / vt;
    b = mv - A * mt;
    if (A < 0.45 * 1944.0 || A > 1.8 * 1944.0) return false;
    diff = 0;
    const double unit = A / 3.0;
    for (int j = 0; j < 24; j++) {
        const double z = (v[j] - b) / unit;
        const int hard = z >= 2 ? 3 : z >= 0 ? 1 : z >= -2 ? -1 : -3;
        if (hard != t[j]) diff++;
    }
    return diff <= kMaxSyncDiff;
}

void Link::onCandidate(const Cand& c) {
    const int sync = c.voice ? patVoice_[c.pat] : patData_[c.pat];
    double A = 0, b = 0;
    int diff = 0;
    static const bool dbg = getenv("DMR_DEBUG") != nullptr;
    if (!verifySync(sync, c.pos, A, b, diff)) {
        if (dbg) fprintf(stderr, "[%.3f] candidate %s rho %.2f REJECTED (A %.0f diff %d)\n", c.pos / 48000.0, syncName(sync), c.rho, A, diff);
        return;
    }
    if (std::fabs(nco_ + b) > kMaxOffsetHz) return;
    if (dbg) fprintf(stderr, "[%.3f] sync %s rho %.2f A %.0f b %.0f diff %d\n", c.pos / 48000.0, syncName(sync), c.rho, A, b, diff);
    const double t0 = c.pos - 230.0 - 540.0;       // the burst starts 77 symbols before the last sync symbol
    const int fam = familyOf(sync);
    Cal cal;
    cal.off = b; cal.unit = A / 3.0; cal.valid = true;
    const double ar = std::fabs(c.rho);
    Track* t = findTrack(t0, 25.0);
    if (t) {
        // a pattern of another family at the place of a locked track is a false alarm
        if (t->confirmed && t->family >= 0 && fam != t->family && sync != kSyncRc) return;
        tel_.syncCount[sync]++;
        if (t->lastAnchor >= 0) {
            const double span = t0 - t->lastAnchor;
            const double m = std::floor(span / t->period + 0.5);
            if (m >= 1 && m <= 12) {
                const double pnew = span / m;
                t->period += 0.25 * (pnew - t->period);
                t->period = std::max(kNominalPeriod * (1 - 4e-4), std::min(kNominalPeriod * (1 + 4e-4), t->period));
            }
        }
        t->lastAnchor = t0;
        t->nextT0 = t0;
        if (t->cal.valid) { t->cal.off += 0.4 * (cal.off - t->cal.off); t->cal.unit += 0.4 * (cal.unit - t->cal.unit); }
        else t->cal = cal;
        t->family = fam;
        if (syncIsDirect(sync)) t->slot = syncDirectSlot(sync);
        return;
    }
    // a new transmission needs a clean sync: a pattern that only just passes is as likely to be noise
    const bool strong = ar >= 0.85 && diff <= 3 && (sync != kSyncRc || (ar >= 0.92 && diff <= 2));
    if (!strong) return;
    // inside a burst of a confirmed track: not a new transmission
    for (const Track& o : tr_) {
        if (!o.used || !o.confirmed) continue;
        double d = std::fmod(t0 - o.nextT0, o.period);
        if (d > o.period / 2) d -= o.period;
        if (d < -o.period / 2) d += o.period;
        if (std::fabs(d) < 1200.0) return;
    }
    // a tentative track that has not decoded anything gives way to a cleaner sync
    for (Track& o : tr_) {
        if (!o.used) continue;
        double d = std::fmod(t0 - o.nextT0, o.period);
        if (d > o.period / 2) d -= o.period;
        if (d < -o.period / 2) d += o.period;
        if (std::fabs(d) < 300.0) {
            if (!o.confirmed && o.hits == 0 && ar > o.birthRho + 0.05) o = Track();
            else return;
        }
    }
    tel_.syncCount[sync]++;
    Track* nt = newTrack(fam, t0, cal, false);
    if (nt) {
        nt->lastAnchor = t0;
        nt->birthRho = ar;
        if (syncIsDirect(sync)) nt->slot = syncDirectSlot(sync);
    }
}

Link::Track* Link::findTrack(double t0, double tol) {
    Track* best = nullptr;
    double bd = tol;
    for (auto& t : tr_) {
        if (!t.used) continue;
        const double d = std::fabs(t0 - t.nextT0);
        if (d <= bd) { bd = d; best = &t; }
    }
    return best;
}

Link::Track* Link::newTrack(int family, double t0, const Cal& cal, bool probe) {
    Track* slot = nullptr;
    for (auto& t : tr_) if (!t.used) { slot = &t; break; }
    if (!slot) {
        // replace a tentative track, else the one that has not been seen for the longest time
        double worst = -1;
        for (auto& t : tr_) {
            const double score = t.confirmed ? (double)t.misses : 1000.0 + t.misses;
            if (score > worst) { worst = score; slot = &t; }
        }
        if (slot->confirmed && slot->misses < 2) return nullptr;
        dropTrack(*slot, "replaced");
    }
    *slot = Track();
    slot->used = true;
    slot->family = family;
    slot->nextT0 = t0;
    slot->cal = cal;
    slot->probe = probe;
    slot->cc = cc_;
    slot->lastSeenSec = nowSec();
    return slot;
}

void Link::dropTrack(Track& t, const char* why) {
    (void)why;
    if (!t.used) return;
    if (t.inCall) endCall(t, false);
    if (t.msg.active) { t.msg.active = false; }
    if (t.confirmed && t.slot >= 1 && t.slot <= 2) {
        lastSlot_[t.slot - 1] = DmrSlot();
        lastSlot_[t.slot - 1].active = false;
        lastSlot_[t.slot - 1].lastBurst = t.lastBurst;
    }
    if (t.confirmed) say("slot " + std::to_string(t.slot ? t.slot : 1) + " lost");
    t = Track();
}

void Link::runTracks() {
    for (auto& t : tr_) {
        while (t.used && (double)n_ >= t.nextT0 + kDecodeLag) decodeBurst(t);
    }
}

// ---------------------------------------------------------------------------------------------------- bursts

namespace {

// reliability of the two bits of every dibit of the burst: the sign bit is as reliable as the distance from zero, the second bit as the distance from the
// outer thresholds at +-2
inline float bitRel(float z, int which) { return which == 0 ? std::fabs(z) : std::fabs(std::fabs(z) - 2.f); }

void payloadReliability(const float* z, float rel[196]) {
    for (int i = 0; i < 49; i++) { rel[2 * i] = bitRel(z[i], 0); rel[2 * i + 1] = bitRel(z[i], 1); }
    for (int i = 0; i < 49; i++) { rel[98 + 2 * i] = bitRel(z[83 + i], 0); rel[98 + 2 * i + 1] = bitRel(z[83 + i], 1); }
}

} // namespace

void Link::noteFec(Track& t, double errors, double bits, bool toCall) {
    if (toCall && t.inCall) t.call.fecErrors += (int)std::lround(errors);
    t.berNum = t.berNum * 0.995 + errors;
    t.berDen = t.berDen * 0.995 + bits;
    berNum_ = berNum_ * 0.995 + errors;
    berDen_ = berDen_ * 0.995 + bits;
}

namespace {

struct Validator {
    int kind;           // 0 none, 1 CRC-CCITT with mask, 2 RS header, 3 RS terminator, 4 idle pattern
    uint16_t mask = 0;
    bool operator()(const Bits& info) const {
        uint8_t d[10], lc[9];
        switch (kind) {
        case 1: return crcBlockCheck(info, mask, d);
        case 2: return lcBurstDecode(info, false, lc) >= 0;
        case 3: return lcBurstDecode(info, true, lc) >= 0;
        case 4: { uint8_t o[12]; bitsToBytes(info, 0, 96, o); return !memcmp(o, kIdleInfo, 12); }
        default: return true;
        }
    }
};

} // namespace

// returns the number of corrected bits, -1 for failure; flips the least reliable payload bits if the validator is not satisfied
static int bptcAssisted(const Bits& burst, const float* z, const Validator& val, Bits& info, bool& fixedByAssist) {
    Bits pay;
    burstDataPayload(burst, pay);
    fixedByAssist = false;
    int c = bptc196Decode(pay, info);
    if (c >= 0 && (val.kind == 0 || val(info))) return c;
    if (val.kind == 0) return c;
    float rel[196];
    payloadReliability(z, rel);
    int idx[6];
    std::vector<int> order(196);
    for (int i = 0; i < 196; i++) order[(size_t)i] = i;
    std::partial_sort(order.begin(), order.begin() + 6, order.end(), [&](int a, int b) { return rel[a] < rel[b]; });
    for (int i = 0; i < 6; i++) idx[i] = order[(size_t)i];
    for (unsigned mask = 1; mask < 64; mask++) {
        Bits p2 = pay;
        for (int i = 0; i < 6; i++) if (mask & (1u << i)) p2[(size_t)idx[i]] ^= 1;
        Bits inf2;
        const int c2 = bptc196Decode(p2, inf2);
        if (c2 >= 0 && val(inf2)) {
            info = inf2;
            fixedByAssist = true;
            return c2 + __builtin_popcount(mask);
        }
    }
    return -1;
}

void Link::decodeBurst(Track& t) {
    const double t0 = t.nextT0;
    const double now = nowSec();
    float v[132], z[132];
    int8_t h[132];
    Cal c = t.cal.valid ? t.cal : (globalCal_.valid ? globalCal_ : Cal{0, kDevUnit, true});
    for (int j = 0; j < 132; j++) {
        v[j] = interp(t0 + 10.0 * j);
        z[j] = (float)((v[j] - c.off) / c.unit);
        h[j] = (int8_t)(z[j] >= 2.f ? 3 : z[j] >= 0.f ? 1 : z[j] >= -2.f ? -1 : -3);
    }
    Bits burst;
    burst.reserve(264);
    for (int j = 0; j < 132; j++) putBits(burst, symbolToDibit(h[j]), 2);

    Outcome out;
    int sync = matchSync(burstCentre(burst), t.confirmed ? 5 : 3, nullptr);
    if (sync >= 0 && t.confirmed && t.family >= 0 && familyOf(sync) != t.family && sync != kSyncRc) sync = -1;
    int cc = t.cc;

    // CACH in front of the burst (base station)
    if (t.family == 0 && (sync < 0 || syncIsBs(sync))) {
        float zc[12];
        for (int i = 0; i < 12; i++) zc[i] = (float)((interp(t0 - 120.0 + 10.0 * i) - c.off) / c.unit);
        handleCach(t, zc);
    }

    bool structure = false, payloadOk = false;
    bool dataBurst = false;
    int dt = -1;
    bool dataLike = sync >= 0 && syncIsData(sync);
    if (!dataLike && sync < 0 && t.family == 0 && t.confirmed && t.cc >= 0) {
        // A base station need not put a sync pattern into a voice header (TS 102 361-1 clause 4.3): the centre field then holds embedded signalling, as
        // in a voice burst. Such a burst is taken for a header only if its slot type says so, with the colour code of the channel, and the link control
        // passes its Reed-Solomon check; a voice burst fails that test.
        const SlotTypeInfo st = burstSlotType(burst);
        if (st.ok && st.errors <= 2 && st.cc == t.cc && st.dt == kDtVoiceLcHeader) {
            Bits info;
            bool fixedByAssist = false;
            dataLike = bptcAssisted(burst, z, Validator{2}, info, fixedByAssist) >= 0;
        }
    }
    if (!dataLike && sync < 0 && t.family == 0 && t.confirmed && t.cc >= 0 && t.msg.active) {
        // In the middle of a data message a burst without sync pattern is a block with reverse channel signalling in its centre (clause 5.1.5): the
        // slot type must still be a data block of this message and carry the colour code. The message CRC decides in the end whether the block was right.
        const SlotTypeInfo st = burstSlotType(burst);
        const bool kindOk = t.msg.got == 0 ? (st.dt == kDtRate12 || st.dt == kDtRate34 || st.dt == kDtRate1) : st.dt == t.msg.dt;
        dataLike = st.ok && st.errors <= 2 && st.cc == t.cc && kindOk;
    }
    if (dataLike) {
        const SlotTypeInfo st = burstSlotType(burst);
        if (st.ok && st.errors <= (t.confirmed ? 3 : 1) && (t.cc < 0 || t.cc == st.cc || !t.confirmed)) {
            structure = true;
            dataBurst = true;
            dt = st.dt;
            cc = st.cc;
            if (st.errors > 0) tel_.golayFixed++;
            tel_.blocksOk++;
            noteFec(t, st.errors, 20);
            // the colour code belongs in the log entry this burst may create; it stays only if the payload checks out
            const int prevCc = t.cc;
            if (t.cc < 0) t.cc = st.cc;
            Outcome o = decodeData(t, burst, z, sync, st.cc, st.dt);
            payloadOk = o.valid;
            if (!payloadOk) t.cc = prevCc;
            t.voiceRun = 0;
        } else {
            tel_.golayFail++;
            tel_.blocksBad++;
            t.lastBurst = "bad slot type";
        }
    } else if (sync == kSyncRc) {
        structure = true; payloadOk = true;
        tel_.rcBursts++;
        t.lastBurst = "RC";
    } else if (sync >= 0 && syncIsVoice(sync)) {
        // burst A: the voice sync; no field protects the burst, but a locked track expects it
        structure = true; payloadOk = true;
        voiceBurst(t, 0, burst, 0, Bits(), true);
        if (t.slot == 0 && syncIsDirect(sync)) t.slot = syncDirectSlot(sync);
    } else {
        const EmbInfo emb = burstEmb(burst);
        const bool ccOk = t.cc < 0 || emb.cc == t.cc;
        if (emb.ok && emb.errors <= (t.confirmed ? 2 : 1) && ccOk && (t.vpos >= 0 || t.inCall)) {
            structure = true; payloadOk = true;
            cc = emb.cc;
            tel_.embOk++;
            tel_.blocksOk++;
            noteFec(t, emb.errors, 16);
            int pos = t.vpos >= 0 ? (t.vpos + 1) % 6 : -1;
            if (pos < 0) pos = emb.lcss == 1 ? 1 : emb.lcss == 2 ? 4 : emb.lcss == 0 ? 5 : 2;
            voiceBurst(t, pos, burst, emb.lcss, emb.emb32, false);
        } else if (t.confirmed) {
            // silence after the end of a transmission is not an error of the channel; a burst missing inside a call is
            tel_.unknownBursts++;
            if (t.inCall || t.msg.active) { tel_.embFail++; tel_.blocksBad++; if (t.inCall) t.call.fecErrors++; }
            t.lastBurst = "unknown";
        }
    }

    // statistics of the symbols, calibration
    if (structure && payloadOk) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, mse = 0;
        for (int j = 0; j < 132; j++) {
            const double x = h[j], yv = v[j];
            sx += x; sy += yv; sxx += x * x; sxy += x * yv;
            const double e = z[j] - h[j];
            mse += e * e;
        }
        mse /= 132.0;
        const double den = 132.0 * sxx - sx * sx;
        if (den > 1e-6) {
            const double unit = (132.0 * sxy - sx * sy) / den, off = (sy - unit * sx) / 132.0;
            if (unit > 0.55 * kDevUnit && unit < 1.7 * kDevUnit) {
                const double a = t.confirmed ? 0.15 : 0.3;
                t.cal.off += a * (off - t.cal.off);
                t.cal.unit += a * (unit - t.cal.unit);
                t.cal.valid = true;
                globalCal_ = t.cal;
                devHz_ = (float)(3.0 * t.cal.unit);
            }
        }
        // the eye: the symbols as they are decided
        for (int j = 0; j < 132; j++) {
            eye_.push_back(z[j]);
            const int bin = std::max(0, std::min(39, (int)std::floor((z[j] + 5.f) / 0.25f)));
            tel_.levelHist[bin]++;
            tel_.levelCount[h[j] == -3 ? 0 : h[j] == -1 ? 1 : h[j] == 1 ? 2 : 3]++;
        }
        if (eye_.size() > 1600) eye_.erase(eye_.begin(), eye_.begin() + (eye_.size() - 800));
        t.errEma += 0.2 * (std::sqrt(mse) / 2.0 - t.errEma);
        snrEma_ = snrValid_ ? snrEma_ + 0.1 * (mse - snrEma_) : mse;
        snrValid_ = true;
        // keep the demodulator's offset small so that the channel filter stays centred
        if (t.confirmed && std::fabs(t.cal.off) > 150.0) {
            const double step = std::max(-200.0, std::min(200.0, t.cal.off));
            ncoDelta_ += step;
            nco_ += step;
            for (auto& o : tr_) if (o.used && o.cal.valid) o.cal.off -= step;
            globalCal_.off -= step;
        }
    }

    // track bookkeeping
    t.lastBurst = t.lastBurst.empty() ? "?" : t.lastBurst;
    if (structure && payloadOk) {
        t.hits++;
        t.misses = 0;
        t.lastSeenSec = now;
        if (cc >= 0) { t.cc = cc; if (cc_ != cc) { cc_ = cc; say("colour code " + std::to_string(cc)); } }
        t.bursts++;
        lastLockSample_ = n_;
        if (!t.confirmed) {
            // a data burst whose payload passed its own check is enough; voice needs company
            if (dataBurst || t.hits >= 3) {
                t.confirmed = true;
                if (t.slot == 0) {      // provisional: the first track is slot 1; the CACH of a base station corrects it
                    bool used[3] = {false, false, false};
                    for (const Track& o : tr_) if (o.used && &o != &t && o.confirmed && o.slot >= 1 && o.slot <= 2) used[o.slot] = true;
                    t.slot = !used[1] ? 1 : 2;
                }
                const char* fam = t.family == 0 ? "base station" : t.family == 1 ? "mobile" : "direct mode";
                if (linkFamily_ != t.family) { linkFamily_ = t.family; say(std::string("signal: ") + fam + ", colour code " + (t.cc >= 0 ? std::to_string(t.cc) : "?")); }
                else say(std::string("slot ") + std::to_string(t.slot ? t.slot : 1) + " locked");
                // the other slot of a base station follows 30 ms later
                if (t.family == 0) {
                    bool have = false;
                    const double want = t0 + kSlotSamples;
                    for (const Track& o : tr_) {
                        if (!o.used || &o == &t) continue;
                        double d = std::fmod(o.nextT0 - want, kNominalPeriod);
                        if (d > kNominalPeriod / 2) d -= kNominalPeriod;
                        if (d < -kNominalPeriod / 2) d += kNominalPeriod;
                        if (std::fabs(d) < 400) have = true;
                    }
                    if (!have) {
                        Track* nt = newTrack(0, t0 + kSlotSamples, t.cal, true);
                        if (nt) { nt->period = t.period; nt->cc = t.cc; nt->lastAnchor = -1; }
                    }
                }
            }
        }
    } else {
        t.misses++;
        t.voiceRun = 0;
        if (t.vpos >= 0) { t.vpos = (t.vpos + 1) % 6; if (t.misses >= 2) t.vpos = -1; }
    }
    const int missLimit = t.confirmed ? (t.family == 0 ? 10 : 5) : 2;
    t.nextT0 = t0 + t.period;
    if (t.misses >= missLimit) {
        dropTrack(t, "silent");
        return;
    }
    // a probe for the second slot that never shows anything is dropped quietly
    if (t.probe && !t.confirmed && t.misses >= 3) { t = Track(); return; }

    // the symbol clock: how far the measured burst period is from the nominal 60 ms
    symPpm_ = (t.period / kNominalPeriod - 1.0) * 1e6;
    t.errEma = std::max(0.0, t.errEma);
    (void)dt;
}

// ---------------------------------------------------------------------------------------------------- data bursts

Link::Outcome Link::decodeData(Track& t, const Bits& burst, const float* z, int sync, int cc, int dt) {
    Outcome o;
    o.data = true;
    o.dt = dt;
    (void)cc;
    tel_.burstCount[dt & 15]++;
    t.lastBurst = dataTypeName(dt);
    const double now = nowSec();
    Bits info;
    bool fixedByAssist = false;
    if (sync >= 0 && !t.slot && syncIsDirect(sync)) t.slot = syncDirectSlot(sync);

    auto countBptc = [&](int corrected) {
        if (corrected < 0) { tel_.bptcFail++; tel_.blocksBad++; if (t.inCall) t.call.fecErrors++; }
        else if (corrected == 0) { tel_.bptcOk++; tel_.blocksOk++; }
        else { tel_.bptcFixed++; tel_.blocksOk++; noteFec(t, corrected, 196); }
    };
    auto endVoiceByData = [&]() {
        if (t.inCall) endCall(t, false);
        t.vpos = -1;
    };

    switch (dt) {
    case kDtVoiceLcHeader:
    case kDtTerminatorLc: {
        const bool term = dt == kDtTerminatorLc;
        Validator val{term ? 3 : 2};
        const int cor = bptcAssisted(burst, z, val, info, fixedByAssist);
        countBptc(cor);
        t.state = 1;
        if (cor < 0) { t.lastBurst += " (lost)"; break; }
        uint8_t lc[9];
        const int rs = lcBurstDecode(info, term, lc);
        if (rs == 0) tel_.rsOk++; else tel_.rsFixed++;
        FullLc f;
        parseFullLc(lc, f);
        o.valid = true;
        int kind = f.flco == kFlcoUnitVoice ? 1 : (f.dst >= 0xFFFFF0 ? 2 : 0);
        if (f.flco != kFlcoGroupVoice && f.flco != kFlcoUnitVoice) { t.lastBurst += " (other LC)"; break; }
        if (!term) {
            if (t.inCall && t.call.idsKnown && t.call.src == f.src && t.call.dst == f.dst) { /* repeated header */ }
            else startCall(t, kind, f.src, f.dst, f.svc, false, true);
            t.vpos = -1;
            t.sinceVoice = 0;
        } else {
            // In hang time a base station repeats the terminator for seconds to keep the channel reserved (TS 102 361-2 clause 5.2): one log entry per
            // call, whose end moves with the last repeat.
            const uint64_t key = (uint64_t)f.src << 32 | (uint64_t)f.dst << 4 | (uint64_t)kind;
            if (!t.inCall && key == t.lastTermKey && now - t.lastTermSec < 8.0) {
                for (size_t i = tel_.callLog.size(); i-- > 0;) {
                    DmrCall& e = tel_.callLog[i];
                    if (e.kind <= 2 && e.slot == slotOf(t) && e.src == f.src && e.dst == f.dst && e.terminated) { e.endSec = now; break; }
                }
                t.lastTermSec = now;
                t.vpos = -1;
                break;
            }
            if (!t.inCall) startCall(t, kind, f.src, f.dst, f.svc, true, true);
            else if (!t.call.idsKnown) { t.call.src = f.src; t.call.dst = f.dst; t.call.idsKnown = true; t.call.kind = kind; }
            endCall(t, true);
            t.lastTermKey = key;
            t.lastTermSec = now;
            t.vpos = -1;
        }
        break;
    }
    case kDtCsbk: {
        const int cor = bptcAssisted(burst, z, Validator{1, kMaskCsbk}, info, fixedByAssist);
        t.state = 4;
        countBptc(cor);
        Csbk c;
        if (cor >= 0 && csbkParse(info, c)) {
            tel_.crcOk++;
            o.valid = true;
            endVoiceByData();
            const uint64_t key = (uint64_t)c.opcode << 56 | (uint64_t)c.fid << 48 | (uint64_t)be24(c.data + 2) << 24 | be24(c.data + 5);
            if (!(key == t.lastCsbkKey && now - t.lastCsbkSec < 3.0)) {
                DmrCall e;
                e.slot = slotOf(t); e.startSec = e.endSec = now; e.cc = t.cc; e.kind = 4;
                e.dst = be24(c.data + 2); e.src = be24(c.data + 5);
                e.idsKnown = c.opcode == 0x3D || c.opcode == 0x04 || c.opcode == 0x38 || c.opcode == 0x05;
                char nb[96];
                snprintf(nb, sizeof nb, "%s (opcode %02X%s)", csbkName(c.opcode, c.fid), c.opcode, c.fid ? ", manufacturer" : "");
                e.note = nb;
                e.terminated = true;
                addLog(e);
                tel_.calls++;
                t.lastCsbkKey = key;
                t.lastCsbkSec = now;
            }
            t.lastBurst = std::string("CSBK ") + csbkName(c.opcode, c.fid);
        } else {
            tel_.crcBad++;
            t.lastBurst += " (CRC)";
        }
        break;
    }
    case kDtPiHeader:
    case kDtMbcHeader:
    case kDtUsbd: {
        const uint16_t mask = dt == kDtPiHeader ? kMaskPi : dt == kDtMbcHeader ? kMaskMbcHeader : kMaskUsbd;
        const int cor = bptcAssisted(burst, z, Validator{1, mask}, info, fixedByAssist);
        countBptc(cor);
        uint8_t d[10];
        if (cor >= 0 && crcBlockCheck(info, mask, d)) { tel_.crcOk++; o.valid = true; }
        else { tel_.crcBad++; t.lastBurst += " (CRC)"; }
        break;
    }
    case kDtMbcContinuation: {
        const int cor = bptcAssisted(burst, z, Validator{0}, info, fixedByAssist);
        countBptc(cor);
        o.valid = cor >= 0;
        break;
    }
    case kDtIdle: {
        const int cor = bptcAssisted(burst, z, Validator{4}, info, fixedByAssist);
        countBptc(cor);
        t.state = 3;
        if (cor >= 0) {
            uint8_t b[12];
            bitsToBytes(info, 0, 96, b);
            if (!memcmp(b, kIdleInfo, 12)) { tel_.idleOk++; o.valid = true; }
        }
        endVoiceByData();
        if (t.msg.active) { t.msg.active = false; }
        break;
    }
    case kDtDataHeader: {
        const int cor = bptcAssisted(burst, z, Validator{1, kMaskDataHeader}, info, fixedByAssist);
        countBptc(cor);
        t.state = 2;
        uint8_t d[10];
        DataHeader hd;
        if (cor >= 0 && crcBlockCheck(info, kMaskDataHeader, d) && parseDataHeader(d, hd)) {
            tel_.crcOk++;
            o.valid = true;
            endVoiceByData();
            Assembler& m = t.msg;
            m = Assembler();
            m.hdr = hd;
            m.startSec = now;
            const bool blocks = hd.blocks > 0 && (hd.dpf == kDpfUnconfirmed || hd.dpf == kDpfConfirmed || hd.dpf == kDpfShortDefined || hd.dpf == kDpfShortRaw ||
                                                   hd.dpf == kDpfResponse || hd.dpf == kDpfUdt);
            m.confirmed = hd.dpf == kDpfConfirmed;
            if (blocks) { m.active = true; }
            else if (hd.dpf == kDpfShortRaw && hd.blocks == 0) {
                // status / precoded short data: no blocks follow
                DmrMessage msg;
                msg.sec = now; msg.slot = slotOf(t); msg.src = hd.src; msg.dst = hd.dst; msg.group = hd.group; msg.crcOk = true;
                msg.format = "status / precoded";
                msg.text = "status " + std::to_string(hd.status);
                addMessage(msg);
                DmrCall e;
                e.slot = slotOf(t); e.startSec = e.endSec = now; e.cc = t.cc; e.kind = 3; e.src = hd.src; e.dst = hd.dst; e.idsKnown = true; e.note = msg.text;
                e.terminated = true;
                addLog(e); tel_.calls++;
            }
            t.lastBurst = std::string("Data header: ") + dpfName(hd.dpf);
        } else {
            tel_.crcBad++;
            t.lastBurst += " (CRC)";
        }
        break;
    }
    case kDtRate12:
    case kDtRate34:
    case kDtRate1: {
        t.state = 2;
        uint8_t oct[24];
        bool okBlock = false;
        bool needCrc9 = t.msg.active && t.msg.confirmed;
        if (dt == kDtRate12) {
            Validator val = needCrc9 ? Validator{0} : Validator{0};
            int cor = bptcAssisted(burst, z, val, info, fixedByAssist);
            countBptc(cor);
            if (cor >= 0) { bitsToBytes(info, 0, 96, oct); okBlock = true; }
        } else if (dt == kDtRate34) {
            float sym[98];
            for (int i = 0; i < 49; i++) { sym[i] = z[i]; sym[49 + i] = z[83 + i]; }
            const int bad = trellis34Decode(sym, oct);
            if (bad < 0) { tel_.trellisFail++; tel_.blocksBad++; if (t.inCall) t.call.fecErrors++; }
            else { tel_.trellisOk++; tel_.blocksOk++; okBlock = true; noteFec(t, bad, 196); }
        } else {
            Bits pay;
            burstDataPayload(burst, pay);
            rate1Decode(pay, oct);
            okBlock = true;
        }
        if (!okBlock) break;
        Assembler& m = t.msg;
        if (!m.active) { o.valid = true; break; }        // a block without its header: counted, nothing to attach it to
        if (m.got == 0) m.dt = dt;
        if (dt != m.dt) { m.active = false; break; }
        DataBlock blk;
        dataBlockParse(dt, oct, m.confirmed, blk);
        if (m.confirmed && !blk.crcOk) { tel_.crcBad++; m.crcAllOk = false; m.bad++; }
        m.bytes.insert(m.bytes.end(), blk.bytes.begin(), blk.bytes.end());
        m.got++;
        o.valid = true;
        t.lastBurst = std::string(dataTypeName(dt)) + " " + std::to_string(m.got) + "/" + std::to_string(m.hdr.blocks);
        if (m.got >= m.hdr.blocks) finishMessage(t);
        break;
    }
    default:
        break;
    }
    return o;
}

void Link::finishMessage(Track& t) {
    Assembler& m = t.msg;
    m.active = false;
    const double now = nowSec();
    DmrMessage msg;
    msg.sec = now; msg.slot = slotOf(t); msg.src = m.hdr.src; msg.dst = m.hdr.dst; msg.group = m.hdr.group;
    const size_t total = m.bytes.size();
    bool crcOk = false;
    std::vector<uint8_t> data;
    if (total >= 4) {
        const size_t body = total - 4;
        const uint32_t calc = crc32Msg(m.bytes.data(), body);
        const uint32_t rx = (uint32_t)m.bytes[body] | (uint32_t)m.bytes[body + 1] << 8 | (uint32_t)m.bytes[body + 2] << 16 | (uint32_t)m.bytes[body + 3] << 24;
        crcOk = calc == rx && m.crcAllOk;
        size_t padBytes = 0;
        if (m.hdr.dpf == kDpfShortDefined || (m.hdr.dpf == kDpfShortRaw && m.hdr.blocks > 0)) padBytes = (size_t)m.hdr.pad / 8;
        else padBytes = (size_t)m.hdr.pad;
        padBytes = std::min(padBytes, body);
        data.assign(m.bytes.begin(), m.bytes.begin() + (ptrdiff_t)(body - padBytes));
    }
    msg.crcOk = crcOk;
    if (crcOk) tel_.crcOk++; else tel_.crcBad++;
    char fb[96];
    switch (m.hdr.dpf) {
    case kDpfShortDefined:
        snprintf(fb, sizeof fb, "short data, %s", ddName(m.hdr.dd));
        msg.format = fb;
        msg.text = textFromBytes(data.data(), data.size(), m.hdr.dd, false);
        break;
    case kDpfShortRaw:
        msg.format = "short data, raw";
        msg.text = textFromBytes(data.data(), data.size(), 0, false);
        break;
    default: {
        snprintf(fb, sizeof fb, "%s, %s", dpfName(m.hdr.dpf), sapName(m.hdr.sap));
        msg.format = fb;
        snprintf(fb, sizeof fb, "%zu bytes", data.size());
        msg.text = fb;
        bool printable = !data.empty();
        for (uint8_t c : data) if (c < 0x20 || c > 0x7E) printable = false;
        if (printable) msg.text = std::string((const char*)data.data(), data.size());
        break;
    }
    }
    addMessage(msg);
    DmrCall e;
    e.slot = slotOf(t); e.startSec = m.startSec; e.endSec = now; e.cc = t.cc; e.kind = 3;
    e.src = m.hdr.src; e.dst = m.hdr.dst; e.idsKnown = true;
    e.fecErrors = crcOk ? 0 : 1;
    e.terminated = crcOk;
    e.note = msg.format + (crcOk ? "" : " (CRC wrong)");
    addLog(e);
    tel_.calls++;
    say("slot " + std::to_string(slotOf(t)) + ": " + msg.format + " from " + idText(m.hdr.src) + " to " + idText(m.hdr.dst) + (crcOk ? "" : " (CRC wrong)") + ": " + msg.text);
}

// ---------------------------------------------------------------------------------------------------- voice

void Link::voiceBurst(Track& t, int pos, const Bits& burst, int lcss, const Bits& emb32, bool hadSync) {
    tel_.voiceBursts++;
    t.state = 1;
    t.lastVoiceSample = n_;
    t.sinceVoice = 0;
    t.vpos = pos;
    t.lastBurst = voiceName(pos);
    if (!hadSync) tel_.embeddedBursts++;
    t.voiceRun++;
    if (t.confirmed || t.hits >= 2) {
        // a call starts at a voice sync, or after two voice bursts in a row (joining in the middle)
        if (!t.inCall && (hadSync || t.voiceRun >= 2)) startCall(t, 0, 0, 0, 0, true, false);
        if (t.inCall) t.call.voiceFrames += 3;
    }
    if (onVoiceBurst) {
        Bits vs;
        burstVoicePayload(burst, vs);
        onVoiceBurst(slotOf(t), pos, vs);
    }
    if (hadSync) { t.lcHave = 0; return; }
    // embedded link control: LCSS 1 starts, 3 continues, 2 ends the four fragments
    if (lcss == 1) { t.lcFrag[0] = emb32; t.lcHave = 1; }
    else if (lcss == 3 && t.lcHave >= 1 && t.lcHave <= 2) { t.lcFrag[t.lcHave++] = emb32; }
    else if (lcss == 2 && t.lcHave == 3) {
        t.lcFrag[3] = emb32;
        t.lcHave = 0;
        uint8_t lc[9];
        const int e = embLcDecode(t.lcFrag, lc);
        if (e < 0) {
            tel_.embLcFail++; tel_.blocksBad++;
            if (t.inCall) t.call.fecErrors++;
            noteFec(t, 8, 128, false);      // a lost matrix counts as a few bit errors in the estimate
        } else {
            tel_.embLcOk++; tel_.blocksOk++;
            if (e > 0) noteFec(t, e, 128);
            FullLc f;
            parseFullLc(lc, f);
            if (f.flco == kFlcoGroupVoice || f.flco == kFlcoUnitVoice) {
                const int kind = f.flco == kFlcoUnitVoice ? 1 : (f.dst >= 0xFFFFF0 ? 2 : 0);
                if (!t.inCall) startCall(t, kind, f.src, f.dst, f.svc, true, true);
                else if (!t.call.idsKnown) {
                    t.call.src = f.src; t.call.dst = f.dst; t.call.kind = kind; t.call.idsKnown = true;
                    t.call.emergency = f.svc & 0x80; t.call.privacy = f.svc & 0x40;
                    say("slot " + std::to_string(slotOf(t)) + ": call from " + idText(f.src) + " to " + idText(f.dst));
                } else if (t.call.src != f.src || t.call.dst != f.dst) {
                    startCall(t, kind, f.src, f.dst, f.svc, true, true);
                }
            } else if (f.flco >= kFlcoTalkerAliasHeader && f.flco <= kFlcoTalkerAliasBlock3) {
                t.ta.add(f);
                if (t.ta.complete()) {
                    const std::string a = t.ta.text();
                    if (!a.empty() && a != t.alias) {
                        t.alias = a;
                        if (t.inCall) t.call.alias = a;
                        say("slot " + std::to_string(slotOf(t)) + ": talker alias " + a);
                    }
                }
            }
        }
    } else {
        t.lcHave = 0;
    }
}

void Link::startCall(Track& t, int kind, uint32_t src, uint32_t dst, uint8_t svc, bool late, bool idsKnown) {
    if (t.inCall) endCall(t, false);
    DmrCall c;
    c.slot = slotOf(t);
    c.startSec = nowSec() - (late ? 0.06 : 0.0);
    c.endSec = c.startSec;
    c.active = true;
    c.cc = t.cc;
    c.kind = kind;
    c.src = src; c.dst = dst;
    c.idsKnown = idsKnown;
    c.emergency = svc & 0x80; c.privacy = svc & 0x40;
    c.lateEntry = late;
    t.alias.clear();
    t.ta.clear();
    t.call = c;
    t.inCall = true;
    t.sinceVoice = 0;
    tel_.calls++;
    if (idsKnown) {
        static const char* kn[3] = {"group call", "private call", "all call"};
        say("slot " + std::to_string(c.slot) + ": " + kn[kind % 3] + " from " + idText(src) + " to " + idText(dst) + (late ? " (late entry)" : ""));
    }
}

void Link::endCall(Track& t, bool terminated) {
    if (!t.inCall) return;
    t.call.active = false;
    t.call.endSec = nowSec();
    t.call.terminated = terminated;
    if (t.call.cc < 0) t.call.cc = t.cc;
    addLog(t.call);
    say("slot " + std::to_string(slotOf(t)) + ": call ended after " + std::to_string((int)std::lround((t.call.endSec - t.call.startSec) * 10) / 10.0).substr(0, 4) + " s, " +
        std::to_string(t.call.voiceFrames) + " voice frames" + (terminated ? "" : " (no terminator)"));
    t.inCall = false;
    t.call = DmrCall();
}

void Link::addLog(const DmrCall& c) {
    tel_.callLog.push_back(c);
    if (tel_.callLog.size() > 100) tel_.callLog.erase(tel_.callLog.begin());
}

void Link::addMessage(const DmrMessage& m) {
    tel_.messages.push_back(m);
    if (tel_.messages.size() > 16) tel_.messages.erase(tel_.messages.begin());
}

// ---------------------------------------------------------------------------------------------------- CACH

void Link::handleCach(Track& t, const float* zc) {
    Bits bits;
    for (int i = 0; i < 12; i++) {
        const int hd = zc[i] >= 2.f ? 3 : zc[i] >= 0.f ? 1 : zc[i] >= -2.f ? -1 : -3;
        putBits(bits, symbolToDibit(hd), 2);
    }
    int at = 0, tc = 0, lcss = 0;
    unsigned pay = 0;
    const int tactErr = cachDecode(bits, at, tc, lcss, pay);
    if (tactErr < 0) return;
    // The TACT is a Hamming (7,4) code: every 7 bit word is within one error of a code word, so noise always "decodes". Only words without a
    // correction vote for the slot, and the slot follows the majority.
    if (t.family == 0 && tactErr == 0) {
        if (t.tcVote[tc] < 8) t.tcVote[tc]++;
        if (t.slot == 0) t.slot = tc + 1;
        if (t.tcVote[0] + t.tcVote[1] >= 2 && t.tcVote[0] != t.tcVote[1]) t.slot = t.tcVote[1] > t.tcVote[0] ? 2 : 1;
    }
    Bits p;
    putBits(p, pay, 17);
    if (lcss == 1) { shortLcPiece_[0] = p; shortLcHave_ = 1; }
    else if (lcss == 3 && shortLcHave_ >= 1 && shortLcHave_ <= 2) shortLcPiece_[shortLcHave_++] = p;
    else if (lcss == 2 && shortLcHave_ == 3) {
        shortLcPiece_[3] = p;
        shortLcHave_ = 0;
        uint32_t lc = 0;
        if (shortLcDecode(shortLcPiece_, lc) >= 0) {
            const int slco = (int)(lc >> 24);
            if (slco == 0) cachInfo_ = "null";
            else if (slco == 1) {
                const int a1 = (int)((lc >> 20) & 15), a2 = (int)((lc >> 16) & 15);
                cachInfo_ = std::string("activity: slot 1 ") + activityName(a1) + ", slot 2 " + activityName(a2);
            } else cachInfo_ = "short LC opcode " + std::to_string(slco);
        }
    } else shortLcHave_ = 0;
}

// ---------------------------------------------------------------------------------------------------- telemetry

void Link::snapshot(DmrTelemetry& out) const {
    out = tel_;
    out.cc = cc_;
    out.eye.assign(eye_.end() - (ptrdiff_t)std::min<size_t>(eye_.size(), 800), eye_.end());
    out.cachInfo = cachInfo_;
    out.link = linkFamily_ == 0 ? "base station" : linkFamily_ == 1 ? "mobile" : linkFamily_ == 2 ? "direct mode" : "";
    out.devHz = (float)devHz_;
    out.symbolPpm = (float)symPpm_;
    out.ber = berDen_ > 50 ? (float)(berNum_ / berDen_) : 0.f;
    // the symbol quality is only meaningful while bursts are being read: two seconds without one and it is not reported
    const bool fresh = snrValid_ && lastLockSample_ > 0 && n_ - lastLockSample_ < 2 * 48000;
    out.snrDb = fresh ? (float)std::max(0.0, std::min(40.0, 10.0 * std::log10(5.0 / std::max(snrEma_, 1e-4)))) : 0.f;
    out.cfoHz = 0;
    for (int s = 0; s < 2; s++) out.slot[s] = lastSlot_[s];
    int locked = 0;
    bool decoding = false, partial = false;
    const double now = nowSec();
    for (const Track& t : tr_) {
        if (!t.used) continue;
        if (!t.confirmed) { partial = true; continue; }
        locked++;
        if (now - t.lastSeenSec < 1.0) decoding = true;
        const int si = (t.slot >= 1 && t.slot <= 2 ? t.slot : 1) - 1;
        DmrSlot& d = out.slot[si];
        d.active = now - t.lastSeenSec < 1.0;
        d.state = t.state;
        d.lastBurst = t.lastBurst;
        d.inCall = t.inCall;
        d.callKind = t.call.kind;
        d.src = t.call.src; d.dst = t.call.dst;
        d.voiceFrames = t.call.voiceFrames;
        d.rmsErr = (float)t.errEma;
        d.ber = t.berDen > 50 ? (float)(t.berNum / t.berDen) : 0.f;
        d.bursts = t.bursts;
        if (t.inCall) out.callLog.push_back(t.call);
    }
    if (out.callLog.size() > 100) out.callLog.erase(out.callLog.begin(), out.callLog.end() - 100);
    out.slotsLocked = locked;
    out.state = decoding ? 2 : (locked || partial) ? 1 : 0;
    out.dataValid = decoding;
    out.cfoHz = cfoResidualHz();
}

} // namespace dmr
} // namespace dect2
