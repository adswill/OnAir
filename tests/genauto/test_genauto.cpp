// General / Auto (dect2/engine/general_auto.h): the label hysteresis, the Decode / return-to-General rule, Auto-decode, ignore, the labels that
// stay when the view moves away, and a full run: a wideband capture with several signals off the centre goes through the real identifier
// (Capture::startAll) and into the state machine.
#include "dect2/dmr/dmr_gen.h"
#include "dect2/engine/general_auto.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/fm/fm_gen.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/common/stable_random.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::genauto;
#include "../common/test_util.h"

static Detection det(const char* name, const char* mode, double rf, double bw, double conf, bool likely = true, int verify = 1, bool bursty = false) {
    Detection d;
    d.rfHz = rf; d.bwHz = bw; d.snrDb = 20; d.name = name; d.modeId = mode; d.confidence = conf; d.likely = likely; d.verify = verify; d.bursty = bursty;
    d.mem.mode = mode; d.mem.freqHz = (uint64_t)rf; d.mem.name = name;
    return d;
}
static void scan(GeneralAuto& g, double& t, double lo, double hi, const std::vector<Detection>& v) {
    t += 6; g.scanStarted(t); g.scanDone(t, lo, hi, v);
}

static void stateMachine() {
    const double MHz = 1e6;
    // ---- hysteresis: a weak first sighting shows only after a second one; a very sure one at once
    {
        GeneralAuto g; double t = 0;
        scan(g, t, 500 * MHz, 508 * MHz, {det("DVB-T / T2", "dvbt", 504 * MHz, 7.6e6, 0.70)});
        CHECK(g.visible().empty(), "label shown after one weak sighting");
        scan(g, t, 500 * MHz, 508 * MHz, {det("DVB-T / T2", "dvbt", 504.001 * MHz, 7.6e6, 0.72)});
        CHECK(g.visible().size() == 1 && g.visible()[0]->d.name == "DVB-T / T2", "label not shown after two sightings");
        // a miss or two does not remove it, three do
        scan(g, t, 500 * MHz, 508 * MHz, {});
        scan(g, t, 500 * MHz, 508 * MHz, {});
        CHECK(g.visible().size() == 1, "label flickered away after 2 misses");
        scan(g, t, 500 * MHz, 508 * MHz, {det("DVB-T / T2", "dvbt", 504 * MHz, 7.6e6, 0.7)});
        scan(g, t, 500 * MHz, 508 * MHz, {}); scan(g, t, 500 * MHz, 508 * MHz, {}); scan(g, t, 500 * MHz, 508 * MHz, {});
        CHECK(g.visible().empty(), "label kept after 3 misses in a row");
        GeneralAuto h; double t2 = 0;
        scan(h, t2, 500 * MHz, 508 * MHz, {det("DVB-T / T2", "dvbt", 504 * MHz, 7.6e6, 0.95)});
        CHECK(h.visible().size() == 1, "a very sure label should show at once");
        GeneralAuto k; double t3 = 0;   // sure but the verify step failed: waits for the second sighting
        scan(k, t3, 500 * MHz, 508 * MHz, {det("DMR", "dmr", 450 * MHz, 8e3, 0.95, true, -1)});
        CHECK(k.visible().empty(), "failed-verify label shown at once");
    }
    // ---- below the confidence floor: never a label
    {
        GeneralAuto g; double t = 0;
        for (int i = 0; i < 4; i++) scan(g, t, 0, 1e9, {det("FM broadcast", "fm", 98 * MHz, 180e3, 0.30)});
        CHECK(g.labels().empty(), "low-confidence detection became a label");
    }
    // ---- a name that changes needs two scans; a frequency that wobbles stays one label
    {
        GeneralAuto g; double t = 0;
        scan(g, t, 0, 2e9, {det("P25 Phase 1", "p25", 851 * MHz, 8e3, 0.8)});
        scan(g, t, 0, 2e9, {det("P25 Phase 1", "p25", 851 * MHz + 700, 8e3, 0.8)});
        scan(g, t, 0, 2e9, {det("NXDN96", "nxdn", 851 * MHz - 400, 8e3, 0.55)});
        CHECK(g.labels().size() == 1 && g.labels()[0].d.name == "P25 Phase 1", "one odd scan renamed the label (%s)", g.labels()[0].d.name.c_str());
        scan(g, t, 0, 2e9, {det("NXDN96", "nxdn", 851 * MHz, 8e3, 0.6)});
        CHECK(g.labels().size() == 1 && g.labels()[0].d.name == "NXDN96", "repeated new name not adopted");
    }
    // ---- bursty signals are kept longer
    {
        GeneralAuto g; double t = 0;
        for (int i = 0; i < 2; i++) scan(g, t, 0, 2e9, {det("AIS", "ais", 162.025 * MHz, 9e3, 0.8, true, 1, true)});
        for (int i = 0; i < 4; i++) scan(g, t, 0, 2e9, {});
        CHECK(g.visible().size() == 1, "bursty label dropped after 4 misses");
        for (int i = 0; i < 3; i++) scan(g, t, 0, 2e9, {});
        CHECK(g.visible().empty(), "bursty label kept past its limit");
    }
    // ---- several labels at once, adjacent narrow channels stay separate
    {
        GeneralAuto g; double t = 0;
        std::vector<Detection> v = {det("DMR", "dmr", 446.00625 * MHz, 8e3, 0.9), det("DMR", "dmr", 446.01875 * MHz, 8e3, 0.9), det("FM broadcast", "fm", 446.5 * MHz, 180e3, 0.9),
                                    det("POCSAG", "pager", 445.5 * MHz, 10e3, 0.9)};
        scan(g, t, 445 * MHz, 447 * MHz, v); scan(g, t, 445 * MHz, 447 * MHz, v);
        CHECK(g.visible().size() == 4, "%zu labels of 4", g.visible().size());
    }
    // ---- Decode / release / return to General / offered again
    {
        GeneralAuto g; double t = 0;
        const Detection t2 = det("DVB-T / T2", "dvbt", 522 * MHz, 7.6e6, 0.9);
        const Detection nfm = det("NFM voice", "general", 523.5 * MHz, 12e3, 0.8);
        scan(g, t, 518 * MHz, 526 * MHz, {t2, nfm}); scan(g, t, 518 * MHz, 526 * MHz, {t2, nfm});
        CHECK(g.state() == State::General && g.visible().size() == 2, "two labels expected");
        const int id = g.visible()[0]->d.name == "DVB-T / T2" ? g.visible()[0]->id : g.visible()[1]->id;
        t += 1;
        Step s = g.decode(t, id);
        CHECK(s.act == Action::Decode && s.mem.mode == "dvbt" && s.mem.freqHz == 522000000, "decode step: act %d mode %s", (int)s.act, s.mem.mode.c_str());
        CHECK(g.state() == State::Decoding && g.active() && g.active()->id == id, "not decoding");
        CHECK(!g.wantScan(t + 100), "scanning while decoding");
        // still tuning the radio over to the signal: no release inside the settle time, even far away
        CHECK(g.tick(t + 0.5, 100 * MHz).act == Action::None, "released during the settle time");
        // tuned a little off the signal (inside the keep-decoding distance of 25% of 7.6 MHz): stays
        CHECK(g.tick(t + 3, 522.4 * MHz).act == Action::None && g.state() == State::Decoding, "released while close to the signal");
        CHECK(g.tick(t + 4, 521.2 * MHz).act == Action::None, "released at 0.8 MHz off an 8 MHz signal");
        // tuned out of the frequency: back to General
        s = g.tick(t + 5, 530 * MHz);
        CHECK(s.act == Action::ReturnToGeneral && g.state() == State::General && !g.active(), "no return to General (act %d)", (int)s.act);
        CHECK(g.tick(t + 6, 530 * MHz).act == Action::None, "second release");
        // the General scan resumes after the settle time, and only then
        CHECK(!g.wantScan(t + 5.5) && g.wantScan(t + 8), "scan timing after the release");
        // tune back: the scan sees it again at once (the label is remembered and in view)
        t += 8; g.scanStarted(t);
        g.scanDone(t, 518 * MHz, 526 * MHz, {t2});
        const auto vis = g.visible();
        bool haveT2 = false;
        for (const Label* l : vis) if (l->d.name == "DVB-T / T2") haveT2 = true;
        CHECK(haveT2, "signal not offered again when tuned back (%zu)", vis.size());
        // NFM was not in the last scan's detections but it was in view: one miss, still shown
        CHECK(g.labels().size() == 2 && vis.size() == 2, "labels %zu, visible %zu", g.labels().size(), vis.size());
    }
    // ---- labels outside the view are remembered, then forgotten
    {
        GeneralAuto g; double t = 0;
        scan(g, t, 100 * MHz, 102 * MHz, {det("FM broadcast", "fm", 101 * MHz, 180e3, 0.95)});
        scan(g, t, 700 * MHz, 702 * MHz, {});
        CHECK(g.labels().size() == 1 && g.visible().empty(), "label of another view: %zu labels, %zu visible", g.labels().size(), g.visible().size());
        scan(g, t, 100 * MHz, 102 * MHz, {det("FM broadcast", "fm", 101 * MHz, 180e3, 0.9)});
        CHECK(g.visible().size() == 1, "not offered again");
        scan(g, t, 700 * MHz, 702 * MHz, {});
        t += 700; scan(g, t, 700 * MHz, 702 * MHz, {});
        CHECK(g.labels().empty(), "label not forgotten after the time");
    }
    // ---- ignore: gone, and stays gone; a different signal in the same place is still shown
    {
        GeneralAuto g; double t = 0;
        scan(g, t, 400 * MHz, 410 * MHz, {det("POCSAG", "pager", 405 * MHz, 10e3, 0.95)});
        g.ignore(g.visible()[0]->id);
        CHECK(g.visible().empty() && g.ignoredCount() == 1, "ignore");
        scan(g, t, 400 * MHz, 410 * MHz, {det("POCSAG", "pager", 405.0005 * MHz, 10e3, 0.95)});
        scan(g, t, 400 * MHz, 410 * MHz, {det("POCSAG", "pager", 405 * MHz, 10e3, 0.95)});
        CHECK(g.visible().empty(), "ignored label came back");
        scan(g, t, 400 * MHz, 410 * MHz, {det("DMR", "dmr", 405 * MHz, 8e3, 0.95)});
        CHECK(g.visible().size() == 1, "another signal in an ignored place was hidden");
        g.clearIgnored();
        CHECK(g.ignoredCount() == 0, "clearIgnored");
    }
    // ---- Auto-decode: off by default; on, it starts only close to a sure label, and not again right after "back to General"
    {
        GeneralAuto g; double t = 0;
        const Detection t2 = det("DVB-T / T2", "dvbt", 522 * MHz, 7.6e6, 0.95);
        scan(g, t, 518 * MHz, 526 * MHz, {t2});
        CHECK(g.tick(t, 522 * MHz).act == Action::None, "auto-decode ran with the toggle off");
        g.cfg.autoDecode = true;
        CHECK(g.tick(t, 530 * MHz).act == Action::None, "auto-decode far from the signal");
        Step s = g.tick(t, 521.5 * MHz);
        CHECK(s.act == Action::Decode && s.mem.mode == "dvbt", "no auto-decode on the signal");
        s = g.stop(t + 1);
        CHECK(s.act == Action::ReturnToGeneral && g.state() == State::General, "stop");
        CHECK(g.tick(t + 10, 522 * MHz).act == Action::None, "auto-decode restarted right after stop");
        CHECK(g.tick(t + 11, 540 * MHz).act == Action::None, "leaving");
        scan(g, t, 518 * MHz, 526 * MHz, {t2}); scan(g, t, 518 * MHz, 526 * MHz, {t2});
        CHECK(g.tick(t + 20, 522 * MHz).act == Action::Decode, "auto-decode not offered again after leaving and returning");
        // a label the verify step rejected, and a weak one: never auto-decoded
        GeneralAuto h; h.cfg.autoDecode = true; double t2s = 0;
        scan(h, t2s, 0, 1e9, {det("DMR", "dmr", 450 * MHz, 8e3, 0.85, true, -1)}); scan(h, t2s, 0, 1e9, {det("DMR", "dmr", 450 * MHz, 8e3, 0.85, true, -1)});
        scan(h, t2s, 0, 1e9, {det("DMR", "dmr", 450 * MHz, 8e3, 0.85, true, -1)});
        CHECK(h.tick(t2s + 5, 450 * MHz).act == Action::None, "auto-decoded a signal whose verify failed");
    }
    // ---- a label with nothing to decode it has no Decode; a missing label gives a note
    {
        GeneralAuto g; double t = 0;
        Detection d = det("Wi-Fi", "", 2437 * MHz, 18e6, 0.9); d.mem.mode.clear();
        scan(g, t, 2400 * MHz, 2480 * MHz, {d});
        Step s = g.decode(t, g.visible()[0]->id);
        CHECK(s.act == Action::None && !s.note.empty() && g.state() == State::General, "decoded a signal nothing decodes");
        s = g.decode(t, 999);
        CHECK(s.act == Action::None && !s.note.empty(), "unknown label");
    }
    // ---- the user picked another mode by hand: the session ends without an action; a late scan is ignored while decoding
    {
        GeneralAuto g; double t = 0;
        scan(g, t, 0, 1e9, {det("FM broadcast", "fm", 98 * MHz, 180e3, 0.95)});
        g.decode(t, g.visible()[0]->id);
        g.scanStarted(t + 1); g.scanDone(t + 2, 0, 1e9, {});   // a late result
        CHECK(g.state() == State::Decoding && g.labels().size() == 1, "late scan disturbed the decode");
        g.abandon();
        CHECK(g.state() == State::General && g.tick(t + 10, 300 * MHz).act == Action::None, "abandon");
        g.reset();
        CHECK(g.labels().empty(), "reset");
    }
    // ---- an explicit keep-decoding distance
    {
        GeneralAuto g; g.cfg.releaseHz = 100e3; double t = 0;
        scan(g, t, 0, 1e9, {det("NFM voice", "general", 145.5 * MHz, 12e3, 0.95)});
        g.decode(t, g.visible()[0]->id);
        CHECK(g.tick(t + 3, 145.5 * MHz + 90e3).act == Action::None, "released inside the set distance");
        CHECK(g.tick(t + 3, 145.5 * MHz + 110e3).act == Action::ReturnToGeneral, "kept outside the set distance");
        CHECK(releaseFor(Config(), 8e6) == 2e6 && releaseFor(Config(), 12e3) == 25e3, "automatic release distance");
    }
}

