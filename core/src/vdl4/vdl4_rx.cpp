// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 4 receiver: see vdl4_rx.h.
#include "dect2/vdl4/vdl4_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/vdl4/vdl4_proto.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <ctime>
#include <mutex>
#include <cstdlib>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using cd = std::complex<double>;
using dect2::math::kPi;
constexpr double kDefaultCenter = 136.85e6;

bool syncAt(const std::vector<uint8_t>& bits, size_t i) {
    if (i + kVdl4SyncBits > bits.size()) return false;
    for (int k = 0; k < kVdl4SyncBits; k++) if (bits[i + k] != kVdl4SyncPattern[k]) return false;
    return true;
}

// Demodulator working rate: 4 samples per bit (76.8 kHz). Each channel mixes to baseband, integrates and dumps
// over rate/76800 input samples (a fractional ratio is fine: the phase accumulator carries the remainder, no integer
// decimation factor is needed, so any input rate from 0.1 to 20+ Msps works), takes the FM discriminator at 4x
// oversampling and looks for the training sequence at the tail of that stream only (O(1) per sample, never a rescan
// of the whole history: that rescan was the CPU spin seen with an RSP1B at 2 and 4 Msps).
constexpr int kOsr = 4;
// Stage A: integrate-and-dump to 16 samples per bit, stage B: four one-pole low-passes (12 kHz) and take every 4th sample.
constexpr int kStageA = 16;
// 4.1: alpha 0.2 put the four poles' -3 dB point at 4.8 kHz, the deviation itself (h 0.5 at 19.2 kb/s is +-4.8 kHz): the 0101 training sequence
// and every short run lost most of its swing, so bursts failed their FCS even at 30 dB and the threshold sat near 24 dB in 25 kHz. 0.36 puts
// it at 9.5 kHz (about the bit rate across both sides, the usual pre-detection width for GMSK on a discriminator).
// DECT2_VDL4_LPA overrides it (measurements only).
const double kLpAlpha = [] { const char* e = std::getenv("DECT2_VDL4_LPA"); const double v = e ? std::atof(e) : 0.0; return v > 0.01 && v < 1.0 ? v : 0.36; }();
constexpr int kBurstBits = 480;                 // line bits kept after the sync (a burst is at most a few hundred)
constexpr int kSyncSamples = kOsr * kVdl4SyncBits;
constexpr size_t kLineMax = 4 * (size_t)kOsr * (kBurstBits + 64);

struct Chan {
    double freqHz = 0, offHz = 0;
    bool inBand = false;
    cd rot = 1, step = 1;
    float floor = 0;
    Vdl4ChannelInfo info;
    std::vector<float> line;                    // discriminator output (rad per sample), kOsr samples per bit; sliced against the sync's mean (= the carrier offset)
    // 4.1: the squelch's power: in the channel, after the channel filter, smoothed over about one training sequence; floor: the mean of the quiet
    // stretches (floorN counts the first 50 ms, whose mean starts it)
    float chPwr = 0;
    int floorN = 0;
    cd acc = 0, prev = 0, lp[4] = {0, 0, 0, 0};   // lp: four one-pole low-passes at 307.2 kHz (channel filter, 25 kHz spacing)
    int dec4 = 0;
    double accN = 0, phase = 0;
    float accPwr = 0;
    int discRun = 0;
    long pending = -1;                          // index in line where the burst data ends; -1 = hunting for sync
    long syncEnd = 0;
    int cool = 0, lockLeft = 0, bestErr = 99;
    long bestN = 0;
    double sum96 = 0, bestThr = 0, thr = 0;     // sum of the last 24 bits' worth of samples (the sync is balanced, so its mean is the carrier offset)
};
} // namespace

std::vector<double> vdl4EuropeChannels() { return {136.725e6, 136.775e6, 136.825e6, 136.875e6, 136.900e6, 136.975e6}; }

struct Vdl4Receiver::Impl {
    std::mutex mu;
    double rate = 0, offset = 0, center = 0;
    std::vector<double> want = vdl4EuropeChannels();
    bool rebuild = true;
    std::function<void(const std::string&)> logCb;
    std::vector<Chan> ch;
    uint64_t nIn = 0;
    Vdl4Telemetry tel;
    uint64_t serial = 0, seq = 0;
    double lastGood = -1e9;

