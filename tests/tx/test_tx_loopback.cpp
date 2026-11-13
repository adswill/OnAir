// Transmit -> receive loop-back for the transmit modes, from the parameter table's defaults (tx_params.h) and a few parameter combinations each:
// the TX generator's IQ, with white noise at a set SNR in the occupied bandwidth and a carrier offset, is written to a file and played through
// OnAir's own receiver (the Engine), which must lock and decode (for the text modes: give the sent text back).
//   --group tv | rest | all  (default all). Cases marked "must" fail the test when they do not decode; the others are reported (info) so that a
//   receiver limit is listed without hiding the rest.
// Simulated signals only: nothing is transmitted.
#include "dect2/dsp/gen_util.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/tx/tx_params.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif
using namespace dect2;

namespace {

int fails = 0, passed = 0, infoFailed = 0;
std::vector<std::string> summary;

struct Case {
    const char* id;
    std::map<std::string, double> set;   // table rows changed from their defaults
    double secs, snrDb, cfoHz, waitSec;
    bool must;
    const char* note;
};

bool isTv(int std) { return std == 0 || std == 3 || std == 6 || std == 8 || std == 9 || std == 4; }

TxRequest requestFor(const TxModeSpec& m, const std::map<std::string, double>& set) {
    TxParamCtx c;
    const auto& rows = txParams(m, c);
    std::map<std::string, double> num;
    for (const auto& d : rows) num[d.key] = d.def;
    for (const auto& kv : set) num[kv.first] = kv.second;
    TxRequest r;
    r.modeId = m.id;
    r.audioSrc = 2;
    r.loop = true;
    txParamsApplyNumbers(rows, num, r);
    // the payload a user would type (fields the mode has no row for are cleared by the sanitizer)
    r.callsign = "N0ABC"; r.grid = "KO24"; r.text = "HELLO 123"; r.name = "ONAIR"; r.dest = ""; r.path = "";
    if (std::string(m.id) == "dmr") { r.text.clear(); }
    txParamsSanitize(m, c, r);
    return r;
}

// generated, shifted to the receiver's centre (the generator puts a mode with offset tuning that far below), noise added, written in pieces
bool writeRecording(const TxModeSpec& m, const TxRequest& r, double rate, double secs, double snrDb, double cfoHz, const std::string& path, double& rateOut, double& obwHz) {
    std::string err;
    auto g = makeTxGenerator(m, r, rate, err);
    if (!g) { printf("  %s: no generator: %s\n", m.id, err.c_str()); return false; }
    rateOut = g->sampleRate();
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t total = (size_t)(secs * rateOut), block = 1 << 16;
    std::vector<cf32> x(block);
    const ModeTuning* mt = modeTuning(m.stdMode);
    const double shiftHz = cfoHz + txModeTuneOffsetHz(m) * (mt ? 1 : 0);
    double ph = 0, sigma = -1;
    genutil::NoiseSource ns(11);
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(block, total - done);
        g->generate(x.data(), n);
        txOutputStage(r, x.data(), n, done, rateOut);
        txSoftLimit(x.data(), n);
        if (sigma < 0) {   // the SNR in the occupied bandwidth, from the first block that has signal
            double p = 0; size_t on = 0;
            for (size_t i = 0; i < n; i++) { const double q = std::norm(x[i]); if (q > 1e-9) { p += q; on++; } }
            if (on > n / 8) {
                p /= (double)on;
                std::vector<float> db;
                const int nfft = 4096;
                if (n >= (size_t)nfft) { txSpectrumDb(x.data(), (size_t)nfft, nfft, db); obwHz = std::max(100.0, txOccupiedBandwidthHz(db, rateOut)); }
                else obwHz = rateOut / 4;
                sigma = std::sqrt(p * rateOut / obwHz / std::pow(10.0, snrDb / 10) / 2);
            }
        }
        const double w = 2 * M_PI * shiftHz / rateOut;
        for (size_t i = 0; i < n; i++) { x[i] *= std::polar(1.f, (float)ph); ph += w; if (ph > M_PI) ph -= 2 * M_PI; if (ph < -M_PI) ph += 2 * M_PI; }
        if (sigma > 0) ns.add(x.data(), n, (float)sigma);
        fwrite(x.data(), sizeof(cf32), n, f);
        done += n;
        if (g->finished()) break;
    }
    fclose(f);
    return true;
}

