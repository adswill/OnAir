// Single-radio trunking (docs/TRUNK1-3.9.md): one wide capture holds a trunked system's control channel and several voice channels, and the
// control channel and N simultaneous calls are decoded from that single stream. Per protocol, with the generators that exist:
//   P25         control channel (TSBK grants) at an offset + 4 Phase 1 calls, one of them next to the DC spike; tuner ppm error, carrier offsets,
//               symbol clock errors, noise, a DC spur. Then 2 voice slots for 4 calls (busy), a grant outside the capture (needs X MHz).
//   DMR, NXDN   3 carriers of the mode's generator inside one capture, grants given to the pool (no Tier III / NXDN trunking control channel
//               generator exists): each slot decodes voice frames at the same time.
//   Type II     the SmartNet system generator: control channel next to a DC spur + 5 analog calls at once, 4 then 5 slots, tones per call.
//   MPT 1327    FFSK control channel (GTCs) + 3 analog traffic channels at once from the channel plan, one call outside the capture.
//   EDACS       the system generator with 3 overlapping calls: each in its own voice slot with its own tone.
//   Coverage    the plan for an RTL-SDR and a HackRF, the "outside the capture (needs X MHz)" text, radio limits.
// Generated signals only: no real trunked system was recorded for this.
#include "../p25/p25_scenario.h"
#include "dect2/edacs/edacs_gen.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/mpt1327/mpt1327_gen.h"
#include "dect2/smartnet/smartnet_gen.h"
#include "dect2/smartnet/smartnet_rx.h"
#include "dect2/smartnet/smartnet_tel.h"
#include "dect2/trunkpool/coverage.h"
#include "dect2/trunkpool/one_radio.h"
#include "dect2/trunkpool/slice_ddc.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
namespace tp = dect2::tpool;
#include "../common/test_util.h"

static double toneShare(const std::vector<float>& a, double f, double fs) {   // the share of the (non-zero) signal power at f
    std::vector<float> nz;
    for (float v : a) if (v != 0) nz.push_back(v);
    if (nz.size() < 4800) return 0;
    double c = 2 * std::cos(2 * M_PI * f / fs), s1 = 0, s2 = 0, tot = 0;
    for (float v : nz) { const double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; tot += (double)v * v; }
    return tot > 0 ? 2 * (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)nz.size() * tot) : 0;
}

static double cpuNow() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }

// ------------------------------------------------------------------------------------------------ coverage

