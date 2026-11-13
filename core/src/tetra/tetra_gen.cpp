// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA test signal (tetra_gen.h).
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetra/tetra_teds.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 {

int tetraTestCells(int scene) { return scene == 1 ? 2 : 1; }   // scene 3: one cell (and the QAM carrier, made apart)

namespace {

uint64_t netTime(int doy, int hh, int mm, int ss, int year, int offs15) {
    const uint64_t utc2 = (uint64_t)((doy * 86400 + hh * 3600 + mm * 60 + ss) / 2);
    return (utc2 << 24) | (0ull << 23) | ((uint64_t)offs15 << 17) | ((uint64_t)(year - 2000) << 11) | 0x7FFull;
}

} // namespace

TetraCellCfg tetraTestCell(int scene, int index) {
    TetraCellCfg c;
    c.sync.mcc = 1; c.sync.mnc = 1; c.sync.systemCode = 2; c.sync.nbrBroadcast = 3; c.sync.cellLoad = 1; c.sync.lateEntry = true;
    tetra::Sysinfo& s = c.si;
    s.band = 3; s.duplex = 0; s.reverse = false; s.numScch = 0; s.msTxPwrMax = 4; s.rxLevAccessMin = 3; s.accessParam = 7; s.dlTimeout = 6;
    s.cckFlag = false; s.optFlag = 3; s.subscriberClass = 0xFFFF;
    if (scene == 3) {   // TEDS: the scene 0 cell, announcing pi/8-D8PSK and 25 / 50 kHz QAM, timeslot 3 in pi/8-D8PSK
        c = tetraTestCell(0, 0);
        c.tedsServices = true; c.tedsD8psk = true; c.tedsQamMask = 3; c.d8pskTs = 3;
        return c;
    }
    const bool second = scene == 1 && index == 1;
    if (!second) {
        c.sync.colourCode = 5;
        s.mainCarrier = 3696; s.offset = 3;   // 392.4125 MHz
        s.hyperframeOrCck = 1234;
        s.optValue = 0xA0u << 12;             // authentication required, security class 3
        s.la = 1001;
        s.bsService = tetra::kSvcRegistration | tetra::kSvcSystemWide | tetra::kSvcVoice | tetra::kSvcSndcp | tetra::kSvcAirEncryption;
        c.encryptedSignalling = true;
        c.amp = 0.25f;
        c.startSlot = 4 * 18 * 7 + 4 * 3 + 1;  // multiframe 8, frame 4, timeslot 2
        tetra::Neighbour a; a.cellId = 1; a.carrier = 3704; a.la = 1002; a.synced = true; a.cellLoad = 1; a.hasExt = true; a.band = 3; a.offset = 0;
        tetra::Neighbour b; b.cellId = 2; b.carrier = 3710; b.la = 1003; b.hasExt = true; b.band = 3; b.offset = 0; b.cellLoad = 2;
        c.nwrk.nbrs = {a, b};
        c.nwrk.hasTime = true; c.nwrk.time48 = netTime(284, 9, 30, 0, 2026, 12);   // 2026-10-12 09:30 UTC, local +03:00
        c.nwrk.reselect = 0x5A5A; c.nwrk.cellLoad = 1;
    } else {
        c.sync.colourCode = 12;
        s.mainCarrier = 3704; s.offset = 0;   // 392.6000 MHz
        s.hyperframeOrCck = 77;
        s.optValue = 0x40u << 12;             // security class 1: no encryption
        s.la = 1002;
        s.bsService = tetra::kSvcRegistration | tetra::kSvcSystemWide | tetra::kSvcVoice | tetra::kSvcCircuitData;
        c.encryptedSignalling = false;
        c.trafficTs = 3;
        c.amp = 0.2f;
        c.startSlot = 4 * 18 * 33 + 4 * 15 + 3;   // multiframe 34, frame 16, timeslot 4
        tetra::Neighbour a; a.cellId = 3; a.carrier = 3696; a.offset = 3; a.hasExt = true; a.band = 3; a.la = 1001; a.cellLoad = 1;
        c.nwrk.nbrs = {a};
        c.nwrk.hasTime = false;
        c.nwrk.reselect = 0x1234; c.nwrk.cellLoad = 2;
    }
    return c;
}

TetraWideCell tetraWideCell(int index, int count, double rateHz) {
    TetraWideCell w;
    count = std::max(1, count);
    w.cfg = tetraTestCell(0, 0);
    const double span = 0.72 * rateHz - 150e3;   // the carrier search's usable band, less a margin for the skirts
    const int channels = std::max(count, (int)std::floor(span / 25e3));
    // evenly over the channels, never on top of a neighbour: slot i of count gets channel round((i + 0.5) / count * channels) - channels / 2
    const int k = (int)std::floor(((double)index + 0.5) / (double)count * (double)channels) - channels / 2;
    w.offHz = k * 25e3;
    static const double kErr[] = {1800, -2600, 900, -1200, 3500, -700, 2300, -3900};
    w.errHz = kErr[(unsigned)index % 8];
    static const float kAmp[] = {0.30f, 0.12f, 0.22f, 0.08f, 0.26f, 0.15f, 0.10f, 0.19f};   // 0.08 .. 0.30: 11.5 dB apart
    w.cfg.amp = kAmp[(unsigned)index % 8];
    w.cfg.sync.colourCode = 1 + index % 63;
    w.cfg.si.la = (uint16_t)(2000 + index);
    w.cfg.si.hyperframeOrCck = (uint16_t)(100 + index);
    const double f = kTetraTestCentreHz + w.offHz;
    w.cfg.si.band = 3; w.cfg.si.offset = 0;
    w.cfg.si.mainCarrier = (uint16_t)std::lround((f - 300e6) / 25e3);
    w.cfg.encryptedSignalling = (index % 2) == 0;
    w.cfg.si.optValue = (index % 2) == 0 ? (0xA0u << 12) : (0x40u << 12);
    w.cfg.trafficTs = 1 + (index % 3) + 1;   // 2..4
    w.cfg.startSlot = (uint32_t)(4 * 18 * (1 + 3 * index) + 4 * (index % 18) + index % 4);
    w.cfg.nwrk.nbrs.clear();
    w.cfg.nwrk.hasTime = false;
    return w;
}

namespace tetra {

namespace {
// The cell's SYSINFO in multiframe mn: a TEDS cell's optional field is the extended services broadcast, section 1 in odd multiframes,
// section 2 in even ones
Sysinfo cellSysinfo(const TetraCellCfg& c, int mn) {
    Sysinfo si = c.si;
    if (c.tedsServices) {
        ExtServices e;
        e.security = (int)((c.si.optValue >> 12) & 0xFF);
        e.sdsTl = 1;
        e.section = (mn & 1) ? 0 : 1;
        e.qos = true; e.d8psk = c.tedsD8psk; e.sec2 = c.tedsQamMask != 0; e.qamMask = c.tedsQamMask;
        si.optFlag = 3; si.optValue = encodeExtServices(e);
    }
    return si;
}
} // namespace

Bits cellSlot(const TetraCellCfg& c, uint32_t abs) {
    abs %= 60u * 72u;
    const int tn = (int)(abs % 4) + 1, fn = (int)((abs / 4) % 18) + 1, mn = (int)(abs / 72) + 1;
    const uint32_t scr = scramblerInit(c.sync.mcc, c.sync.mnc, c.sync.colourCode);
    int hdr = 0, f1 = 1, f2 = 1;
    if (fn != 18 && tn != 1) { hdr = 1; f1 = tn == c.trafficTs ? 5 : 0; }
    const Bits bb = encodeAach((uint16_t)((hdr << 12) | (f1 << 6) | f2), scr);
    auto split = [](const Bits& b432, Bits& a, Bits& b) { a.assign(b432.begin(), b432.begin() + 216); b.assign(b432.begin() + 216, b432.end()); };
    const Sysinfo si0 = cellSysinfo(c, mn);
    if (fn == 18) {
        const Bits si = encodeChannel(encodeSysinfo(si0), kSchHd, scr);
        const Bits nul = encodeChannel(macNullPdu(124), kSchHd, scr);
        if (tn == 4 - ((mn + 1) % 4)) {
            Sync s = c.sync; s.tn = tn; s.fn = fn; s.mn = mn;
            return syncBurst(encodeChannel(encodeSync(s), kBsch, scramblerInit(0, 0, 0)), bb, si);
        }
        if (tn == 4 - ((mn + 3) % 4)) return normalBurst(nul, bb, si, true);
        return normalBurst(nul, bb, nul, true);
    }
    Bits a, b;
    if (tn == c.trafficTs) {   // speech frames of a call: random bits, as encrypted or clear voice would look to this decoder
        std::mt19937 g(abs * 7919u + 17u);
        a.resize(216); b.resize(216);
        for (auto& v : a) v = (uint8_t)(g() & 1);
        for (auto& v : b) v = (uint8_t)(g() & 1);
        return normalBurst(a, bb, b, false);
    }
    Bits t1;
    if (tn == 1 && fn == 1) t1 = macBroadcastResource(mleNwrkBroadcast(c.nwrk), 268, 0);
    else if (tn == 1 && (fn == 5 || fn == 11) && c.encryptedSignalling) { Bits fake(120, 0); t1 = macBroadcastResource(fake, 268, 1, abs); }
    else t1 = macNullPdu(268);
    split(encodeChannel(t1, kSchF, scr), a, b);
    return normalBurst(a, bb, b, false);
}

std::vector<int> cellSlotSteps(const TetraCellCfg& c, uint32_t abs) {
    abs %= 60u * 72u;
    const int tn = (int)(abs % 4) + 1, fn = (int)((abs / 4) % 18) + 1, mn = (int)(abs / 72) + 1;
    if (c.d8pskTs > 0 && tn == c.d8pskTs && fn != 18) {
        const uint32_t scr = scramblerInit(c.sync.mcc, c.sync.mnc, c.sync.colourCode);
        const Bits bb = encodeAach((uint16_t)((1 << 12) | (0 << 6) | 1), scr);   // common control, unallocated uplink
        // block 1: SYSINFO as sent with pi/8-D8PSK (21.4.4.1: 28 reserved bits before the MLE part, 152 bits) and a null PDU; block 2: a null PDU
        Bits s = encodeSysinfo(cellSysinfo(c, mn));
        s.insert(s.begin() + 82, 28, 0);
        const Bits nul = macNullPdu(kSchP8Hd.type1 - (int)s.size());
        s.insert(s.end(), nul.begin(), nul.end());
        return normalBurstP8(encodeChannel(s, kSchP8Hd, scr), bb, encodeChannel(macNullPdu(kSchP8Hd.type1), kSchP8Hd, scr), true);
    }
    const Bits b = cellSlot(c, abs);
    std::vector<int> st(b.size() / 2);
    for (size_t k = 0; k < st.size(); k++) st[k] = 2 * dibitPhase(b[2 * k], b[2 * k + 1]);
    return st;
}

} // namespace tetra

namespace {

class TetraSynth : public ModeSynth {
public:
    TetraSynth(const SynthConfig& c, double rate) : out_(rate), cfo_(c.cfoHz), ppm_(c.sroPpm), rng_(31) {
        const int scene = c.modeOpt[0] == 0 && rate >= 4e6 ? 2 : c.modeOpt[0];   // a capture of 4 Msps or more holds a stretch of the band: the default test signal becomes the wide scene
        const int n = scene == 2 ? std::max(1, c.modeOpt[1] > 0 ? c.modeOpt[1] : 12) : tetraTestCells(scene);
        double pmax = 0;
        for (int i = 0; i < n; i++) {
            Bts b;
            if (scene == 2) { const TetraWideCell w = tetraWideCell(i, n, out_); b.c = w.cfg; b.offHz = w.offHz + w.errHz; }   // (a wide capture: each cell with its own tuning error)
            else b.c = tetraTestCell(scene, i);
            if (scene != 2) b.offHz = tetra::dlCarrierHz(b.c.si.band, b.c.si.mainCarrier, b.c.si.offset) - kTetraTestCentreHz;
            b.rs.reset(new cell::Resampler(std::min(0.5, 0.5 * out_ / b.rate) * 0.95));
            b.abs = b.c.startSlot;
            bts_.push_back(std::move(b));
            pmax = std::max(pmax, (double)bts_.back().c.amp * bts_.back().c.amp);
        }
        if (scene == 3 && out_ >= 120e3) {   // the TEDS QAM carrier (needs the 50 kHz of it inside the capture)
            Bts q;
            q.c.amp = 0.2f;
            q.qam.reset(new tetra::QamGen(tetra::kTetraTedsQamKHz, tetra::kTetraTedsQamOrder, 4242));
            q.rate = tetra::kQamGenRate;
            q.offHz = tetra::kTetraTedsQamOffHz;
            q.rs.reset(new cell::Resampler(std::min(0.5, 0.5 * out_ / q.rate) * 0.95));
            bts_.push_back(std::move(q));
        }
        noise_ = (float)std::sqrt(pmax * (out_ / 25e3) / std::pow(10.0, c.snrDb / 10) / 2);
        if (c.snrDb >= 99) noise_ = 0;
    }
    double sampleRate() const override { return out_; }
    void generate(cf32* o, size_t n) override {
        for (size_t i = 0; i < n; i++) o[i] = 0;
        std::vector<cf32> tmp(n);
        for (auto& b : bts_) {
            const double step = b.rate * (1 + ppm_ * 1e-6) / out_;
            const int H = b.rs->halfLen();
            size_t done = 0;
            while (done < n) {
                if (b.pos + H + 2 >= (double)b.buf.size()) { more(b); continue; }
                const size_t fit = (size_t)std::floor(((double)b.buf.size() - H - 2 - b.pos) / step) + 1;
                const size_t m = std::min(n - done, fit);
                b.rs->run(b.buf.data(), b.buf.size(), b.pos, step, tmp.data() + done, m);
                b.pos += step * (double)m;
                done += m;
            }
            const double w = 2 * M_PI * b.offHz / out_;
            for (size_t i = 0; i < n; i++) {
                o[i] += tmp[i] * b.c.amp * cf32((float)std::cos(b.ph), (float)std::sin(b.ph));
                b.ph = std::fmod(b.ph + w, 2 * M_PI);
            }
        }
        dect2::normal_distribution<float> nd(0.f, 1.f);
        const double wc = 2 * M_PI * cfo_ / out_;
        for (size_t i = 0; i < n; i++) {
            if (cfo_ != 0) { o[i] *= cf32((float)std::cos(cph_), (float)std::sin(cph_)); cph_ = std::fmod(cph_ + wc, 2 * M_PI); }
            if (noise_ > 0) o[i] += cf32{noise_ * nd(rng_), noise_ * nd(rng_)};
        }
    }

private:
    struct Bts {
        TetraCellCfg c;
        double offHz = 0, pos = 0, ph = 0;
        uint32_t abs = 0;
        std::unique_ptr<cell::Resampler> rs;
        tetra::Modulator mod;
        std::unique_ptr<tetra::QamGen> qam;   // scene 3: a TEDS QAM carrier instead of a cell
        double rate = tetra::kRate;
        std::vector<cf32> buf;
    };
    void more(Bts& b) {
        const long keep = std::max(0L, (long)std::floor(b.pos) - b.rs->halfLen() - 2);
        if (keep > 0) { b.buf.erase(b.buf.begin(), b.buf.begin() + keep); b.pos -= (double)keep; }
        if (b.qam) { b.qam->push(240, b.buf); return; }   // 0.1 s
        for (int k = 0; k < 24; k++) {   // six frames at a time
            if (b.c.d8pskTs) b.mod.pushSteps(tetra::cellSlotSteps(b.c, b.abs), b.buf);
            else b.mod.push(tetra::cellSlot(b.c, b.abs), b.buf);
            b.abs = (b.abs + 1) % (60u * 72u);
        }
    }
    double out_, cfo_, ppm_, cph_ = 0;
    float noise_ = 0;
    std::vector<Bts> bts_;
    std::mt19937 rng_;
};

} // namespace

std::unique_ptr<ModeSynth> makeTetraSynth(const SynthConfig& cfg, double sampleRate) { return std::unique_ptr<ModeSynth>(new TetraSynth(cfg, sampleRate)); }

} // namespace dect2