bool decoded(const TxModeSpec& m, const RxTelemetry& rx) {
    if (m.stdMode == 27) {
        if (m.variant >= 26 && m.variant <= 31) return rx.hfdig.kbd.text.find("HELLO") != std::string::npos;
        if (m.variant == 0) return rx.hfdig.rtty.text.find("HELLO") != std::string::npos;
        for (const auto& d : rx.hfdig.ftx.decodes) if (d.msg.find("N0ABC") != std::string::npos) return true;
        return false;
    }
    if (m.stdMode == 25) {
        if (!rx.pager.messages) return false;
        for (const auto& p : *rx.pager.messages) if (p.text.find("HELLO") != std::string::npos) return true;
        return false;
    }
    if (isTv(m.stdMode)) return rx.state == 2 && rx.dataValid;
    return rx.state >= 1 && rx.dataValid;
}

void run(const Case& c) {
    const TxModeSpec* m = txModeById(c.id);
    if (!m) { printf("FAIL: no mode %s\n", c.id); fails++; return; }
    const TxRequest r = requestFor(*m, c.set);
    const double rate = txRequestRate(*m, r, {});
    char path[256];
#ifdef _WIN32
    snprintf(path, sizeof path, "onair_txlb_%s.cf32", c.id);
#else
    snprintf(path, sizeof path, "/tmp/onair_txlb_%s_%d.cf32", c.id, (int)getpid());
#endif
    double fs = rate, obw = 0;
    bool ok = false;
    std::string why = "no recording";
    if (writeRecording(*m, r, rate, c.secs, c.snrDb, c.cfoHz, path, fs, obw)) {
        Engine e;
        DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "tx loop-back";
        const ModeTuning* mt = modeTuning(m->stdMode);
        TuneSettings t;
        t.sampleRate = fs;
        t.centerHz = 100e6;
        t.bandwidthMhz = m->stdMode == 0 ? (r.bandwidthMhz > 0 ? r.bandwidthMhz : 8) : m->stdMode == 3 ? 6 : m->stdMode == 6 ? r.synth.isdbtBwMhz : m->stdMode == 4 ? 1.536 : mt ? mt->bandwidthMhz : 8;
        FileOptions fo; fo.path = path; fo.format = FileFormat::CF32; fo.sampleRate = fs; fo.loop = true;
        e.setStandard(m->stdMode == 0 ? m->variant : m->stdMode);
        if (const SubMode* sm = subModeById(c.id)) applySubMode(e, sm);
        if (e.start(dev, t, fo)) {
            RxTelemetry rx;
            uint64_t seq = 0;
            const auto t0 = std::chrono::steady_clock::now();
            while (!ok && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < c.waitSec) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (e.latestRx(rx, seq)) { seq = rx.seq; ok = decoded(*m, rx); }
            }
            e.stop();
            why = ok ? "decoded" : "not decoded";
        } else why = "the receiver did not start";
    }
    remove(path);
    std::string set;
    for (const auto& kv : c.set) { char b[64]; snprintf(b, sizeof b, " %s=%g", kv.first.c_str(), kv.second); set += b; }
    char line[400];
    snprintf(line, sizeof line, "%-9s%-34s SNR %4.0f dB in %7.1f kHz, %+5.0f Hz: %s%s", c.id, set.empty() ? " (defaults)" : set.c_str(), c.snrDb, obw / 1e3, c.cfoHz,
             why.c_str(), ok ? "" : c.must ? "  [must]" : "  [info]");
    printf("  %s\n", line);
    fflush(stdout);
    summary.push_back(line);
    if (ok) passed++;
    else if (c.must) { printf("FAIL: %s %s\n", c.id, set.c_str()); fails++; }
    else infoFailed++;
}

} // namespace