static void testCoverage() {
    CHECK(tp::radioMaxRateHz("RTL-SDR Blog V4") == 2.4e6 && tp::radioMaxRateHz("HackRF One") == 20e6 && tp::radioMaxRateHz("Airspy HF+") < 1e6 && tp::radioMaxRateHz("airspy mini") == 10e6, "radio limits");
    CHECK(tp::radioMaxRateHz("something else") == 0, "unknown radio");
    CHECK(tp::insideCapture(851.9e6, 851.5e6, 2.4e6) && !tp::insideCapture(852.6e6, 851.5e6, 2.4e6), "inside / outside");
    const std::string t = tp::outsideText(855.0e6, 851.0e6, 851.5e6, 12500, 2.4e6);
    CHECK(t.find("outside the capture (needs 4.8 MHz") == 0 && t.find("this radio reaches 2.4 MHz") != std::string::npos, "text: %s", t.c_str());
    CHECK(tp::outsideText(852.0e6, 851.0e6, 851.5e6) == "outside the capture (needs 1.2 MHz)", "text: %s", tp::outsideText(852.0e6, 851.0e6, 851.5e6).c_str());
    // a site: control channel 851.0125, busy channels near it, two rare ones far away
    tp::ChannelUsage u;
    const double ch[] = {851.2625e6, 851.5125e6, 851.7625e6, 852.0125e6, 851.3875e6, 856.0125e6, 858.0125e6};
    const int uses[] = {30, 25, 20, 15, 10, 3, 2};
    for (int i = 0; i < 7; i++) for (int k = 0; k < uses[i]; k++) u.add(ch[i], k);
    CHECK(u.channels().front().hz == ch[0] && u.channels().front().grants == 30, "most used first");
    tp::CoverageInput in; in.controlHz = 851.0125e6; in.channels = u.channels(); in.maxRateHz = 2.4e6;
    const tp::CoveragePlan p = tp::planCoverage(in);
    CHECK(p.ok && p.controlInside && p.covered == 5 && p.total == 7 && p.rateHz <= 2.4e6, "RTL plan: %s", p.summary.c_str());
    CHECK(p.dcClear, "the plan keeps every channel off the DC spike: centre %.4f", p.centreHz / 1e6);
    for (double f : {in.controlHz, ch[0], ch[1], ch[2], ch[3], ch[4]}) CHECK(tp::insideCapture(f, p.centreHz, p.rateHz), "%.4f inside", f / 1e6);
    for (double f : {in.controlHz, ch[0], ch[1], ch[2], ch[3], ch[4]}) CHECK(std::fabs(f - p.centreHz) >= in.dcGuardHz, "%.4f off DC (%.0f Hz)", f / 1e6, std::fabs(f - p.centreHz));
    CHECK(p.outside.size() == 2 && p.outside[0] == ch[5] && p.rateAllHz > 8e6 && p.summary.find("this radio reaches 2.4 MHz") != std::string::npos, "outside list: %s", p.summary.c_str());
    in.maxRateHz = 20e6;
    const tp::CoveragePlan h = tp::planCoverage(in);
    CHECK(h.covered == 7 && h.controlInside && h.rateHz <= 20e6 && h.rateHz >= h.rateAllHz, "HackRF covers all: %s", h.summary.c_str());
    printf("coverage RTL: %s\ncoverage HackRF: %s\n", p.summary.c_str(), h.summary.c_str());
    // a narrow radio: only the control channel and the nearest channel fit
    in.maxRateHz = 0.3e6;
    const tp::CoveragePlan n = tp::planCoverage(in);
    CHECK(n.controlInside && n.covered <= 1, "narrow: %s", n.summary.c_str());
    // no channel learned yet
    tp::CoverageInput e; e.controlHz = 0;
    CHECK(!tp::planCoverage(e).ok, "nothing known: no plan");
}

// ------------------------------------------------------------------------------------------------ the channel filter

static void testDdc() {
    // a tone at +300 kHz and a strong one at +330 kHz in 2.4 Msps: the slice at +300 kHz keeps the first and rejects the second
    for (double rate : {240e3, 1.2e6, 2.4e6, 10e6, 20e6}) {
        tp::SliceDdc d;
        const double off = std::min(300e3, rate * 0.3);
        CHECK(d.configure(rate, off), "%.0f: configure", rate);
        CHECK(d.outRate() >= 48000 && d.outRate() < 100000, "%.0f: out %.0f", rate, d.outRate());
        const size_t n = (size_t)(rate * 0.05);
        std::vector<cf32> x(n);
        for (size_t i = 0; i < n; i++) {
            const double t = (double)i / rate;
            x[i] = std::polar(0.1f, (float)(2 * M_PI * (off + 1000) * t)) + std::polar(1.0f, (float)(2 * M_PI * (off + 30000) * t));
        }
        std::vector<cf32> y;
        for (size_t i = 0; i < n; i += 4096) { const auto& o = d.process(x.data() + i, std::min<size_t>(4096, n - i)); y.insert(y.end(), o.begin(), o.end()); }
        // power at +1 kHz against +30 kHz (aliased) in the output, after the filter has settled
        double p1 = 0, p2 = 0;
        const size_t s0 = y.size() / 2;
        std::complex<double> a1 = 0, a2 = 0;
        for (size_t i = s0; i < y.size(); i++) {
            const double t = (double)i / d.outRate();
            a1 += std::complex<double>(y[i]) * std::polar(1.0, -2 * M_PI * 1000 * t);
            a2 += std::complex<double>(y[i]) * std::polar(1.0, -2 * M_PI * std::remainder(30000.0, d.outRate()) * t);
        }
        p1 = std::norm(a1); p2 = std::norm(a2);
        const double rej = 10 * std::log10((p1 + 1e-30) / (p2 + 1e-30)) + 20;   // the wanted tone is 20 dB weaker at the input
        CHECK(rej > 50, "%.0f: the neighbour is only %.1f dB down", rate, rej);
        if (rate == 2.4e6) printf("channel filter at 2.4 Msps: out %.0f Hz, neighbour 30 kHz away %.1f dB down\n", d.outRate(), rej);
    }
}

