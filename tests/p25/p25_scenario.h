// Synthetic P25 trunked-system scenarios (test harness, not shipped). Nothing is transmitted: all samples exist in memory only.
//
//   World   a set of transmitters (Source) at absolute frequencies, each on the air from startSec to endSec: Phase 1 C4FM (P25Generator, the
//           control channel script and voice calls with HDU / LDU1 / LDU2 / TDU / TDULC) or Phase 2 H-DQPSK two-slot TDMA (p25p2Signal, scrambled with
//           the seed WACN / system / NAC). Mixed to the baseband of a Radio (centre frequency, sample rate) with the impairments below.
//   Impair  per source: carrier offset, symbol clock error (Phase 1), two-ray multipath with a rotating phase (fading); radio: tuner frequency error
//           (ppm), noise, a DC spur at the centre. Deterministic (seeded).
//   Radio   the retunable fake: tune(dial) moves the centre to dial + 50 kHz (the tuner offset of the P25 mode), and the next settleSec of samples
//           are noise only (tuner settling), as on a real SDR. Counts and times every retune.
//   Rig     mirrors the loop of app/p25/p25_ui.cpp (the real receivers, the real Controller / Follower): TSBKs from P25Receiver telemetry into
//           Controller::onTsbk, the hopped radio's voice / terminator / encrypted reports, the Phase 2 receiver's reports (followTag), the wide
//           bank's carriers for Phase 1 calls inside the capture, setCapture + tick, and the Actions executed on the fake radio (Retune, OpenSlice,
//           CloseSlice). The UI file itself is ImGui and cannot run in a test; keep this loop in step with it when that one changes.
//   Rig::runUntil(t, stop)   the deterministic scenario runner the ctests use.
//
// Not covered: a real transmitter and real audio (voice frames are the generators' known placeholder patterns; there is no IMBE / AMBE+2 code),
// the engine's own mode switching (setStandard / setSecondMode are replaced by direct receiver use), the audio path.
#pragma once
#include "dect2/engine/modes.h"
#include "dect2/engine/wide_modes.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/trunk.h"
#include "dect2/p25p2/p25p2_gen.h"
#include "dect2/p25p2/p25p2_proto.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace p25sim {
using namespace dect2;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTuneOffsetHz = 50000;   // p25Tuning().tuneOffsetHz

// ------------------------------------------------------------------------------------------------ sources

struct Impair {
    double cfoHz = 0;            // carrier frequency offset of this transmitter
    double clockPpm = 0;         // symbol clock error (Phase 1 generator)
    double mpDelayUs = 0;        // two-ray multipath: second ray delay (0: none)
    double mpGain = 0;           // ... its amplitude relative to the first
    double mpFadeHz = 0;         // ... its phase rotation rate (Doppler / fading)
    double level = 1;            // amplitude factor (path loss)
};

struct Source {
    std::string name;
    double freqHz = 0;
    double startSec = 0, endSec = 1e9;       // on the air from / to (the signal starts afresh at startSec)
    Impair imp;
    std::function<std::unique_ptr<std::function<void(cf32*, size_t)>>(double rate)> make;   // creates the sample generator at the world rate
    // runtime
    std::unique_ptr<std::function<void(cf32*, size_t)>> gen;
    double phase = 0;
    std::vector<cf32> hist;
    size_t histPos = 0;
    double fadePh = 0;
};

// ---- Phase 1 sources

inline std::unique_ptr<std::function<void(cf32*, size_t)>> p1Gen(P25GenConfig c) {
    auto g = std::make_shared<P25Generator>(c);
    return std::make_unique<std::function<void(cf32*, size_t)>>([g](cf32* o, size_t n) { g->generate(o, n); });
}

// A Phase 1 transmitter: txs as given, no noise of its own (the world adds it), loop as asked
inline Source phase1(const std::string& name, double freqHz, std::vector<P25GenTx> txs, double startSec, double endSec, bool loop, Impair imp = Impair()) {
    Source s;
    s.name = name; s.freqHz = freqHz; s.startSec = startSec; s.endSec = endSec; s.imp = imp;
    s.make = [=](double rate) {
        P25GenConfig c;
        c.rate = rate; c.snrDb = 99; c.cfoHz = imp.cfoHz; c.clockPpm = imp.clockPpm; c.loop = loop; c.leadSec = 0.05;
        c.txs = txs; c.amp = 0.05;
        return p1Gen(c);
    };
    return s;
}

