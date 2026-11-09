// Wideband LoRa survey (loraraw_survey.h): several LoRa signals of different spreading factor, bandwidth and sync word in one 2 MHz capture,
// found without being told any parameter and classified (Meshtastic preset, LoRaWAN plan channel, LoRa APRS, private); sensitivity of the
// spectral gate against the Signal Lab search that is told the channel; false alarms on noise and interferers; carrier offsets; time.
#include "dect2/loraraw/loraraw_gen.h"
#include "dect2/loraraw/loraraw_lab.h"
#include "dect2/loraraw/loraraw_survey.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::lora;
#include "../common/test_util.h"

static const double kFs = 2e6;

struct Sig { int sf; double bw; int cr; int sync; double offsetHz; double snrDb; double cfoHz; int nFrames; double startSec; };

// noiseless samples of one signal, scaled so that its SNR in its own bandwidth is snrDb against complex white noise of power sigma2
static void addSignal(std::vector<cf32>& x, const Sig& s, double sigma2, unsigned seed) {
    LoraRawGenConfig g;
    g.rate = kFs; g.dialOffsetHz = s.offsetHz; g.snrDb = 99; g.loop = false; g.leadSec = s.startSec; g.seed = seed; g.cfoHz = s.cfoHz;
    g.amp = std::sqrt(std::pow(10.0, s.snrDb / 10.0) * sigma2 * s.bw / kFs);
    double dur = g.leadSec;
    for (int i = 0; i < s.nFrames; i++) {
        LoraRawGenFrame f;
        f.p.sf = s.sf; f.p.bwHz = s.bw; f.p.cr = s.cr; f.p.syncWord = (uint8_t)s.sync; f.p.crc = true; f.p.preamble = s.sync == 0x2B ? 16 : 8;
        f.p.ldro = autoLdro(s.sf, s.bw);
        const std::string t = "survey " + std::to_string(s.sf) + "/" + std::to_string(i) + " abcdefghij";
        f.payload.assign(t.begin(), t.end());
        f.gapSec = 0.35;
        g.frames.push_back(f);
        dur += airSeconds(f.p, f.payload.size()) + f.gapSec;
    }
    LoraRawGenerator gen(g);
    const size_t n = std::min(x.size(), (size_t)(dur * kFs));
    std::vector<cf32> tmp(n);
    gen.generate(tmp.data(), n);
    for (size_t i = 0; i < n; i++) x[i] += tmp[i];
}

static std::vector<cf32> capture(const std::vector<Sig>& sigs, double seconds, double sigma2, unsigned seed, bool tones = false) {
    std::vector<cf32> x((size_t)(seconds * kFs));
    std::mt19937 g(seed);
    std::normal_distribution<float> nd(0.f, (float)std::sqrt(sigma2 / 2));
    for (auto& v : x) v = cf32(nd(g), nd(g));
    unsigned sd = seed * 7 + 1;
    for (const Sig& s : sigs) addSignal(x, s, sigma2, sd++);
    if (tones) {   // a few strong carriers: a spur, a CW interferer and a drifting one
        for (size_t i = 0; i < x.size(); i++) {
            const double t = (double)i / kFs;
            x[i] += cf32((float)(4 * std::sqrt(sigma2) * std::cos(2 * 3.14159265 * 310e3 * t)), (float)(4 * std::sqrt(sigma2) * std::sin(2 * 3.14159265 * 310e3 * t)));
            x[i] += cf32((float)(3 * std::sqrt(sigma2) * std::cos(2 * 3.14159265 * (-540e3 + 400 * std::sin(t)) * t)), 0.f);
        }
    }
    return x;
}

static const SurveyEntry* findEntry(const SurveyResult& r, int sf, double bw, int sync, double offHz, double tol = 20e3) {
    for (const SurveyEntry& e : r.entries) if (e.decoded && e.sf == sf && std::fabs(e.bwHz - bw) < 1 && e.syncWord == sync && std::fabs(e.offsetHz - offHz) < tol) return &e;
    return nullptr;
}