// ------------------------------------------------------------------------------------------------ P25

struct P25Out { int maxActive = 0; std::map<int, uint64_t> frames; std::map<int, std::string> notes; uint64_t tsbks = 0; double cpuPerSlot = 0, ccCpu = 0; bool busySeen = false; bool dcNotch = false; };

static P25Out runP25(int slots, double secs, bool outside) {
    using namespace p25sim;
    P25Out o;
    const double rate = 2.4e6, C = 851.5e6;
    World w; w.rate = rate; w.noiseSnrDb = 24; w.dcSpur = 0.6; w.tunerPpm = 1.5; w.seed = 7;
    Band b = band800();
    CcSpec cc; cc.bands = {b}; cc.ccChannel = b.channel(1);
    const int chans[4] = {8, 40, 52, 75};   // -394, +6 (on the DC spike), +156, +444 kHz from the centre
    for (int k = 0; k < 4; k++) cc.grants.push_back({b.channel(chans[k]), 101 + k, 9001 + k, false});
    if (outside) cc.grants.push_back({b.channel(200), 105, 9005, false});   // 853.5 MHz: 2 MHz above the centre, outside 2.4 Msps
    Impair ic; ic.cfoHz = 350; ic.clockPpm = 20;
    w.add(phase1("cc", b.freq(1), {ccTx(cc, 60)}, 0, 1e9, true, ic));
    for (int k = 0; k < 4; k++) {
        Impair iv; iv.cfoHz = (k % 2 ? -450 : 600); iv.clockPpm = (k - 1.5) * 30;
        w.add(phase1("v" + std::to_string(k), b.freq(chans[k]), {voiceTx(cc.nac, 101 + k, 9001 + k, secs)}, 0.2 + 0.1 * k, secs + 1, false, iv));
    }
    const double ccHz = b.freq(1);
    tp::SliceDdc ccDdc; ccDdc.configure(rate, ccHz - C);
    P25Receiver ccRx; ccRx.setSilent(true); ccRx.configure(ccDdc.outRate()); ccRx.setSignalOffset(0);
    trunk::Controller ctl;
    tp::OneRadio one("p25", slots);
    one.configure(rate); one.setCentreHz(C); one.setControlHz(ccHz);
    ctl.grantTap = [&](const trunk::Grant& g, double) { one.onGrant(tp::grantFromTrunk(g)); };
    const size_t n = (size_t)(rate * 0.05);
    std::vector<cf32> buf(n);
    uint64_t seen = 0;
    double now = 0, slotSec = 0, ccCpu = 0;
    for (double t = 0; t < secs + 0.5; t += 0.05) {
        w.mix(C, false, buf.data(), n);
        now += 0.05;
        const double c0 = cpuNow();
        const auto& c = ccDdc.process(buf.data(), n);
        ccRx.feed(c.data(), c.size());
        ccCpu += cpuNow() - c0;
        P25Telemetry tl;
        if (ccRx.telemetry(tl, 0))
            for (const auto& r : tl.tsbks) {
                if (r.seq <= seen) continue;
                seen = r.seq;
                p25::Tsbk k; k.opcode = r.opcode; k.mfid = r.mfid; k.args = r.args;
                ctl.onTsbk(k, r.nac, now); o.tsbks++;
            }
        ctl.tick(now); ctl.takeActions();
        one.feed(buf.data(), n);
        o.dcNotch = o.dcNotch || one.voice().dcNotchOn();
        const int act = one.activeCalls();
        o.maxActive = std::max(o.maxActive, act);
        slotSec += 0.05 * act;
        for (const tp::OneRadioCall& r : one.calls()) {
            if (r.active && r.voiceSlot >= 0) o.frames[r.tg] = std::max(o.frames[r.tg], one.voice().sliceVoiceFrames(r.voiceSlot));
            if (!r.note.empty()) o.notes[r.tg] = r.note;
            if (r.note.find("busy: every voice slot") == 0) o.busySeen = true;
        }
    }
    o.cpuPerSlot = slotSec > 0 ? one.cpuShare() * now / slotSec : 0;
    o.ccCpu = ccCpu / now;
    return o;
}