// ---- Phase 2 source: calls as given; the signal is rendered at the world rate when it starts
inline Source phase2(const std::string& name, double freqHz, P25p2GenConfig cfg, double startSec, double endSec, Impair imp = Impair()) {
    Source s;
    s.name = name; s.freqHz = freqHz; s.startSec = startSec; s.endSec = endSec; s.imp = imp;
    s.make = [=](double rate) {
        P25p2GenConfig c = cfg;
        c.rate = rate; c.snrDb = 99; c.cfoHz = imp.cfoHz;
        auto sig = std::make_shared<std::vector<cf32>>(p25p2Signal(c));
        auto pos = std::make_shared<size_t>(0);
        return std::make_unique<std::function<void(cf32*, size_t)>>([sig, pos](cf32* o, size_t n) {
            for (size_t i = 0; i < n; i++) o[i] = *pos < sig->size() ? (*sig)[(*pos)++] * 0.05f : cf32(0, 0);
        });
    };
    return s;
}

// ------------------------------------------------------------------------------------------------ control channel scripts

typedef std::vector<std::pair<int, uint64_t>> Frame;
inline std::pair<int, uint64_t> msg(const p25::Tsbk& t) { return {t.opcode, t.args}; }

// A band plan row: identifier, kind and numbers. Channel n of identifier id = (id << 12) | n; its frequency is base + n * spacing (TDMA: base + (n / slots) * spacing)
struct Band {
    int id = 1;
    enum Kind { Std, Vu, Tdma } kind = Std;
    double baseHz = 851006250, spacingHz = 12500, txOffsetHz = -45000000, bwHz = 12500;
    int chanType = 3;            // Tdma only
    int vuBwCode = 4;            // Vu only (4: 6.25 kHz)
    p25::Tsbk tsbk() const {
        switch (kind) {
        case Vu: return trunk::gen::idenUpVu(id, baseHz, spacingHz, txOffsetHz, vuBwCode);
        case Tdma: return trunk::gen::idenUpTdma(id, chanType, baseHz, spacingHz, txOffsetHz);
        default: return trunk::gen::idenUp(id, baseHz, spacingHz, txOffsetHz, bwHz);
        }
    }
    int channel(int n) const { return (id << 12) | n; }
    double freq(int n, int slots = 1) const { return baseHz + (n / slots) * spacingHz; }
};

inline Band band800() { Band b; b.id = 1; b.baseHz = 851006250; b.spacingHz = 12500; b.txOffsetHz = -45000000; return b; }
inline Band band700() { Band b; b.id = 2; b.baseHz = 769006250; b.spacingHz = 12500; b.txOffsetHz = 30000000; return b; }   // 700 MHz: uplink +30 MHz
inline Band bandUhf() { Band b; b.id = 3; b.kind = Band::Vu; b.baseHz = 453000000; b.spacingHz = 6250; b.txOffsetHz = 5000000; return b; }
inline Band bandVhf() { Band b; b.id = 4; b.kind = Band::Vu; b.baseHz = 151000000; b.spacingHz = 12500; b.txOffsetHz = 600000; b.vuBwCode = 5; return b; }
inline Band bandTdma() { Band b; b.id = 5; b.kind = Band::Tdma; b.baseHz = 770000000; b.spacingHz = 12500; b.txOffsetHz = 30000000; return b; }

struct Grant { int channel = 0, tg = 0, src = 0; bool upd = false; };

struct CcSpec {
    int nac = 0x293, wacn = 0xBEE00, sysId = 0x3A1, rfss = 1, site = 5, ccChannel = 0x1001;
    std::vector<Band> bands;
    std::vector<Grant> grants;   // sent in turn
    bool adjacent = true;
};