    void log(const std::string& s) { if (logCb) logCb(s); }

    void rebuildCh() {
        ch.clear();
        rebuild = false;
        if (!(rate >= 1e3) || !std::isfinite(rate)) return;   // unusable rate: no channels, the status line says so
        const double dial = center > 0 ? center : kDefaultCenter;
        for (double f : want) {
            Chan c;
            c.freqHz = f;
            c.offHz = f - dial - offset;
            c.inBand = std::fabs(c.offHz) < rate * 0.45 && rate >= kStageA * kVdl4BitRate;
            c.info.freqHz = f;
            c.info.inBand = c.inBand;
            const double w = 2 * kPi * c.offHz / rate;
            c.step = cd(std::cos(w), -std::sin(w));   // mix the channel DOWN to baseband (exp(-jwn))
            ch.push_back(c);
        }
    }

    void onFrame(Chan& c, const std::vector<uint8_t>& fr, float snr, float lvl, double cfo) {
        if (!vdl4FcsOk(fr.data(), fr.size())) { c.info.bad++; tel.blocksBad++; return; }
        Vdl4BurstFields bf;
        if (!vdl4ParseBurst(fr.data(), fr.size() - 2, bf)) { c.info.bad++; tel.blocksBad++; return; }
        Vdl4Message m;
        if (!vdl4DecodeMessage(bf, c.freqHz, m)) return;
        m.serial = ++serial;
        m.timeSec = tel.nowSec;
        m.wallTime = (int64_t)std::time(nullptr);
        m.levelDb = lvl;
        m.snrDb = snr;
        m.cfoHz = (float)cfo;
        tel.messages.insert(tel.messages.begin(), m);
        if (tel.messages.size() > kVdl4MsgMax) tel.messages.pop_back();
        tel.blocksOk++;
        c.info.good++;
        c.info.frames++;
        tel.dataValid = true;
        tel.snrDb = snr;
        lastGood = tel.nowSec;
        tel.state = 2;
        if (m.hasPos) {
            tel.positionsTotal++;
            auto& ac = tel.aircraft;
            auto it = std::find_if(ac.begin(), ac.end(), [&](const Vdl4Aircraft& a) { return a.addr == m.src; });
            if (it == ac.end()) {
                Vdl4Aircraft a;
                a.addr = m.src;
                ac.insert(ac.begin(), a);
                it = ac.begin();
            }
            it->messages++;
            it->freqHz = c.freqHz;
            it->lastHeardSec = tel.nowSec;
            it->lastWall = m.wallTime;
            it->levelDb = lvl;
            if (m.hasPos) {
                it->hasPos = true;
                it->lat = m.lat;
                it->lon = m.lon;
                it->posKind = m.posKind;
                it->posTime = tel.nowSec;
            }
            if (m.hasAlt) { it->hasAlt = true; it->altFt = m.altFt; }
        }
    }

    // Decode the burst whose sync ended at c.syncEnd (index in c.line of the sync's last bit sample).
    void decodeBurst(Chan& c, float pwr, float snr) {
        std::vector<uint8_t> burst;
        burst.reserve(kVdl4DataStartBit + kBurstBits);
        for (int i = 0; i < kVdl4RampBits; i++) burst.push_back(1);
        for (int i = 0; i < kVdl4SyncBits; i++) burst.push_back(kVdl4SyncPattern[i]);
        for (long k = c.syncEnd + kOsr; k < (long)c.line.size() && (int)burst.size() < kVdl4DataStartBit + kBurstBits; k += kOsr) burst.push_back(c.line[(size_t)k] > c.thr ? 1 : 0);
        std::vector<uint8_t> data;
        if (vdl4BurstDataBits(burst, data)) {
            for (const auto& fr : vdl4HdlcFrames(data)) onFrame(c, fr, snr, 10.f * std::log10(std::max(pwr, 1e-12f)), c.offHz);
        }
        c.info.bursts++;
        c.info.active = true;
        c.info.lastHeardSec = tel.nowSec;
        c.info.levelDb = 10.f * std::log10(std::max(pwr, 1e-12f));
    }