static void testP25() {
    P25Out a = runP25(4, 6.0, true);
    CHECK(a.tsbks > 150, "control channel at -481 kHz with the DC spur: %llu TSBKs", (unsigned long long)a.tsbks);
    CHECK(a.maxActive == 4, "4 calls at once from one capture: %d", a.maxActive);
    for (int tg = 101; tg <= 104; tg++) CHECK(a.frames[tg] >= 150, "TG %d: %llu voice frames (one slot each)", tg, (unsigned long long)a.frames[tg]);   // 50 frames/s
    CHECK(a.dcNotch, "the slice on the DC spike switched the notch on");
    CHECK(a.notes[105].find("outside the capture (needs") == 0, "outside call: '%s'", a.notes[105].c_str());
    printf("P25 one radio, 4 slots: %d at once, frames %llu %llu %llu %llu, %llu TSBKs; cpu %.2f %% of a core per voice slot, control channel %.2f %% (2.4 Msps)\n", a.maxActive,
           (unsigned long long)a.frames[101], (unsigned long long)a.frames[102], (unsigned long long)a.frames[103], (unsigned long long)a.frames[104], (unsigned long long)a.tsbks,
           100 * a.cpuPerSlot, 100 * a.ccCpu);
    printf("P25 outside call note: %s\n", a.notes[105].c_str());
    P25Out b = runP25(2, 4.0, false);
    CHECK(b.maxActive == 2 && b.busySeen, "2 slots for 4 calls: %d at once, busy %d", b.maxActive, (int)b.busySeen);
}

// ------------------------------------------------------------------------------------------------ DMR and NXDN (generator carriers, grants given)