// The control channel script of a spec: a cycle of frames (identifier updates, system status, grants). Each grant is repeated twice per cycle.
inline P25GenTx ccTx(const CcSpec& s, double durSec) {
    P25GenTx tx;
    tx.kind = P25GenTx::Control;
    tx.nac = s.nac; tx.durSec = durSec; tx.gapSec = 0.1;
    for (const Band& b : s.bands) tx.script.push_back(Frame{msg(b.tsbk())});
    tx.script.push_back(Frame{msg(trunk::gen::netSts(s.wacn, s.sysId, s.ccChannel, 0x70)), msg(trunk::gen::rfssSts(s.sysId, s.rfss, s.site, s.ccChannel, 0x70))});
    if (s.adjacent) tx.script.push_back(Frame{msg(trunk::gen::adjSts(s.sysId, s.rfss, s.site + 1, s.ccChannel + 8, 0x70))});
    for (int rep = 0; rep < 2; rep++)
        for (const Grant& g : s.grants) {
            if (g.upd) tx.script.push_back(Frame{msg(trunk::gen::grpVGrantUpd(g.channel, g.tg, 0, 0))});
            else tx.script.push_back(Frame{msg(trunk::gen::grpVGrant(0x04, g.channel, g.tg, g.src))});
        }
    return tx;
}

inline P25GenTx voiceTx(int nac, int tg, int src, double durSec, bool emergency = false, bool noHeader = false, bool noTerm = false) {
    P25GenTx tx;
    tx.kind = P25GenTx::Voice;
    tx.nac = nac; tx.dst = tg; tx.src = src; tx.group = true; tx.durSec = durSec; tx.gapSec = 0.1;
    tx.emergency = emergency; tx.noHeader = noHeader; tx.noTerm = noTerm;
    return tx;
}

// A Phase 2 voice channel: one call on logical channel `slot` (1 or 2) for durSec, scrambled with the site's seed
inline P25p2GenConfig p2Call(const CcSpec& s, int slot, int tg, int src, double durSec, int algId = 0x80) {
    P25p2GenConfig c;
    c.colorCode = s.nac; c.wacn = s.wacn; c.sysId = s.sysId; c.scramble = true;
    c.superframes = (int)(durSec / 0.06) + 4;
    P25p2GenCall k; k.channel = slot; k.startSf = 1; k.nSf = (int)(durSec / 0.06); k.src = src; k.group = tg; k.algId = algId;
    c.calls = {k};
    return c;
}

// ------------------------------------------------------------------------------------------------ the world

struct World {
    double rate = 480000;
    double noiseSnrDb = 30;          // reference carrier (amplitude 0.05) to noise in 12.5 kHz; >= 99: none
    double dcSpur = 0;               // DC offset at the centre of the samples, relative to the carrier amplitude
    double tunerPpm = 0;             // the radio's reference error: every frequency is off by this many ppm
    uint32_t seed = 1;
    std::vector<std::unique_ptr<Source>> srcs;
    double timeSec = 0;

    Source& add(Source s) { srcs.push_back(std::make_unique<Source>(std::move(s))); return *srcs.back(); }