int main(int argc, char** argv) {
    std::string group = "all";
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--group")) group = argv[i + 1];
    const std::vector<Case> tv = {
        {"dvbt2", {}, 3, 25, 0, 120, true, ""},
        {"dvbt2", {{"t2.fft", 1}, {"t2.gi", 2}, {"t2.pp", 1}, {"t2.plpMod", 1}}, 3, 22, 0, 120, false, "2K, 1/8, PP2, 16-QAM"},
        {"dvbt", {}, 3, 25, 0, 120, true, ""},
        {"dvbt", {{"dvbt.mode", 0}, {"dvbt.mod", 1}, {"dvbt.rate", 0}}, 3, 20, 0, 120, false, "2K 16-QAM 1/2"},
        {"isdbt", {}, 3, 25, 0, 120, true, ""},
        {"dvbs", {}, 3, 15, 0, 120, true, ""},
        {"dvbs", {{"dvbs.std", 1}, {"dvbs.crS", 3}, {"dvbs.symrate", 2e6}}, 3, 12, 0, 120, false, "DVB-S 3/4 2 Msym/s"},
        {"dtmb", {}, 3, 25, 0, 120, true, ""},
        {"atsc", {}, 3, 25, 0, 120, true, ""},
        {"dab", {}, 4, 20, 0, 90, true, ""},
    };
    const std::vector<Case> rest = {
        {"fm", {}, 2, 30, 200, 30, true, ""},
        {"dmr", {}, 3, 25, 50, 30, true, ""},
        {"ais", {}, 7, 20, 100, 40, true, ""},
        {"packet", {}, 4, 20, 50, 30, true, ""},
        {"packet", {{"aprs.baud", 1}}, 4, 25, 50, 30, false, "9600 Bd"},
        {"pager", {}, 6, 20, 50, 40, true, ""},
        {"pager", {{"pager.speed", 2}}, 6, 20, 50, 40, false, "POCSAG 2400"},
        {"pager", {{"pager.speed", 3}}, 6, 20, 50, 40, false, "FLEX 1600"},
        {"cw", {}, 8, 20, 20, 30, true, ""},
        {"cw", {{"cw.wpm", 30}, {"audio.tone", 900}}, 8, 20, 20, 30, false, "30 wpm, 900 Hz"},
        {"rtty", {}, 10, 20, 20, 40, true, ""},
        {"rtty", {{"rtty.baud", 1}, {"rtty.shift", 2}}, 10, 20, 20, 40, false, "50 Bd, 425 Hz"},
        {"psk", {}, 8, 20, 10, 30, false, ""},
        {"psk", {{"psk.mode", 1}}, 8, 20, 10, 30, false, "BPSK63"},
        {"olivia", {}, 20, 15, 10, 60, false, ""},
        {"olivia", {{"kbd.tones", 2}, {"kbd.bw", 2}}, 20, 15, 10, 60, false, "8 tones, 500 Hz"},
        {"contestia", {}, 12, 20, 0, 40, true, ""},
        {"dominoex", {}, 8, 20, 0, 40, true, ""},
        {"ft8", {}, 32, 15, 20, 60, true, ""},
        {"ale", {}, 6, 20, 0, 30, true, ""},
        {"cubesat", {}, 4, 25, 0, 30, true, ""},
        {"adsb", {}, 2, 25, 0, 20, true, ""},
        {"drm", {}, 6, 25, 0, 40, true, ""},
        {"loraraw", {}, 3, 15, 0, 30, false, ""},
        {"loraraw", {{"lora.sf", 9}, {"lora.cr", 2}}, 4, 15, 0, 30, false, "SF9 4/7"},
        {"vorils", {}, 3, 25, 0, 30, false, ""},
        {"same", {}, 8, 20, 0, 30, false, ""},
        {"acars", {}, 4, 20, 0, 30, false, ""},
    };
    printf("transmit -> receive loop-back from the parameter table (white noise in the occupied bandwidth, carrier offset):\n");
    if (group == "tv" || group == "all") for (const auto& c : tv) run(c);
    if (group == "rest" || group == "all") for (const auto& c : rest) run(c);
    printf("decoded %d, must-cases not decoded %d, info cases not decoded %d\n", passed, fails, infoFailed);
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL PASSED\n");
    return 0;
}