static void testCarriers(int stdMode, tp::Proto proto, const char* name) {
    const double rate = 1.2e6, C = 460.0e6;
    const double off[3] = {-312500, 12500, 262500};
    std::vector<std::unique_ptr<ModeSynth>> g;
    for (int k = 0; k < 3; k++) {
        SynthConfig c; c.mode = stdMode; c.snrDb = 99; c.cfoHz = off[k] + (k - 1) * 300.0; c.sroPpm = (k - 1) * 25.0;
        c.modeOpt[4] = k + 1;   // DMR: a different seed per carrier
        if (stdMode == 11) c.modeOpt[3] = 2;   // DMR: busy traffic
        g.push_back(makeModeSynth(stdMode, c, rate));
        if (!g.back()) { CHECK(false, "%s: no generator", name); return; }
    }
    const size_t n = (size_t)(rate * 0.05);
    std::vector<cf32> buf(n), tmp(n);
    float scale[3] = {1, 1, 1};
    for (int k = 0; k < 3; k++) {   // each carrier at an rms of 0.05
        g[(size_t)k]->generate(tmp.data(), n);
        double p = 0; for (const cf32& v : tmp) p += std::norm(v);
        scale[k] = (float)(0.05 / std::sqrt(p / (double)n + 1e-20));
    }
    std::mt19937 rng(3);
    const float sg = (float)std::sqrt(0.05 * 0.05 / std::pow(10.0, 22 / 10.0) * rate / 12500 / 2);   // 22 dB in 12.5 kHz
    std::normal_distribution<float> nd(0.f, sg);
    tp::OneRadio one(name, 4);
    one.configure(rate); one.setCentreHz(C);
    std::map<int, uint64_t> frames;
    int maxActive = 0;
    for (double t = 0; t < 6.0; t += 0.05) {
        for (size_t i = 0; i < n; i++) buf[i] = cf32(nd(rng), nd(rng)) + cf32(0.02f, 0.01f);   // noise and a DC spur
        for (int k = 0; k < 3; k++) { g[(size_t)k]->generate(tmp.data(), n); for (size_t i = 0; i < n; i++) buf[i] += tmp[i] * scale[k]; }
        if (std::fmod(t + 1e-9, 1.0) < 0.05)   // a grant (repeat) for each carrier every second
            for (int k = 0; k < 3; k++) { tp::GrantEvent e; e.proto = proto; e.tg = 200 + k; e.src = 7000 + k; e.freqHz = C + off[k]; one.onGrant(e); }
        one.feed(buf.data(), n);
        maxActive = std::max(maxActive, one.activeCalls());
        for (const tp::OneRadioCall& r : one.calls()) if (r.active && r.voiceSlot >= 0) frames[r.tg] = std::max(frames[r.tg], one.voice().sliceVoiceFrames(r.voiceSlot));
    }
    CHECK(maxActive == 3, "%s: %d calls at once", name, maxActive);
    for (int k = 0; k < 3; k++) CHECK(frames[200 + k] >= 20, "%s: carrier %d (%.0f kHz) %llu voice frames", name, k, off[k] / 1e3, (unsigned long long)frames[200 + k]);
    printf("%s one radio: %d at once, voice frames %llu %llu %llu\n", name, maxActive, (unsigned long long)frames[200], (unsigned long long)frames[201], (unsigned long long)frames[202]);
}

// ------------------------------------------------------------------------------------------------ Motorola Type II

static void testSmartnet() {
    SmartnetGenConfig c;
    c.rate = 1.92e6; c.centreHz = 851.5e6; c.controlHz = 851.5125e6; c.controlChan = 0x014;   // the control channel 12.5 kHz from the centre: next to the DC spur
    c.durSec = 5.0; c.cfoHz = 400; c.clockPpm = 60;
    const int chans[5] = {0x018, 0x01A, 0x020, 0x028, 0x030};   // +112.5 ... +712.5 kHz
    const double tones[5] = {600, 900, 1200, 1500, 1800};
    for (int k = 0; k < 5; k++) {
        SmartnetGenCall x; x.chan = chans[k]; x.tg = 0x1000 + 0x10 * k; x.src = 100 + k; x.startSec = 1.0 + 0.1 * k; x.endSec = 4.0; x.toneHz = tones[k];
        c.calls.push_back(x);
    }
    SmartnetGenCall outCall; outCall.chan = 0x100; outCall.tg = 0x2220; outCall.src = 9; outCall.startSec = 2.0; outCall.endSec = 3.0; outCall.voiceCarrier = false;   // 857.4 MHz: outside
    c.calls.push_back(outCall);
    std::vector<cf32> sig = smartnetSystemSignal(c);
    for (auto& v : sig) v += cf32(0.05f, -0.03f);   // the radio's DC spur, as strong as a weak carrier
    for (int slots : {4, 5}) {
        SmartnetReceiver rx;
        rx.setSilent(true);
        rx.configure(c.rate); rx.setCenterHz(c.centreHz); rx.setSignalOffset(c.controlHz - c.centreHz + c.cfoHz);
        rx.setVoiceSlots(slots);
        std::map<int, std::vector<float>> audio;
        rx.setAudioTap([&](int tg, const float* x, size_t n, int) { auto& v = audio[tg]; v.insert(v.end(), x, x + n); });
        int maxActive = 0;
        bool busy = false;
        std::string farNote;
        SmartnetTelemetry t; uint64_t seq = 0;
        for (size_t i = 0; i < sig.size(); i += 19200) {
            rx.feed(sig.data() + i, std::min<size_t>(19200, sig.size() - i));
            if (rx.telemetry(t, seq)) {
                seq = t.seq;
                int a = 0;
                for (const auto& x : t.calls) { a += x.active && x.followed && x.voiceSlot >= 0; if (x.note.find("busy: every voice slot") == 0) busy = true; if (x.tg == 0x2220 && !x.note.empty()) farNote = x.note; }
                maxActive = std::max(maxActive, a);
            }
        }
        CHECK(t.oswOk > 150, "Type II %d slots: control channel next to the DC spur: %llu OSWs", slots, (unsigned long long)t.oswOk);
        CHECK(maxActive == std::min(slots, 5), "Type II %d slots: %d at once", slots, maxActive);
        if (slots == 4) CHECK(busy, "Type II 4 slots: the fifth call is busy");
        CHECK(farNote.find("outside the capture (needs") == 0, "Type II outside call: '%s'", farNote.c_str());
        int good = 0;
        for (int k = 0; k < 5; k++) good += toneShare(audio[0x1000 + 0x10 * k], tones[k], 48000) > 0.5;
        CHECK(good >= std::min(slots, 5), "Type II %d slots: %d calls with their own tone", slots, good);
        printf("Type II one radio, %d slots: %d at once, %d tones, %llu OSWs; outside: %s\n", slots, maxActive, good, (unsigned long long)t.oswOk, farNote.c_str());
    }
}

