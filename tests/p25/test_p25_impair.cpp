// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 Phase 1 trunking under real-world impairments. A busy synthetic control channel (site broadcasts, identifier update, a new voice grant every
// 3 s and the update messages of the calls in progress) goes through carrier offsets (also with the channel off centre, as the app tunes it), a
// noise sweep, a simulcast echo, Rayleigh fading, interference bursts and a sample drop-out, then the receiver, the TSBK decoder and the trunking
// controller. Measured per scenario: time to the first control message, TSBK success, grants seen and followed, false decodes (a decoded message
// that was never sent). Silent. Set P25_IMPAIR_TABLE=1 to print the numbers of every scenario.
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/trunk.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {

struct Scn {
    const char* name;
    double snr = 30, cfo = 0, dial = 0, ppm = 0, rate = 250000;
    double echoUs = 0, echoDb = -6, echoDf = 0;   // simulcast: a second copy delayed by echoUs, echoDb weaker, its carrier echoDf Hz away
    double fadeHz = 0;                            // Rayleigh fading with this maximum Doppler
    double burstDuty = 0;                         // interference: this fraction of the time 10 dB above the signal, in 40 ms bursts
    double dropSec = 0;                           // a sample drop-out (zeros) of this length every 8 s
};

struct Result {
    double firstTsbk = -1, firstGrant = -1;
    int blocksSent = 0, blocksOk = 0, falseDec = 0, callsTotal = 0, callsSeen = 0, callsFollowed = 0;
    bool siteOk = false;
};

constexpr double kDur = 40;     // seconds of control channel
constexpr double kCallEvery = 3, kCallLen = 4;
constexpr int kCalls = 12;

struct Traffic {
    std::vector<std::vector<std::pair<int, uint64_t>>> script;
    std::set<std::pair<int, uint64_t>> sent;
    double frameSec = 0;
};

Traffic makeTraffic() {
    Traffic tr;
    P25GenTx probe;
    probe.kind = P25GenTx::Control;
    probe.durSec = 0.001;
    probe.script = {{{p25::kGrpVGrant, 0}, {p25::kGrpVGrant, 0}, {p25::kGrpVGrant, 0}}};
    std::vector<int8_t> sym;
    p25TxSymbols(probe, sym);
    tr.frameSec = (double)sym.size() / 4800.0;
    const P25GenTx d;
    auto add = [&](std::vector<std::pair<int, uint64_t>> f) { for (auto& m : f) tr.sent.insert(m); tr.script.push_back(f); };
    const uint64_t idenUp = (1ULL << 60) | (100ULL << 51) | (180ULL << 42) | (100ULL << 32) | (uint64_t)std::llround(d.idenBaseHz / 5);
    const uint64_t net = ((uint64_t)d.wacn << 36) | ((uint64_t)d.sysId << 24) | ((uint64_t)d.ctrlChannel << 8) | 0x70;
    const uint64_t rfss = ((uint64_t)d.sysId << 40) | ((uint64_t)d.rfss << 32) | ((uint64_t)d.site << 24) | ((uint64_t)d.ctrlChannel << 8) | 0x70;
    const uint64_t adj = ((uint64_t)d.sysId << 40) | ((uint64_t)d.rfss << 32) | ((uint64_t)d.adjSite << 24) | ((uint64_t)d.adjChannel << 8) | 0x70;
    const int nFrames = (int)(kDur / tr.frameSec) + 4;
    for (int f = 0; f < nFrames; f++) {
        const double t = f * tr.frameSec;
        const int k = (int)(t / kCallEvery);
        const bool startFrame = k < kCalls && t - k * kCallEvery < tr.frameSec;
        auto grantOf = [&](int c) {
            return ((uint64_t)(4ULL << 56)) | ((uint64_t)(0x1000 | (10 + c)) << 40) | ((uint64_t)(1000 + c) << 24) | (uint64_t)(5000 + c);
        };
        if (f % 10 == 0) { add({{p25::kNetSts, net}, {p25::kRfssSts, rfss}, {p25::kIdenUp, idenUp}}); continue; }
        if (f % 10 == 5) { add({{p25::kAdjSts, adj}, {p25::kRfssSts, rfss}, {p25::kNetSts, net}}); continue; }
        if (startFrame) { add({{p25::kGrpVGrant, grantOf(k)}, {p25::kRfssSts, rfss}, {p25::kRfssSts, rfss}}); continue; }
        // updates of the calls in progress (two per message)
        std::vector<int> live;
        for (int c = 0; c < kCalls; c++) if (t >= c * kCallEvery && t < c * kCallEvery + kCallLen) live.push_back(c);
        if (live.empty()) { add({{p25::kRfssSts, rfss}, {p25::kRfssSts, rfss}, {p25::kRfssSts, rfss}}); continue; }
        const int a = live[(size_t)f % live.size()], b = live[(size_t)(f + 1) % live.size()];
        const uint64_t upd = ((uint64_t)(0x1000 | (10 + a)) << 48) | ((uint64_t)(1000 + a) << 32) | ((uint64_t)(0x1000 | (10 + b)) << 16) | (uint64_t)(1000 + b);
        add({{p25::kGrpVGrantUpd, upd}, {p25::kRfssSts, rfss}, {p25::kRfssSts, rfss}});
    }
    return tr;
}

Result run(const Scn& s, const Traffic& tr) {
    Result r;
    P25GenConfig c;
    c.rate = s.rate; c.snrDb = 99; c.cfoHz = s.cfo; c.dialOffsetHz = s.dial; c.clockPpm = s.ppm; c.loop = false; c.leadSec = 0.3;
    P25GenTx tx;
    tx.kind = P25GenTx::Control;
    tx.durSec = kDur;
    tx.script = tr.script;
    c.txs.push_back(tx);
    P25Generator g(c);
    P25GenConfig nc = c;
    nc.snrDb = s.snr;
    const double sigma = P25Generator::noiseSigma(nc);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    std::uniform_real_distribution<double> ud(0, 2 * M_PI);
    // Jakes-style fading: eight sinusoids
    double fph[8], fdop[8];
    for (int i = 0; i < 8; i++) { fph[i] = ud(rng); fdop[i] = s.fadeHz * std::cos(2 * M_PI * (i + 0.5) / 16 + 0.3); }
    const size_t dly = (size_t)std::llround(s.echoUs * 1e-6 * s.rate);
    std::vector<cf32> hist(dly + 1, cf32(0, 0));
    size_t hp = 0;
    const double echoG = std::pow(10.0, s.echoDb / 20);

    P25Receiver rx;
    rx.setSilent(true);
    rx.setSignalOffset(s.dial);
    rx.configure(s.rate);
    trunk::Controller ctl;
    ctl.setCapture(851.2e6, 2.4e6, true);
    trunk::FollowConfig fc;
    ctl.follower.setConfig(fc);

    std::set<uint64_t> seen;
    uint64_t lastSeq = 0;
    std::vector<cf32> buf(25000);
    int64_t nSamp = 0;
    double t = 0;
    for (double sec = 0; sec < kDur + 1.5; sec += buf.size() / s.rate) {
        g.generate(buf.data(), buf.size());
        for (size_t i = 0; i < buf.size(); i++, nSamp++) {
            const double ts = (double)nSamp / s.rate;
            cf32 x = buf[i];
            if (dly) {
                const cf32 d = hist[hp];
                hist[hp] = x; hp = (hp + 1) % hist.size();
                const double ph = 2 * M_PI * s.echoDf * ts;
                x += (float)echoG * d * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            if (s.fadeHz > 0) {
                double re = 0, im = 0;
                for (int k = 0; k < 8; k++) { const double a = 2 * M_PI * fdop[k] * ts + fph[k]; re += std::cos(a); im += std::sin(a); }
                x *= cf32((float)(re / 2.0), (float)(im / 2.0));   // unit mean power: sum of 8 / 2 has power 2... scaled below
                x *= 0.7071f;
            }
            if (s.dropSec > 0 && std::fmod(ts, 8.0) > 7.0 && std::fmod(ts, 8.0) < 7.0 + s.dropSec) x = cf32(0, 0);
            if (sigma > 0) x += cf32(sigma * nd(rng), sigma * nd(rng));
            if (s.burstDuty > 0 && std::fmod(ts, 0.4) < 0.4 * s.burstDuty) {
                const float bs = (float)(c.amp * 3.0);
                x += cf32(bs * nd(rng), bs * nd(rng));
            }
            buf[i] = x;
        }
        rx.feed(buf.data(), buf.size());
        P25Telemetry tel;
        if (rx.telemetry(tel, lastSeq)) {
            lastSeq = tel.seq;
            for (const auto& rec : tel.tsbks) {
                if (!seen.insert(rec.seq).second) continue;
                r.blocksOk++;
                if (r.firstTsbk < 0) r.firstTsbk = rec.sigSec;
                if (!tr.sent.count({rec.opcode, rec.args}) || rec.nac != c.txs[0].nac) r.falseDec++;
                p25::Tsbk k;
                k.opcode = rec.opcode; k.mfid = rec.mfid; k.args = rec.args;
                ctl.onTsbk(k, rec.nac, t);
                if (r.firstGrant < 0 && ctl.sys.grants() > 0) r.firstGrant = rec.sigSec;
            }
        }
        t += buf.size() / s.rate;
        ctl.tick(t);
    }
    r.blocksSent = (int)(kDur / tr.frameSec) * 3;
    r.siteOk = ctl.sys.site().valid && ctl.sys.site().site == c.txs[0].site && ctl.sys.iden(1).valid;
    r.callsTotal = kCalls;
    std::set<int> seenTg, folTg;
    for (const auto& rec : ctl.log.records()) {
        if (rec.tg >= 1000 && rec.tg < 1000 + kCalls) seenTg.insert(rec.tg);
        if (rec.followed && rec.tg >= 1000 && rec.tg < 1000 + kCalls) folTg.insert(rec.tg);
    }
    r.callsSeen = (int)seenTg.size();
    r.callsFollowed = (int)folTg.size();
    return r;
}

} // namespace

int main() {
    const bool table = getenv("P25_IMPAIR_TABLE") != nullptr;
    const Traffic tr = makeTraffic();
    std::vector<Scn> scn;
    auto S = [&](Scn s) { scn.push_back(s); };
    { Scn s{"clean"}; S(s); }
    { Scn s{"cfo +1 kHz"}; s.cfo = 1000; S(s); }
    { Scn s{"cfo -2 kHz"}; s.cfo = -2000; S(s); }
    { Scn s{"cfo +2 kHz, off centre 50 kHz"}; s.cfo = 2000; s.dial = 50000; S(s); }
    { Scn s{"cfo -1.5 kHz, off centre -80 kHz, 1 Msps"}; s.cfo = -1500; s.dial = -80000; s.rate = 1000000; S(s); }
    { Scn s{"cfo +5 kHz"}; s.cfo = 5000; S(s); }
    { Scn s{"cfo -6 kHz, off centre 50 kHz"}; s.cfo = -6000; s.dial = 50000; S(s); }
    { Scn s{"clock +50 ppm, cfo +1 kHz"}; s.ppm = 50; s.cfo = 1000; S(s); }
    { Scn s{"snr 12 dB"}; s.snr = 12; S(s); }
    { Scn s{"snr 8 dB, cfo +1.5 kHz"}; s.snr = 8; s.cfo = 1500; S(s); }
    { Scn s{"snr 5 dB"}; s.snr = 5; S(s); }
    { Scn s{"simulcast echo 15 us -6 dB"}; s.echoUs = 15; s.echoDb = -6; s.echoDf = 20; S(s); }
    { Scn s{"simulcast echo 40 us -6 dB"}; s.echoUs = 40; s.echoDb = -6; s.echoDf = 20; S(s); }
    { Scn s{"simulcast echo 40 us -3 dB, snr 20"}; s.echoUs = 40; s.echoDb = -3; s.echoDf = 50; s.snr = 20; S(s); }
    { Scn s{"fading 5 Hz, snr 20"}; s.fadeHz = 5; s.snr = 20; S(s); }
    { Scn s{"fading 60 Hz, snr 20"}; s.fadeHz = 60; s.snr = 20; S(s); }
    { Scn s{"interference bursts 10 %"}; s.burstDuty = 0.1; S(s); }
    { Scn s{"drop-out 0.3 s every 8 s"}; s.dropSec = 0.3; S(s); }

    if (table) printf("%-44s %8s %8s %7s %6s %6s %6s %6s\n", "scenario", "first(s)", "grant(s)", "blocks%", "false", "seen", "folld", "site");
    for (const Scn& s : scn) {
        const Result r = run(s, tr);
        const double pct = 100.0 * r.blocksOk / std::max(1, r.blocksSent);
        if (table) printf("%-44s %8.2f %8.2f %6.1f%% %6d %3d/%-2d %3d/%-2d %6s\n", s.name, r.firstTsbk, r.firstGrant, pct, r.falseDec, r.callsSeen, r.callsTotal, r.callsFollowed, r.callsTotal, r.siteOk ? "yes" : "NO");
        CHECK(r.falseDec == 0, "%s: %d false decodes", s.name, r.falseDec);
        const std::string n = s.name;
        const bool harsh = n.find("snr 5") != std::string::npos || n.find("snr 8") != std::string::npos || n.find("echo 40") != std::string::npos || n.find("fading") != std::string::npos ||
                           n.find("bursts") != std::string::npos || n.find("drop") != std::string::npos;
        if (!harsh) {
            CHECK(r.firstTsbk >= 0 && r.firstTsbk < 1.0, "%s: first message after %.2f s", s.name, r.firstTsbk);
            CHECK(pct > 90, "%s: %.1f %% of the control messages", s.name, pct);
            CHECK(r.siteOk, "%s: no site", s.name);
            CHECK(r.callsSeen >= r.callsTotal - 1 && r.callsFollowed >= r.callsTotal - 2, "%s: %d seen, %d followed of %d", s.name, r.callsSeen, r.callsFollowed, r.callsTotal);
        }
    }
    return testSummary("p25 impair");
}