    // One discriminator sample at 4 samples per bit. pwr: the power of the whole input band (the level shown); chp: the in-channel power.
    void demodSample(Chan& c, cd d, float pwr, float chp) {
        const double w = std::arg(d * std::conj(c.prev));
        c.prev = d;
        c.line.push_back((float)w);
        c.sum96 += w - (c.line.size() > (size_t)kSyncSamples ? c.line[c.line.size() - 1 - kSyncSamples] : 0.f);
        // 4.1: the squelch and the noise floor on the in-channel power. They were on the power of the whole input band (2 MHz and more), where a
        // burst 20 dB over the noise in its own 25 kHz channel is only 1.7 dB over the floor: the squelch, not the demodulator, set the threshold
        // (20 dB in the QA sweep, 21 dB in 25 kHz). The floor is the mean of the quiet stretches (slow, and only while the power is under twice it).
        if (c.floorN < 4096) c.floor += (chp - c.floor) / (float)(++c.floorN);
        else c.floor += (chp < c.floor ? 0.002f : chp < 2.f * c.floor ? 0.0005f : 0.00001f) * (chp - c.floor);
        const long n = (long)c.line.size();
        if (c.pending >= 0) {
            if (n >= c.pending) {
                decodeBurst(c, pwr, 10.f * std::log10(std::max(chp / std::max(c.floor, 1e-12f), 1e-6f)));
                c.pending = -1;
                c.cool = 0;
                c.lockLeft = 0;
            }
        } else if (c.cool++ > 2 * kOsr && n > (long)(kVdl4SyncBits + 4) * kOsr) {
            // 4 ramp ones then the 24-bit 0101 training sequence ending at the tail. The alternating pattern also matches
            // 2 or 4 bits early (the ramp ones count as only 1 or 2 errors), so after the first hit the best fit within
            // the next 5 bits is taken as the end of the sync.
            int err = 0;
            for (int j = 0; j < kVdl4SyncBits + 4 && err <= 3; j++) {
                const uint8_t want = j < kVdl4SyncBits ? kVdl4SyncPattern[kVdl4SyncBits - 1 - j] : 1;
                err += (c.line[(size_t)(n - 1 - (long)kOsr * j)] > c.sum96 / kSyncSamples ? 1 : 0) != want;
            }
            if (c.lockLeft == 0 && err <= 3 && chp > 2.5f * c.floor) {   // squelch: noise alone must not start a lock
                c.lockLeft = 5 * kOsr; c.bestErr = 99; }
            if (c.lockLeft > 0) {
                if (err < c.bestErr) { c.bestErr = err; c.bestN = n; c.bestThr = c.sum96 / kSyncSamples; }
                if (--c.lockLeft == 0) {
                    c.thr = c.bestThr;
                    c.syncEnd = c.bestN;               // one sample after the fitted bit centre
                    c.pending = n + kOsr + (long)kOsr * kBurstBits;
                }
            }
        }
        if (c.line.size() > kLineMax) {              // trim the history, keep what a pending burst needs
            const long cut = c.pending >= 0 ? std::min<long>((long)c.line.size() / 2, c.syncEnd - kOsr * (kVdl4SyncBits + 2)) : (long)c.line.size() - kOsr * 2 * kVdl4SyncBits;
            if (cut > 0) {
                c.line.erase(c.line.begin(), c.line.begin() + cut);
                c.bestN -= cut;
                if (c.pending >= 0) { c.pending -= cut; c.syncEnd -= cut; }
            }
        }
    }