// ------------------------------------------------------------------------------------------------ MPT 1327

static void testMpt1327() {
    using namespace dect2::mpt1327;
    const double rate = 1.2e6, C = 170.0e6, ccHz = 169.7e6, sp = 12500;
    const int ctrlCh = 100;
    struct Ch { int ch; int ident; double tone; };
    const Ch calls[3] = {{110, 1201, 700}, {125, 1202, 1100}, {140, 1203, 1600}};   // -175, +12.5 (DC), +200 kHz from the centre
    std::vector<uint64_t> cw;
    for (int i = 0; i < 4; i++) cw.push_back(makeAloha(5, 0, 0, 3, 2, 8, 1));
    for (int rep = 0; rep < 4; rep++) {
        for (const Ch& k : calls) cw.push_back(makeGtc(5, k.ident, 0, k.ch, 2000 + k.ident % 100));
        cw.push_back(makeGtc(5, 1299, 0, 220, 2099));   // channel 220: 1.5 MHz above the control channel, outside the capture
        for (int i = 0; i < 6; i++) cw.push_back(makeAloha(5, 0, 0, 3, 2, 8, 1));
    }
    const std::vector<uint8_t> bits = controlChannelBits(20001, cw);
    const std::vector<float> aud = ffskAudio(bits, rate, false, 0.45f, 1.0 + 40e-6);
    std::vector<cf32> iq = fmModulate(aud, rate, 3000, ccHz - C + 300);
    const size_t N = iq.size();
    for (const Ch& k : calls) {   // the traffic carriers: a tone, on from 0.8 s to the end
        std::vector<float> a(N, 0.f);
        for (size_t i = 0; i < N; i++) a[i] = (float)(0.6 * std::sin(2 * M_PI * k.tone * (double)i / rate));
        const double f = ccHz + (k.ch - ctrlCh) * sp;
        const std::vector<cf32> v = fmModulate(a, rate, 4000, f - C - 250);
        for (size_t i = (size_t)(0.8 * rate); i < N; i++) iq[i] += v[i] * 0.5f;
    }
    std::mt19937 g(9);
    std::normal_distribution<float> nd(0.f, 0.03f);
    for (auto& v : iq) v += cf32(nd(g), nd(g)) + cf32(0.04f, 0.02f);
    MptReceiver rx;
    rx.setSilent(true);
    rx.configure(rate); rx.setCenterHz(C); rx.setSignalOffset(ccHz - C);
    rx.setChannelPlan(ctrlCh, sp);
    std::map<int, std::vector<float>> audio;
    rx.setAudioTap([&](int called, const float* x, size_t n, int) { auto& v = audio[called]; v.insert(v.end(), x, x + n); });
    int maxActive = 0;
    std::string farNote;
    MptTelemetry t; uint64_t seq = 0;
    for (size_t i = 0; i < N; i += 12000) {
        rx.feed(iq.data() + i, std::min<size_t>(12000, N - i));
        if (rx.telemetry(t, seq)) {
            seq = t.seq;
            int a = 0;
            for (const MptCall& c : t.calls) { a += c.active && c.followed && c.voiceSlot >= 0; if ((c.called & 8191) == 1299 && !c.note.empty()) farNote = c.note; }
            maxActive = std::max(maxActive, a);
        }
    }
    CHECK(t.messages >= 30, "MPT 1327 control channel at -300 kHz in 1.2 Msps: %llu messages", (unsigned long long)t.messages);
    CHECK(maxActive == 3, "MPT 1327: %d calls at once", maxActive);
    int good = 0;
    for (const Ch& k : calls) {
        const double s = toneShare(audio[(5 << 13) | k.ident], k.tone, 48000);
        good += s > 0.5;
        CHECK(s > 0.5, "MPT 1327 channel %d: tone %.2f", k.ch, s);
    }
    CHECK(farNote.find("outside the capture (needs") == 0, "MPT 1327 outside call: '%s'", farNote.c_str());
    printf("MPT 1327 one radio: %llu messages, %d at once, %d tones; outside: %s\n", (unsigned long long)t.messages, maxActive, good, farNote.c_str());
}