int main() {
    const double sigma2 = 0.004;   // noise power: amplitude 0.063 rms
    SurveyOptions o; o.centerHz = 868.0e6; o.maxSeconds = 12;

    o.centerHz = 869.0e6;
    const std::vector<Sig> mixed = {
        {11, 250e3, 5, 0x2B, +0.525e6, 8, 2500, 3, 0.10},    // 869.525 MHz: Meshtastic LongFast EU
        {7, 125e3, 5, 0x34, -0.900e6, 8, -3000, 4, 0.30},    // 868.100 MHz: LoRaWAN EU868 default channel
        {9, 125e3, 6, 0x12, -0.300e6, 8, 1200, 3, 0.20},     // 868.700 MHz: private network, sync 0x12
        {8, 62.5e3, 8, 0x12, +0.150e6, 8, 800, 3, 0.40},     // 869.150 MHz: MeshCore-like narrow
    };
    {
        const std::vector<cf32> x = capture(mixed, 5.0, sigma2, 11);
        const auto t0 = std::chrono::steady_clock::now();
        const SurveyResult r = loraSurvey(x.data(), x.size(), kFs, o);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("mixed band (%.1f s of capture, %zu candidate scales, %zu banks): %s\n  took %.2f s (%.2fx real time)\n", r.seconds, r.channels.size(), r.banks, r.note.c_str(), sec, sec / r.seconds);
        for (const SurveyEntry& e : r.entries) printf("  %s%.3f MHz  SF%d  %.1f kHz  4/%d  sync %02X  frames %d (crc ok %d)  snr %.1f  duty %.2f%%  -> %s\n", e.invertIq ? "inv " : "", e.freqHz / 1e6, e.sf, e.bwHz / 1e3, e.cr, e.syncWord, e.frames, e.crcOk, e.snrDb, e.dutyPct, e.network.c_str());
        const SurveyEntry* a = findEntry(r, 11, 250e3, 0x2B, 0.525e6);
        CHECK(a && a->crcOk == 3 && a->network.find("Meshtastic LongFast") == 0, "Meshtastic LongFast found (%s)", a ? a->network.c_str() : "missing");
        if (a) CHECK(std::fabs(a->freqHz - 869.525e6) < 6e3, "its frequency %.4f MHz", a->freqHz / 1e6);
        const SurveyEntry* b = findEntry(r, 7, 125e3, 0x34, -0.90e6);
        CHECK(b && b->crcOk == 4 && b->network == "LoRaWAN EU868", "LoRaWAN EU868 found (%s)", b ? b->network.c_str() : "missing");
        const SurveyEntry* c = findEntry(r, 9, 125e3, 0x12, -0.3e6);
        CHECK(c && c->crcOk == 3 && c->network.find("private LoRa") == 0 && c->cr == 6, "private SF9 found (%s)", c ? c->network.c_str() : "missing");
        const SurveyEntry* d = findEntry(r, 8, 62.5e3, 0x12, 0.15e6);
        CHECK(d && d->crcOk == 3 && d->cr == 8, "SF8 62.5 kHz found (%s)", d ? d->network.c_str() : "missing");
        CHECK(r.entries.size() == 4, "no extra entries: %zu", r.entries.size());
        CHECK(sec / r.seconds < 8, "survey runs within 8x real time on this machine (%.1fx)", sec / r.seconds);
    }

    // ---- LoRa APRS next to a second signal: centre 433.775 MHz, SF12 125k sync 0x12 on the dial, and a LoRaWAN-less private SF10
    {
        SurveyOptions o2; o2.centerHz = 433.9e6; o2.maxSeconds = 12;
        const std::vector<Sig> sigs = {{12, 125e3, 5, 0x12, -0.125e6, 8, 1500, 2, 0.1}, {10, 125e3, 5, 0x2B, +0.3e6, 8, -900, 2, 0.2}};
        const std::vector<cf32> x = capture(sigs, 6.0, sigma2, 21);
        const SurveyResult r = loraSurvey(x.data(), x.size(), kFs, o2);
        const SurveyEntry* a = findEntry(r, 12, 125e3, 0x12, -0.125e6);
        CHECK(a && a->crcOk == 2 && a->network == "LoRa APRS", "LoRa APRS found (%s)", a ? a->network.c_str() : "missing");
        const SurveyEntry* m = findEntry(r, 10, 125e3, 0x2B, 0.3e6);
        CHECK(m && m->crcOk == 2, "Meshtastic-sync signal found");
    }

    // ---- two spreading factors on one channel at the same time, and two adjacent 250 kHz channels (no gap between them)
    {
        const std::vector<Sig> sigs = {{7, 125e3, 5, 0x12, 0.0, 10, 0, 3, 0.1}, {9, 125e3, 5, 0x12, 0.0, 10, 0, 3, 0.15}, {8, 250e3, 5, 0x2B, +0.5e6, 8, 0, 3, 0.1}, {8, 250e3, 5, 0x2B, +0.75e6, 8, 0, 3, 0.2}};
        const std::vector<cf32> x = capture(sigs, 4.0, sigma2, 31);
        SurveyOptions o3; o3.centerHz = 868.5e6;
        const SurveyResult r = loraSurvey(x.data(), x.size(), kFs, o3);
        printf("co-channel and adjacent channels: %s\n", r.note.c_str());
        CHECK(findEntry(r, 7, 125e3, 0x12, 0) && findEntry(r, 9, 125e3, 0x12, 0), "both spreading factors of one channel found");
        const SurveyEntry* a = findEntry(r, 8, 250e3, 0x2B, 0.5e6, 30e3);
        const SurveyEntry* b = findEntry(r, 8, 250e3, 0x2B, 0.75e6, 30e3);
        CHECK(a && b, "two touching 250 kHz channels both found (%d %d)", a != nullptr, b != nullptr);
    }

    // ---- sensitivity: the survey (spectral gate + dechirp banks, told nothing) against the Signal Lab search told the channel
    printf("sensitivity: SF7 125 kHz in 2 MHz, frames decoded of 4 (survey / lab told the offset)\n  snr_dB  survey  lab\n");
    double lastSurveyOk = 99, lastLabOk = 99;
    for (double snr : {10.0, 4.0, 0.0, -3.0, -6.0}) {
        int sOk = 0, lOk = 0; const int trials = 3;
        for (int t = 0; t < trials; t++) {
            const std::vector<Sig> sigs = {{7, 125e3, 5, 0x12, 0.3e6, snr, 0, 4, 0.1}};
            const std::vector<cf32> x = capture(sigs, 3.0, sigma2, 41 + (unsigned)t);
            SurveyOptions o4; o4.centerHz = 868.5e6; o4.maxSeconds = 12;
            const SurveyResult r = loraSurvey(x.data(), x.size(), kFs, o4);
            if (const SurveyEntry* e = findEntry(r, 7, 125e3, 0x12, 0.3e6)) sOk += e->crcOk;
            LabDetectOptions lo; lo.autoOffset = false; lo.offsetHz = 0.3e6;
            const LabDetect d = labAutoDetect(x.data(), x.size(), kFs, lo);
            if (d.found && d.settings.sf == 7) lOk += d.crcOk;
        }
        printf("  %5.1f   %2d/12   %2d/12\n", snr, sOk, lOk);
        if (sOk >= 9) lastSurveyOk = snr;
        if (lOk >= 9) lastLabOk = snr;
    }
    printf("  lowest SNR with at least 9 of 12: survey %.0f dB, lab %.0f dB\n", lastSurveyOk, lastLabOk);
    CHECK(lastSurveyOk <= 4.0, "the survey must work at 4 dB SNR");

    // ---- long frames at low SNR: SF12 125 kHz (frames of about 1 s) through the 100 ms detection scale
    printf("sensitivity: SF12 125 kHz, frames decoded of 4 (survey / lab told the offset)\n  snr_dB  survey  lab\n");
    double lowSurvey12 = 99, lowLab12 = 99;
    for (double snr : {-8.0, -12.0, -15.0, -18.0}) {
        int sOk = 0, lOk = 0; const int trials = 2;
        for (int tr = 0; tr < trials; tr++) {
            const std::vector<Sig> sigs = {{12, 125e3, 5, 0x12, -0.4e6, snr, 0, 2, 0.1}};
            const std::vector<cf32> x = capture(sigs, 4.0, sigma2, 61 + (unsigned)tr);
            SurveyOptions o6; o6.centerHz = 868.5e6; o6.maxSeconds = 12;
            const SurveyResult r = loraSurvey(x.data(), x.size(), kFs, o6);
            if (const SurveyEntry* e = findEntry(r, 12, 125e3, 0x12, -0.4e6)) sOk += e->crcOk;
            LabDetectOptions lo; lo.autoOffset = false; lo.offsetHz = -0.4e6;
            const LabDetect d = labAutoDetect(x.data(), x.size(), kFs, lo);
            if (d.found && d.settings.sf == 12) lOk += d.crcOk;
        }
        printf("  %5.1f   %2d/4    %2d/4\n", snr, sOk, lOk);
        if (sOk >= 3) lowSurvey12 = snr;
        if (lOk >= 3) lowLab12 = snr;
    }
    printf("  lowest SNR with at least 3 of 4: survey %.0f dB, lab %.0f dB\n", lowSurvey12, lowLab12);
    CHECK(lowSurvey12 <= -12.0, "the survey must find SF12 at -12 dB (%.0f)", lowSurvey12);

    // ---- false alarms: noise alone, and noise with strong carriers, over several seeds
    {
        int falseEntries = 0, falseChannels = 0;
        for (unsigned s = 0; s < 8; s++) {
            const std::vector<cf32> x = capture({}, 3.0, sigma2, 100 + s, s % 2 == 1);
            SurveyOptions o5; o5.centerHz = 868.5e6;
            const SurveyResult r = loraSurvey(x.data(), x.size(), kFs, o5);
            falseEntries += (int)r.entries.size(); falseChannels += (int)r.channels.size();
            for (const SurveyEntry& e : r.entries) printf("  false entry seed %u: offset %.0f SF%d %.0f sync %02X frames %d crc %d pre %d decoded %d\n", s, e.offsetHz, e.sf, e.bwHz, e.syncWord, e.frames, e.crcOk, e.preambles, e.decoded);
        }
        printf("false alarms over 8 noise captures (4 with carriers): %d channels, %d entries\n", falseChannels, falseEntries);
        CHECK(falseEntries == 0, "no LoRa entries on noise (%d)", falseEntries);
    }
    return testSummary("loraraw_survey");
}