    float gauss() {   // xorshift + Box-Muller
        auto u = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed + 0.5) / 4294967296.0; };
        return (float)(std::sqrt(-2 * std::log(u())) * std::cos(2 * kPi * u()));
    }
    // n samples at the radio's centre. tuning: no signal (the tuner is settling): noise only.
    void mix(double centreHz, bool tuning, cf32* out, size_t n) {
        std::fill(out, out + n, cf32(0, 0));
        std::vector<cf32> tmp(n);
        const double t1 = timeSec + (double)n / rate;
        for (auto& sp : srcs) {
            Source& s = *sp;
            if (t1 <= s.startSec || timeSec >= s.endSec) continue;
            if (!s.gen) s.gen = s.make(rate);
            (*s.gen)(tmp.data(), n);
            // block-wise on/off at the block edges (blocks are short against calls)
            if (timeSec + 0.5 * (double)n / rate >= s.endSec) continue;
            if (s.imp.mpGain > 0 && s.imp.mpDelayUs > 0) {   // two rays
                const size_t d = std::max<size_t>(1, (size_t)std::lround(s.imp.mpDelayUs * 1e-6 * rate));
                if (s.hist.size() != d) { s.hist.assign(d, cf32(0, 0)); s.histPos = 0; }
                for (size_t i = 0; i < n; i++) {
                    const cf32 x = tmp[i], old = s.hist[s.histPos];
                    s.hist[s.histPos] = x; s.histPos = (s.histPos + 1) % d;
                    s.fadePh += 2 * kPi * s.imp.mpFadeHz / rate;
                    tmp[i] = x + old * (float)s.imp.mpGain * cf32((float)std::cos(s.fadePh), (float)std::sin(s.fadePh));
                }
            }
            const double shift = s.freqHz - centreHz - tunerPpm * 1e-6 * centreHz;
            if (tuning || std::fabs(shift) > rate / 2 - 8000) { s.phase = std::remainder(s.phase + 2 * kPi * shift / rate * (double)n, 2 * kPi); continue; }
            const double w = 2 * kPi * shift / rate;
            for (size_t i = 0; i < n; i++) {
                s.phase += w;
                out[i] += tmp[i] * (float)s.imp.level * cf32((float)std::cos(s.phase), (float)std::sin(s.phase));
            }
            s.phase = std::remainder(s.phase, 2 * kPi);
        }
        if (noiseSnrDb < 99) {
            const double nb = 0.05 * 0.05 / std::pow(10.0, noiseSnrDb / 10);
            const float sg = (float)std::sqrt(nb * rate / 12500 / 2);
            for (size_t i = 0; i < n; i++) out[i] += cf32(sg * gauss(), sg * gauss());
        }
        if (dcSpur > 0) for (size_t i = 0; i < n; i++) out[i] += cf32((float)(0.05 * dcSpur), (float)(0.05 * dcSpur * 0.5));
        timeSec = t1;
    }
};

// ------------------------------------------------------------------------------------------------ the fake radio

struct Radio {
    double dialHz = 0, centreHz = 0, rate = 480000;
    double settleSec = 0.3;
    double settleLeft = 0;
    bool wide = false;           // the capture is a fixed wide span (no retune): a retune action is still executed but the span stays
    struct Tune { double sec, hz; };
    std::vector<Tune> tunes;
    int retunes = 0;
    void tune(double dial, double now) {
        dialHz = dial; centreHz = dial + kTuneOffsetHz; settleLeft = settleSec;
        retunes++; tunes.push_back({now, dial});
    }
    void setCentre(double c) { dialHz = c - kTuneOffsetHz; centreHz = c; }
};

// ------------------------------------------------------------------------------------------------ the rig (the loop of p25_ui.cpp)

struct RigCfg {
    double rate = 480000;
    bool wideSlices = false;     // Phase 1 calls inside the capture are decoded by the whole-span bank (the app's "whole span" / wideFollow)
    bool p2Inside = true;        // Phase 2 calls inside the capture run on the second Phase 2 receiver
    trunk::FollowConfig follow;
    double stepSec = 0.05;
};

struct Rig {
    World world;
    Radio radio;
    trunk::Controller ctl;
    RigCfg cfg;
    P25Receiver rx;                       // the radio's receiver (control channel, or the hopped voice channel)
    P25p2Receiver p2;                     // the Phase 2 receiver (hop: the first mode; inside the capture: the second mode)
    std::unique_ptr<WideModes> wm;
    RxTelemetry wt; uint64_t wseq = 0;
    double now = 0;
    uint64_t tsbkSeen = 0, hopSeq = 0, p2Seq = 0;
    uint64_t hopCall = 0, p2Call = 0;
    bool p2Hop = false, p2Second = false, wasActive = false;
    int p2Slot = 0;
    int p2Seed[3] = {0, 0, 0};
    double chanHome = 0;                  // the control channel start() was given
    double chanHz = 0;                    // the channel the receiver is set to (the control channel the user tuned)
    std::vector<double> wideList;
    // what the tests look at
    std::vector<trunk::Action> executed;                 // every action, in order (Notices too)
    std::vector<std::pair<double, trunk::FollowState>> stateLog;
    uint64_t tsbksFed = 0, hopVoiceReports = 0, p2VoiceReports = 0, wideVoiceReports = 0, terminators = 0;
    int maxSlicesOpen = 0;