// ------------------------------------------------------------------------------------------------ EDACS

static void testEdacs() {
    EdacsGenConfig c;
    for (int i = 0; i < 10; i++) c.lcnFreqHz.push_back(856.000e6 + 25e3 * i);
    c.rate = 960000; c.centreHz = 856.1e6; c.cfoHz = 300; c.durSec = 6;
    const double tones[3] = {800, 1300, 1900};
    for (int k = 0; k < 3; k++) {
        EdacsGenCall x; x.lcn = 3 + 2 * k; x.gid = 300 + k; x.lid = 40 + k; x.startSec = 1.0 + 0.2 * k; x.endSec = 4.5; x.toneHz = tones[k];
        c.calls.push_back(x);
    }
    const std::vector<cf32> s = edacsSystemSignal(c);
    EdacsReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate); rx.setSignalOffset(c.controlHz - c.centreHz + c.cfoHz); rx.setCenterHz(c.centreHz);
    rx.setLcnTable(c.lcnFreqHz);
    rx.setFollow(true);
    rx.setVoiceSlots(4);
    std::map<int, std::vector<float>> audio;
    rx.setSlotAudioTap([&](int gid, const float* x, size_t n, int) { auto& v = audio[gid]; v.insert(v.end(), x, x + n); });
    int maxUsed = 0;
    EdacsTelemetry t; uint64_t seq = 0;
    for (size_t i = 0; i < s.size(); i += 9600) {
        rx.feed(s.data() + i, std::min<size_t>(9600, s.size() - i));
        if (rx.telemetry(t, seq)) { seq = t.seq; maxUsed = std::max(maxUsed, t.slotsInUse); }
    }
    CHECK(maxUsed == 3, "EDACS: %d calls in voice slots at once", maxUsed);
    int good = 0;
    for (int k = 0; k < 3; k++) { const double v = toneShare(audio[300 + k], tones[k], 48000); good += v > 0.5; CHECK(v > 0.5, "EDACS group %d: tone %.2f", 300 + k, v); }
    printf("EDACS one radio: %d at once, %d tones\n", maxUsed, good);
}

int main() {
    testCoverage();
    testDdc();
    testP25();
    testCarriers(11, tp::Proto::DmrTier3, "DMR");
    testCarriers(40, tp::Proto::Nxdn, "NXDN");
    testSmartnet();
    testMpt1327();
    testEdacs();
    return testSummary("trunkpool onerig");
}