// ---- the whole way: IQ of several modes in one wide capture, each off the centre, through the identifier thread and the state machine
static void shiftAdd(std::vector<cf32>& sum, const std::vector<cf32>& x, double fs, double hz, float gain = 1.f) {
    double ph = 0; const double d = 2 * M_PI * hz / fs;
    const size_t n = std::min(sum.size(), x.size());
    for (size_t i = 0; i < n; i++) { sum[i] += x[i] * gain * cf32((float)std::cos(ph), (float)std::sin(ph)); ph = std::fmod(ph + d, 2 * M_PI); }
}

static void endToEnd() {
    const double fs = 2e6, rf0 = 446e6;
    const size_t n = (size_t)(fs * 1.3);
    std::vector<cf32> sum(n, cf32(0, 0));
    { DmrGenConfig c; c.rate = fs; c.cfoHz = 900; c.snrDb = 40; DmrSignal g(c); std::vector<cf32> x(n); g.generate(x.data(), n); shiftAdd(sum, x, fs, 300e3); }
    { P25GenConfig c; c.rate = fs; c.snrDb = 40; P25GenTx t; t.durSec = 4; c.txs.push_back(t); P25Generator g(c); std::vector<cf32> x(n); g.generate(x.data(), n); shiftAdd(sum, x, fs, -450e3); }
    { FmGenConfig c; c.rate = fs; c.cnrDb = 45; FmGenerator g(c); std::vector<cf32> x; g.generate(n, x); shiftAdd(sum, x, fs, -800e3, 0.5f); }
    sigid::Capture cap;
    cap.startAll(fs, rf0, 1.2, 12);
    for (size_t p = 0; p < n && cap.armed(); p += 65536) cap.push(sum.data() + p, std::min<size_t>(65536, n - p));
    std::vector<sigid::Capture::Found> found; uint64_t seq = 0;
    for (int i = 0; i < 1200 && !cap.resultAll(found, seq); i++) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK(seq == 1, "no result from the capture thread");
    GeneralAuto g; double t = 0;
    std::vector<Detection> v;
    for (const auto& f : found) {
        Detection d;
        printf("  found %.4f MHz %s\n", (rf0 + f.region.centreHz) / 1e6, f.res.cands.empty() ? "-" : f.res.cands[0].name.c_str());
        if (fromFound(f, rf0, d)) v.push_back(d);
    }
    scan(g, t, rf0 - fs / 2, rf0 + fs / 2, v);
    t += 0; scan(g, t, rf0 - fs / 2, rf0 + fs / 2, v);
    auto has = [&](const char* name, double rf, double tol) {
        for (const Label* l : g.visible()) if (l->d.name == name && std::fabs(l->d.rfHz - rf) < tol) return l;
        return (const Label*)nullptr;
    };
    const Label* dmr = has("DMR", rf0 + 300e3 + 900, 6e3);
    CHECK(dmr, "DMR label missing");
    CHECK(has("P25 Phase 1", rf0 - 450e3, 6e3), "P25 label missing");
    CHECK(has("FM broadcast", rf0 - 800e3, 15e3), "FM broadcast label missing");
    if (dmr) {   // Decode, then tune away: back to General
        const Step s = g.decode(t, dmr->id);
        CHECK(s.act == Action::Decode && s.mem.mode == "dmr" && std::fabs((double)s.mem.freqHz - (rf0 + 300e3)) < 6e3, "DMR decode step (%s, %llu)", s.mem.mode.c_str(), (unsigned long long)s.mem.freqHz);
        CHECK(g.tick(t + 5, rf0 + 300e3).act == Action::None, "released on the signal");
        CHECK(g.tick(t + 6, rf0 + 400e3).act == Action::ReturnToGeneral, "not released 100 kHz away");
    }
}

int main() {
    stateMachine();
    endToEnd();
    return testSummary("genauto");
}