    explicit Rig(const RigCfg& c = RigCfg()) : cfg(c) {
        world.rate = c.rate; radio.rate = c.rate;
        rx.setSilent(true); rx.configure(c.rate);
        p2.setSilent(true); p2.configure(c.rate);
        trunk::FollowConfig f = c.follow;
        ctl.follower.setConfig(f);
        if (c.wideSlices) {
            wm = std::make_unique<WideModes>();
            wm->configure(51, c.rate, 0);
            wm->setFollowOnly(true, {});
        }
    }
    bool wideOn() const { return cfg.wideSlices && wm; }

    // The radio is on this dial frequency from the start (the user's tuning); the receiver looks for the channel at dial
    void start(double dialHz, double nowSec = 0) {
        radio.tune(dialHz, nowSec); radio.settleLeft = 0;
        radio.tunes.clear(); radio.retunes = 0;
        applyOffsets(dialHz); chanHome = dialHz;
        ctl.follower.setControlChannel(dialHz, {dialHz});
    }
    // The wide capture: the radio sits at centreHz for good (the user's span); the receiver listens to the channel at ccHz
    void startWide(double centreHz, double ccHz) {
        radio.setCentre(centreHz); radio.wide = true; radio.settleLeft = 0;
        applyOffsets(ccHz);
        if (wm) {   // the bank's raster must run through the channels: its origin is the channels' offset from the centre
            double o = std::fmod(ccHz - centreHz, 12500.0);
            if (o > 6250) o -= 12500; else if (o < -6250) o += 12500;
            wm->configure(51, cfg.rate, o);
            wm->setFollowOnly(true, {});
            wm->setCenterHz(centreHz);
        }
        ctl.follower.setControlChannel(ccHz, {ccHz});
    }
    void applyOffsets(double channelHz) {
        chanHz = channelHz;
        rx.setSignalOffset(channelHz - radio.centreHz);
    }

    void tuneRadio(double dial, bool home) {
        if (radio.wide) { applyOffsets(dial); rx.reset(); return; }   // a wide capture: the receiver moves inside it
        radio.tune(dial, now);
        applyOffsets(dial);
        rx.reset();
        (void)home;
    }

    void p2Start(const trunk::Action& ac) {
        const trunk::SiteInfo& si = ctl.sys.site();
        p2Seed[0] = si.wacn < 0 ? 0 : si.wacn; p2Seed[1] = si.sysId < 0 ? 0 : si.sysId; p2Seed[2] = si.nac >= 0 ? si.nac : p2Seed[1];
        p2.setSystem(p2Seed[0], p2Seed[1], p2Seed[2]);
        p2.setListenSlot(ac.slot + 1);
        p2.startFollow(ac.callId);
        p2.setCallHint(1, -1, -1, true, 0); p2.setCallHint(2, -1, -1, true, 0);
        if (const trunk::CallRecord* cr = ctl.log.byId(ac.callId)) p2.setCallHint(ac.slot + 1, cr->src, cr->tg, cr->group, cr->freqHz);
        p2Call = ac.callId; p2Slot = ac.slot & 1; p2Seq = 0;
    }

    // One step: the radio's samples through the receivers, then the follower
    void step() {
        const size_t n = (size_t)std::lround(cfg.stepSec * cfg.rate);
        std::vector<cf32> buf(n);
        world.mix(radio.centreHz, radio.settleLeft > 0, buf.data(), n);
        radio.settleLeft = std::max(0.0, radio.settleLeft - cfg.stepSec);
        now += (double)n / cfg.rate;
        if (!p2Hop && !wideOn()) rx.feed(buf.data(), n);
        if (p2Hop) {   // the Phase 2 receiver is the first mode: the hop's frequency is the channel
            p2.feed(buf.data(), n);
        } else if (p2Second) {
            p2.feed(buf.data(), n);
        }
        if (wideOn()) wm->feed(51, buf.data(), n);
        ctlStep();
    }