    // One input sample: mix, integrate and dump, discriminate.
    void demodChan(Chan& c, cf32 x, double sampPerOut) {
        // the complex products written out: std::complex's operator* checks for NaN after every product, which was most of the cost of this
        // loop (six channels at the full input rate). Same products and sums in the same order (no fused multiply-add across statements).
        {
            const double a = c.rot.real(), b = c.rot.imag(), s = c.step.real(), t = c.step.imag();
            const double ac = a * s, bd = b * t, ad = a * t, bc = b * s;
            c.rot = cd(ac - bd, ad + bc);
        }
        if (++c.discRun > 256) { c.discRun = 0; const double nr = std::abs(c.rot); if (nr > 0) c.rot /= nr; }
        {
            const double a = x.real(), b = x.imag(), s = c.rot.real(), t = c.rot.imag();
            const double ac = a * s, bd = b * t, ad = a * t, bc = b * s;
            c.acc += cd(ac - bd, ad + bc);
        }
        c.accPwr += x.real() * x.real() + x.imag() * x.imag();
        c.accN += 1;
        c.phase += 1;
        if (c.phase < sampPerOut) return;
        c.phase -= sampPerOut;
        cd d = c.acc / std::max(c.accN, 1.0);
        const float pwr = c.accPwr / (float)std::max(c.accN, 1.0);
        c.acc = 0; c.accPwr = 0; c.accN = 0;
        for (auto& st : c.lp) { st += kLpAlpha * (d - st); d = st; }
        if (++c.dec4 >= kStageA / kOsr) {
            c.dec4 = 0;
            c.chPwr += (float)((std::norm(d) - c.chPwr) * (1.0 / (kOsr * kVdl4SyncBits)));
            demodSample(c, d, pwr, c.chPwr);
        }
    }
};

Vdl4Receiver::Vdl4Receiver() : p_(new Impl) {}
Vdl4Receiver::~Vdl4Receiver() = default;

void Vdl4Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->rebuild = true; }
void Vdl4Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->offset != hz) { p_->offset = hz; p_->rebuild = true; } }
void Vdl4Receiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->center != hz) { p_->center = hz; p_->rebuild = true; } }
bool Vdl4Receiver::ready() const { return std::isfinite(p_->rate) && p_->rate >= vdl4Tuning().minSampleRate - 1; }
void Vdl4Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }
void Vdl4Receiver::setChannels(const std::vector<double>& hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->want) { p_->want = hz; p_->rebuild = true; } }

void Vdl4Receiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& c : p_->ch) { c.line.clear(); c.pending = -1; c.lockLeft = 0; c.sum96 = 0; c.floor = 0; c.chPwr = 0; c.floorN = 0; c.info = Vdl4ChannelInfo(); c.info.freqHz = c.freqHz; c.info.inBand = c.inBand; }
    p_->tel.state = 0;
}

void Vdl4Receiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->rebuild) p_->rebuildCh();
    const bool ok = ready();
    p_->tel.inputRate = p_->rate;
    p_->tel.rateOk = ok;
    if (!ok) {       // report the unsupported rate instead of running at it
        p_->tel.channels.clear();
        p_->tel.centerHz = p_->center;
        p_->seq++;
        return;
    }
    const double sampPerOut = p_->rate / (kVdl4BitRate * kStageA);
    for (size_t i = 0; i < n; i++) {
        p_->tel.nowSec = (double)p_->nIn++ / p_->rate;
        for (auto& c : p_->ch) {
            if (!c.inBand) continue;
            p_->demodChan(c, x[i], sampPerOut);
        }
    }
    if (p_->tel.nowSec - p_->lastGood > 30) p_->tel.state = p_->tel.blocksOk ? 1 : 0;
    for (auto& c : p_->ch) {
        if (p_->tel.nowSec - c.info.lastHeardSec > 2) c.info.active = false;
        c.info.noiseDb = 10.f * std::log10(std::max(c.floor, 1e-12f));
    }
    p_->tel.channels.clear();
    for (const auto& c : p_->ch) p_->tel.channels.push_back(c.info);
    p_->tel.centerHz = p_->center;
    p_->seq++;
}

void Vdl4Receiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.messages.clear();
    p_->tel.aircraft.clear();
    p_->tel.stations.clear();
    p_->tel.blocksOk = p_->tel.blocksBad = p_->tel.positionsTotal = 0;
    p_->tel.dataValid = false;
}

std::string Vdl4Receiver::saveResults() const { return std::string(); }
bool Vdl4Receiver::loadResults(const std::string&) { return false; }

bool Vdl4Receiver::telemetry(Vdl4Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seq == lastSeq) return false;
    out = p_->tel;
    out.seq = p_->seq;
    return true;
}

ModeTuning vdl4Tuning() {
    ModeTuning t;
    t.stdMode = 84;
    t.id = "vdl4";
    t.name = "VDL Mode 4";
    t.minMhz = 118;
    t.maxMhz = 140;
    t.defMhz = 136.850;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.4;
    t.minSampleRate = 400000;   // the first stage runs at 16 samples per bit = 307.2 kHz, and the six channels span 250 kHz
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