    void ctlStep() {
        using namespace trunk;
        // the control channel messages (when the Phase 1 receiver is the radio's first mode)
        P25Telemetry t;
        const std::vector<P25TsbkRec>* tl = nullptr;
        double sigNow = 0;
        if (wideOn()) {   // the bank decodes the control channel too: its merged telemetry is what the app reads
            wm->telemetry(51, wt, wseq);
            tl = &wt.p25.tsbks; sigNow = wt.p25.timeSec;
        } else if (!p2Hop) {
            rx.telemetry(t, 0);
            tl = &t.tsbks; sigNow = t.timeSec;
        }
        if (tl)
            for (const auto& r : *tl) {
                if (r.seq <= tsbkSeen) continue;
                tsbkSeen = r.seq;
                if (sigNow - r.sigSec > 3.0 && r.opcode >= 0x00 && r.opcode <= 0x06) continue;
                p25::Tsbk k; k.opcode = r.opcode; k.mfid = r.mfid; k.args = r.args;
                ctl.onTsbk(k, r.nac, now); tsbksFed++;
            }
        ctl.setCapture(radio.centreHz, radio.rate, wideOn());
        if (hopCall && !p2Hop && !wideOn() && t.seq != hopSeq) {
            hopSeq = t.seq;
            if (t.now.active) { wasActive = true; hopVoiceReports++; if (t.now.encrypted) ctl.onEncrypted(hopCall, now); else ctl.onVoice(hopCall, now); }
            else if (wasActive) { wasActive = false; terminators++; ctl.onTerminator(hopCall, now); }
        }
        if (p2Call) {
            P25p2Telemetry p;
            p2.telemetry(p, 0);
            if (p.seq != p2Seq && p.followTag == p2Call) {
                p2Seq = p.seq;
                if (p.wacn != p2Seed[0] || p.sysId != p2Seed[1] || p.colorCode != p2Seed[2]) p2.setSystem(p2Seed[0], p2Seed[1], p2Seed[2]);
                const P25p2Call& c = p.now[p2Slot];
                if (c.active && c.cryptoKnown && c.encrypted) ctl.onEncrypted(p2Call, now);
                else if (p.timeSec - p.voiceSec[p2Slot] < 0.6) { p2VoiceReports++; ctl.onVoice(p2Call, now); }
            }
        }
        if (wideOn()) {   // a Phase 1 call decoded inside the capture lives on its carrier
            if (getenv("P25SIM_DEBUG") && (int)(now * 10) % 10 == 0) {
                printf("t=%.1f ready %d live %zu wide rows %zu:", now, (int)wm->ready(51), wm->liveCount(51), wt.wide.size());
                for (const auto& c : wt.wide) printf(" [%.0f act%d st%d idle%.1f]", radio.centreHz + c.hz, c.active, c.state, c.idleSec);
                printf("\n");
            }
            for (const CallRecord& cr : ctl.log.records()) {
                if (!cr.active || !cr.followed || cr.dmr || cr.slot >= 0 || cr.id == hopCall || !ctl.follower.inSpan(cr.freqHz)) continue;
                const WideCarrier* nearC = nullptr; double bd = 9000;
                for (const auto& c : wt.wide) { const double d = std::fabs(radio.centreHz + c.hz - cr.freqHz); if (d < bd) { bd = d; nearC = &c; } }
                if (!nearC) continue;
                if (nearC->active) { wideVoiceReports++; ctl.onVoice(cr.id, now); }
                else if (nearC->idleSec > 1.0) ctl.onTerminator(cr.id, now);
            }
        }
        ctl.tick(now);
        maxSlicesOpen = std::max(maxSlicesOpen, ctl.follower.activeSlices());
        for (const Action& ac : ctl.takeActions()) {
            executed.push_back(ac);
            switch (ac.kind) {
            case Action::Retune:
                if (p2Second) { p2Second = false; p2Call = 0; }
                if (ac.tdma) { p2Start(ac); p2Hop = true; }
                else { p2Hop = false; p2Call = 0; }
                hopCall = ac.callId; hopSeq = 0; wasActive = false;
                tuneRadio(ac.freqHz, ac.callId == 0);
                if (ac.tdma) { p2.setSignalOffset(ac.freqHz - radio.centreHz); }
                break;
            case Action::OpenSlice:
                if (ac.tdma && cfg.p2Inside) { p2Start(ac); p2.setSignalOffset(ac.freqHz - radio.centreHz); p2Second = true; }
                else if (ac.tdma) { ctl.onTerminator(ac.callId, now); p2Call = 0; }
                break;
            case Action::CloseSlice:
                if (ac.tdma && ac.callId == p2Call) { p2Second = false; p2Call = 0; }
                break;
            case Action::Notice: ctl.messages.push_back(ac.text); break;
            default: break;
            }
        }
        if (wideOn()) {
            std::vector<double> v{chanHz > 0 ? chanHz : radio.dialHz};
            const double cc = ctl.follower.currentControlHz();
            if (cc > 0 && std::fabs(cc - v[0]) > 1) v.push_back(cc);
            for (double f : ctl.follower.sliceFreqs(false)) v.push_back(f);
            if (v != wideList) { wideList = v; wm->setFollowOnly(true, v); }
        }
        if (stateLog.empty() || stateLog.back().second != ctl.follower.state()) stateLog.push_back({now, ctl.follower.state()});
    }

    // Run until time tSec (absolute) or until stop() says so; returns the time reached
    double runUntil(double tSec, const std::function<bool()>& stop = nullptr) {
        while (now < tSec) { step(); if (stop && stop()) break; }
        return now;
    }
    bool sawState(trunk::FollowState s) const { for (const auto& e : stateLog) if (e.second == s) return true; return false; }
    // the time of the first retune home (to the control channel the radio started on) after tSec; 1e9 when none
    double tunes_home_after(double tSec) const { for (const auto& t : radio.tunes) if (t.sec >= tSec && std::fabs(t.hz - chanHome) < 1) return t.sec; return 1e9; }
    int countActions(trunk::Action::Kind k) const { int c = 0; for (const auto& a : executed) c += a.kind == k; return c; }
    const trunk::CallRecord* callForTg(int tg) const {
        const trunk::CallRecord* best = nullptr;
        for (const auto& r : ctl.log.records()) if (r.tg == tg) best = &r;
        return best;
    }
};

// ------------------------------------------------------------------------------------------------ scenario builders

// A control channel on the air from 0: the status and identifier messages all the time, each window's grants only between its from and to, and not
// at all in the outages (a CC loss; it comes back with a fresh signal when the outage ends).
struct GrantWin { std::vector<Grant> grants; double from = 0, to = 0; };
inline void addControl(World& w, double freqHz, const CcSpec& base, const std::vector<GrantWin>& wins,
                       const std::vector<std::pair<double, double>>& outages = {}, double endSec = 1e9, Impair imp = Impair()) {
    std::vector<double> cut{0};
    for (const auto& g : wins) { cut.push_back(g.from); cut.push_back(g.to); }
    for (const auto& o : outages) { cut.push_back(o.first); cut.push_back(o.second); }
    std::sort(cut.begin(), cut.end());
    cut.erase(std::unique(cut.begin(), cut.end()), cut.end());
    cut.push_back(endSec);
    for (size_t i = 0; i + 1 < cut.size(); i++) {
        const double a = cut[i], b = cut[i + 1];
        if (b <= a || a >= endSec) continue;
        bool out = false;
        for (const auto& o : outages) if (a >= o.first - 1e-9 && b <= o.second + 1e-9) out = true;
        if (out) continue;
        CcSpec seg = base;
        seg.grants.clear();
        for (const auto& g : wins) if (a >= g.from - 1e-9 && b <= g.to + 1e-9) seg.grants.insert(seg.grants.end(), g.grants.begin(), g.grants.end());
        const double len = std::min(b - a, 30.0) + 0.3;
        w.add(phase1("cc", freqHz, {ccTx(seg, len)}, a, std::min(b, endSec), true, imp));
    }
}
inline void addControl(World& w, double freqHz, const CcSpec& base, const std::vector<Grant>& grants, double from, double to,
                       const std::vector<std::pair<double, double>>& outages = {}, double endSec = 1e9, Impair imp = Impair()) {
    addControl(w, freqHz, base, std::vector<GrantWin>{{grants, from, to}}, outages, endSec, imp);
}

inline void addVoice(World& w, double freqHz, int nac, int tg, int src, double startSec, double durSec, bool noTerm = false, Impair imp = Impair(), bool emergency = false) {
    w.add(phase1("voice", freqHz, {voiceTx(nac, tg, src, durSec, emergency, false, noTerm)}, startSec, startSec + durSec + 1.0, false, imp));
}

} // namespace p25sim
