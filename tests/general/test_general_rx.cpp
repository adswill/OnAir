// General receiver: a tone through each demodulator comes out at the right audio frequency and level; the squelch closes on noise
// (auto and level), opens on a station; a channel off the dial is found by its offset; de-emphasis; WAV recording. No sound.
#include "dect2/general/general_gen.h"
#include "dect2/general/general_rx.h"
#include "dect2/general/general_tel.h"
#include "dect2/source/source.h"
#include <cmath>
#include <complex>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "dect2/common/mathconst.h"
using namespace dect2;
#include "../common/test_util.h"

using dect2::math::kPi;
constexpr double kRate = 2000000;

struct Run {
    std::vector<float> aud;
    GeneralTelemetry tel;
};

// demodulator d on a synthetic station modulated with modulation m (the receiver's channel at chanOff from the dial)
static Run run(int d, int m, double secs, double snr, bool quiet, double sigFromDial = 0, double chanOff = 0, double deemph = 0,
               bool sqAuto = true, bool sqOn = true, float sqDb = -60, double tone = 1000) {
    SynthConfig sc;
    sc.mode = 57; sc.snrDb = snr; sc.modeOpt[0] = m; sc.modeOpt[1] = quiet ? 1 : 0; sc.modeVal[0] = sigFromDial; sc.modeVal[1] = tone;
    auto syn = makeGeneralSynth(sc, kRate);
    GeneralReceiver r;
    r.configure(kRate);
    r.setSignalOffset(-generalTuning().tuneOffsetHz);
    r.setSilent(true);
    r.setDemod(d);
    r.setWidthHz(generalDefaultWidth(d));
    r.setChannelOffsetHz(chanOff);
    r.setDeemphUs(deemph);
    r.setSquelch(sqOn, sqAuto, sqDb);
    Run out;
    r.setAudioTap([&](const float* x, size_t n) { out.aud.insert(out.aud.end(), x, x + n); });
    std::vector<cf32> buf(20000);
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += buf.size() / kRate) {
        syn->generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
        GeneralTelemetry g;
        if (r.telemetry(g, seq)) { seq = g.seq; out.tel = g; }
    }
    return out;
}

// amplitude of the tone at f in the last half of the audio (correlation), and the strongest of a few candidates
static double toneAmp(const std::vector<float>& a, double f) {
    const size_t s = a.size() / 2;
    double re = 0, im = 0;
    for (size_t i = s; i < a.size(); i++) { const double w = 2 * kPi * f * (double)i / kGeneralAudioRate; re += a[i] * std::cos(w); im += a[i] * std::sin(w); }
    return 2 * std::sqrt(re * re + im * im) / (double)(a.size() - s);
}
static double rms(const std::vector<float>& a) {
    const size_t s = a.size() / 2;
    double p = 0;
    for (size_t i = s; i < a.size(); i++) p += (double)a[i] * a[i];
    return a.size() > s ? std::sqrt(p / (double)(a.size() - s)) : 0;
}

int main() {
    // edges and presets
    double lo, hi;
    generalFilterEdges(GenUsb, 2700, lo, hi); CHECK(lo == 300 && hi == 3000, "usb edges %g %g", lo, hi);
    generalFilterEdges(GenLsb, 2700, lo, hi); CHECK(lo == -3000 && hi == -300, "lsb edges %g %g", lo, hi);
    generalFilterEdges(GenNfm, 12500, lo, hi); CHECK(lo == -6250 && hi == 6250, "nfm edges");
    CHECK(generalClampWidth(GenCw, 1e6) == 3000, "clamp");
    CHECK(generalTuning().stdMode == 57 && std::fabs(generalTuning().defMhz - 145.5) < 1e-9, "tuning");

    struct Case { int d; double tone, amp; const char* name; };
    // the expected peaks: half scale at full deviation / depth, and the synth uses half of it (0.25); SSB and CW sit at 0.5 under the AGC
    const Case cases[] = {{GenNfm, 1000, 0.25, "NFM"}, {GenWfm, 1000, 0.25, "WFM"}, {GenAm, 1000, 0.25, "AM"},
                          {GenUsb, 1000, 0.5, "USB"}, {GenLsb, 1000, 0.5, "LSB"}, {GenCw, 700, 0.5, "CW"}};
    for (const Case& c : cases) {
        const Run r = run(c.d, c.d, 1.5, 30, false);
        const double a = toneAmp(r.aud, c.tone), aOff = toneAmp(r.aud, c.tone * 1.37), tot = rms(r.aud) * std::sqrt(2.0);
        printf("  %s: %zu samples, tone %.3f (want %.3f), off-tone %.4f, peak from rms %.3f, %s\n", c.name, r.aud.size(), a, c.amp, aOff, tot, generalSummary(r.tel).c_str());
        CHECK(r.aud.size() > 60000, "%s audio %zu", c.name, r.aud.size());
        CHECK(std::fabs(20 * std::log10(a / c.amp)) < 1.0, "%s level %.3f want %.3f", c.name, a, c.amp);
        CHECK(aOff < 0.1 * a, "%s off-tone %.4f", c.name, aOff);
        CHECK(tot < 1.3 * a, "%s distortion: rms peak %.3f vs tone %.3f", c.name, tot, a);
        CHECK(r.tel.squelchOpen && r.tel.dataValid && r.tel.snrDb > 15, "%s squelch %d snr %.1f", c.name, r.tel.squelchOpen, r.tel.snrDb);
    }

    // a station 25 kHz above the dial, the channel put on it
    {
        const Run r = run(GenNfm, GenNfm, 1.0, 30, false, 25000, 25000);
        CHECK(std::fabs(20 * std::log10(toneAmp(r.aud, 1000) / 0.25)) < 1.0 && r.tel.squelchOpen, "offset channel %.3f", toneAmp(r.aud, 1000));
        const Run w = run(GenNfm, GenNfm, 1.0, 30, false, 25000, 0);   // the channel left on the dial: nothing there
        CHECK(!w.tel.squelchOpen && rms(w.aud) < 1e-4, "dial channel open %d rms %.5f", w.tel.squelchOpen, rms(w.aud));
    }

    // squelch on noise: auto closes in every demodulator; level closes under the noise threshold
    for (int d = 0; d < GenDemodCount; d++) {
        const Run r = run(d, d, 1.5, 30, true);
        printf("  noise %s: level %.1f noise %.1f open %d rms %.5f\n", generalDemodName(d), r.tel.levelDb, r.tel.noiseDb, r.tel.squelchOpen, rms(r.aud));
        CHECK(!r.tel.squelchOpen && r.tel.blocksOk < 5 && rms(r.aud) < 1e-4, "%s auto squelch on noise: open %d, %llu open blocks, rms %.5f",
              generalDemodName(d), r.tel.squelchOpen, (unsigned long long)r.tel.blocksOk, rms(r.aud));
    }
    {
        const Run n = run(GenNfm, GenNfm, 1.0, 30, true, 0, 0, 0, false, true, -30);   // noise at about -40 dBFS, the station at -10
        const Run s = run(GenNfm, GenNfm, 1.0, 30, false, 0, 0, 0, false, true, -30);
        CHECK(!n.tel.squelchOpen && rms(n.aud) < 1e-4, "level squelch on noise (%.1f dBFS)", n.tel.levelDb);
        CHECK(s.tel.squelchOpen && toneAmp(s.aud, 1000) > 0.2, "level squelch on a station (%.1f dBFS)", s.tel.levelDb);
        const Run o = run(GenNfm, GenNfm, 1.0, 30, true, 0, 0, 0, true, false);
        CHECK(o.tel.squelchOpen && rms(o.aud) > 0.01, "squelch off: noise heard (rms %.4f)", rms(o.aud));
        const Run w = run(GenNfm, GenNfm, 1.5, 8, false);   // a weak station (8 dB in 12.5 kHz) still opens auto
        CHECK(w.tel.squelchOpen, "weak station: snr %.1f", w.tel.snrDb);
    }

    // de-emphasis 50 us on WFM: a 10 kHz tone comes out about 10 dB down against no de-emphasis, 300 Hz unchanged
    {
        const Run a = run(GenWfm, GenWfm, 1.0, 40, false, 0, 0, 0, true, true, -60, 10000), b = run(GenWfm, GenWfm, 1.0, 40, false, 0, 0, 50, true, true, -60, 10000);
        const double db = 20 * std::log10(toneAmp(b.aud, 10000) / toneAmp(a.aud, 10000));
        const Run c = run(GenWfm, GenWfm, 1.0, 40, false, 0, 0, 50, true, true, -60, 300);
        printf("  de-emphasis: 10 kHz %.1f dB, 300 Hz %.3f\n", db, toneAmp(c.aud, 300));
        CHECK(db < -8.5 && db > -11.5, "de-emphasis at 10 kHz %.1f dB", db);
        CHECK(std::fabs(20 * std::log10(toneAmp(c.aud, 300) / 0.25)) < 1.0, "de-emphasis at 300 Hz %.3f", toneAmp(c.aud, 300));
    }

    // WAV recording: the header gives the samples written
    {
        const std::string path = "general_rx_test.wav";
        SynthConfig sc; sc.mode = 57; sc.snrDb = 30;
        auto syn = makeGeneralSynth(sc, kRate);
        GeneralReceiver r;
        r.configure(kRate); r.setSignalOffset(-generalTuning().tuneOffsetHz); r.setSilent(true);
        CHECK(r.startRecording(path) && r.recording(), "record start");
        std::vector<cf32> buf(20000);
        for (int i = 0; i < 100; i++) { syn->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }   // 1 s
        r.stopRecording();
        FILE* f = fopen(path.c_str(), "rb");
        unsigned char h[44] = {};
        const size_t got = f ? fread(h, 1, 44, f) : 0;
        fseek(f, 0, SEEK_END);
        const long size = f ? ftell(f) : 0;
        if (f) fclose(f);
        remove(path.c_str());
        const uint32_t data = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24, sr = h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24;
        printf("  wav: %ld bytes, data %u, %u Hz\n", size, data, sr);
        CHECK(got == 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4) && sr == 48000 && data + 44 == (uint32_t)size && data > 90000, "wav header");
        CHECK(!r.startRecording("/nonexistent-dir/x.wav") && !r.recording(), "a bad path fails");
    }

    // keyed signal (a beacon or CW: 0.5 s on, 1.0 s off, five times): the CW squelch holds through the gaps (no chatter), the others follow them
    {
        SynthConfig on, off;
        on.mode = 57; on.snrDb = 30; off.mode = 57; off.snrDb = 30; off.modeOpt[1] = 1;
        for (int d : {(int)GenCw, (int)GenNfm}) {
            auto sOn = makeGeneralSynth(on, kRate), sOff = makeGeneralSynth(off, kRate);
            GeneralReceiver r;
            r.configure(kRate); r.setSignalOffset(-generalTuning().tuneOffsetHz); r.setSilent(true);
            r.setDemod(d); r.setWidthHz(generalDefaultWidth(d)); r.setSquelch(true, true, -60);
            std::vector<cf32> buf(20000);
            uint64_t seq = 0; bool was = false; int closes = 0;
            auto feedSecs = [&](std::unique_ptr<ModeSynth>& syn, double secs) {
                for (double t = 0; t < secs; t += buf.size() / kRate) {
                    syn->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size());
                    GeneralTelemetry g;
                    if (r.telemetry(g, seq)) { seq = g.seq; if (getenv("SQDBG")) printf("[%d %.0f %.0f]", (int)g.squelchOpen, g.levelDb, g.noiseDb); if (was && !g.squelchOpen) closes++; was = g.squelchOpen; }
                }
            };
            feedSecs(sOff, 1.0);
            for (int k = 0; k < 5; k++) { feedSecs(sOn, 0.5); feedSecs(sOff, 1.0); }
            printf("  keyed signal %s: squelch closed %d times\n", generalDemodName(d), closes);
            if (d == GenCw) CHECK(closes <= 1, "CW squelch chatters on a keyed signal: %d closes", closes);
            else CHECK(closes >= 3, "%s squelch should follow a keyed signal (%d closes)", generalDemodName(d), closes);
        }
    }

    // one WAV per transmission: station 2 s, nothing 2 s, station 2 s, nothing: two files (a 0.1 s blip would be dropped)
    {
        SynthConfig on, off;
        on.mode = 57; on.snrDb = 30; off.mode = 57; off.snrDb = 30; off.modeOpt[1] = 1;   // quiet: noise only
        auto sOn = makeGeneralSynth(on, kRate), sOff = makeGeneralSynth(off, kRate);
        GeneralReceiver r;
        r.configure(kRate); r.setSignalOffset(-generalTuning().tuneOffsetHz); r.setSilent(true);
        r.setSquelch(true, true, -60);
        r.setTransmissionRecording(true, ".", "general_rx_tx", 0.5);
        std::vector<cf32> buf(20000);
        auto feedSecs = [&](GeneralReceiver& rr, std::unique_ptr<ModeSynth>& syn, double secs) {
            for (double t = 0; t < secs; t += buf.size() / kRate) { syn->generate(buf.data(), buf.size()); rr.feed(buf.data(), buf.size()); }
        };
        feedSecs(r, sOff, 1.0); feedSecs(r, sOn, 2.0); feedSecs(r, sOff, 2.0); feedSecs(r, sOn, 2.0); feedSecs(r, sOff, 2.0);
        r.setTransmissionRecording(false, ".", "general_rx_tx");
        int files = 0;
        for (int i = 1; i <= 6; i++) { char n[64]; snprintf(n, sizeof n, "general_rx_tx_%d.wav", i); if (FILE* f = fopen(n, "rb")) { fclose(f); files++; remove(n); } }
        printf("  per-transmission files: %d (kept %d)\n", files, r.transmissionFiles());
        CHECK(files == 2 && r.transmissionFiles() == 2, "per-transmission files %d / %d", files, r.transmissionFiles());
    }

    // MP3 through LAME (loaded at run time): skipped when the library is not installed; the file must be a valid MP3 stream of about the right length
    if (!GeneralReceiver::mp3Available()) printf("  mp3: libmp3lame not installed, skipped\n");
    else {
        SynthConfig on, off;
        on.mode = 57; on.snrDb = 30; off.mode = 57; off.snrDb = 30; off.modeOpt[1] = 1;
        auto sOn = makeGeneralSynth(on, kRate), sOff = makeGeneralSynth(off, kRate);
        GeneralReceiver r;
        r.configure(kRate); r.setSignalOffset(-generalTuning().tuneOffsetHz); r.setSilent(true);
        r.setSquelch(true, true, -60);
        r.setRecordMp3(true);
        r.setTransmissionRecording(true, ".", "general_rx_mp", 0.5);
        std::vector<cf32> buf(20000);
        auto feedSecs = [&](std::unique_ptr<ModeSynth>& syn, double secs) {
            for (double t = 0; t < secs; t += buf.size() / kRate) { syn->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        };
        feedSecs(sOff, 1.0); feedSecs(sOn, 2.0); feedSecs(sOff, 2.0);
        r.setTransmissionRecording(false, ".", "general_rx_mp");
        int files = 0; long sz = 0; bool sync = false;
        for (int i = 1; i <= 4; i++) {
            char n[64]; snprintf(n, sizeof n, "general_rx_mp_%d.mp3", i);
            if (FILE* f = fopen(n, "rb")) {
                unsigned char h[4] = {};
                const size_t got = fread(h, 1, 4, f);
                fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); remove(n); files++;
                sync = got == 4 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0;
            }
        }
        printf("  mp3 files: %d, %ld bytes\n", files, sz);
        CHECK(files == 1 && sync && sz > 10000 && sz < 40000, "mp3 recording: %d files, %ld bytes, sync %d", files, sz, (int)sync);
    }

    // the same as FLAC: the files start with fLaC, STREAMINFO holds the sample count, and they are smaller than the WAV would be
    {
        SynthConfig on, off;
        on.mode = 57; on.snrDb = 30; off.mode = 57; off.snrDb = 30; off.modeOpt[1] = 1;
        auto sOn = makeGeneralSynth(on, kRate), sOff = makeGeneralSynth(off, kRate);
        GeneralReceiver r;
        r.configure(kRate); r.setSignalOffset(-generalTuning().tuneOffsetHz); r.setSilent(true);
        r.setSquelch(true, true, -60);
        r.setRecordFormat(true, {"TITLE=test"});
        r.setTransmissionRecording(true, ".", "general_rx_fl", 0.5);
        std::vector<cf32> buf(20000);
        auto feedSecs = [&](std::unique_ptr<ModeSynth>& syn, double secs) {
            for (double t = 0; t < secs; t += buf.size() / kRate) { syn->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        };
        feedSecs(sOff, 1.0); feedSecs(sOn, 2.0); feedSecs(sOff, 2.0);
        r.setTransmissionRecording(false, ".", "general_rx_fl");
        int files = 0; bool magic = true; long sz = 0; uint64_t total = 0;
        for (int i = 1; i <= 4; i++) {
            char n[64]; snprintf(n, sizeof n, "general_rx_fl_%d.flac", i);
            if (FILE* f = fopen(n, "rb")) {
                unsigned char h[42] = {};
                const size_t got = fread(h, 1, 42, f);
                fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); remove(n); files++;
                magic = magic && got == 42 && !memcmp(h, "fLaC", 4);
                total = ((uint64_t)(h[21] & 0x0F) << 32) | (uint64_t)h[22] << 24 | h[23] << 16 | h[24] << 8 | h[25];
            }
        }
        printf("  flac files: %d, %ld bytes, %llu samples\n", files, sz, (unsigned long long)total);
        CHECK(files == 1 && magic && total > 90000 && sz < (long)total * 2, "flac recording");
    }

    // audio filters: a 100 Hz low-pass takes most of the power out of the tone / speech the synthesiser sends
    {
        SynthConfig on; on.mode = 57; on.snrDb = 30;
        auto power = [&](double lp) {
            auto syn = makeGeneralSynth(on, kRate);
            GeneralReceiver r;
            r.configure(kRate); r.setSignalOffset(-generalTuning().tuneOffsetHz); r.setSilent(true);
            r.setSquelch(false, false, -60);
            r.setAudioFilters(0, lp, 0);
            double sum = 0; size_t cnt = 0;
            r.setAudioTap([&](const float* x, size_t n) { for (size_t i = 0; i < n; i++) sum += (double)x[i] * x[i]; cnt += n; });
            std::vector<cf32> buf(20000);
            for (double t = 0; t < 3.0; t += buf.size() / kRate) { syn->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
            return cnt ? sum / cnt : 0.0;
        };
        const double pOff = power(0), pLp = power(100);
        printf("  audio power: filters off %.5f, 100 Hz low-pass %.5f\n", pOff, pLp);
        CHECK(pOff > 1e-6 && pLp < 0.5 * pOff, "low-pass reduces the audio power");
    }

    // single-file recording: with the squelch on only the burst + pre-roll + hang is written; with it off everything is
    {
        SynthConfig on, off;
        on.mode = 57; on.snrDb = 30; off.mode = 57; off.snrDb = 30; off.modeOpt[1] = 1;
        auto len = [&](bool sq) {
            auto sOn = makeGeneralSynth(on, kRate), sOff = makeGeneralSynth(off, kRate);
            GeneralReceiver r;
            r.configure(kRate); r.setSignalOffset(-generalTuning().tuneOffsetHz); r.setSilent(true);
            r.setDemod(GenNfm); r.setSquelch(sq, true, -60);
            r.setTransmissionGate(0.5, 0.5, false, false);
            const std::string path = "general_rx_sqrec.wav";
            r.startRecording(path);
            std::vector<cf32> buf(20000);
            auto feedSecs = [&](std::unique_ptr<ModeSynth>& syn, double secs) {
                for (double t = 0; t < secs; t += buf.size() / kRate) { syn->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
            };
            feedSecs(sOff, 1.0); feedSecs(sOn, 1.5); feedSecs(sOff, 4.0);
            r.stopRecording();
            FILE* f = fopen(path.c_str(), "rb");
            long size = 0;
            if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
            remove(path.c_str());
            return (size - 44) / 2.0 / 48000.0;
        };
        const double gated = len(true), all = len(false);
        printf("  squelch-gated single file: %.2f s (squelch on), %.2f s (off)\n", gated, all);
        CHECK(gated > 1.5 && gated < 4.0, "squelch on: file %.2f s, expected about 2.5", gated);
        CHECK(all > 6.0, "squelch off: file %.2f s, expected all 6.5", all);
    }

    // pre-roll and hang: two transmissions 0.8 s apart stay one file, the file is longer than the open time, and a
    // channel-IQ sibling plus a CSV line are written beside a name that carries the time, the frequency and the mode
    {
        SynthConfig on, off;
        on.mode = 57; on.snrDb = 30; off.mode = 57; off.snrDb = 30; off.modeOpt[1] = 1;
        auto sOn = makeGeneralSynth(on, kRate), sOff = makeGeneralSynth(off, kRate);
        GeneralReceiver r;
        r.configure(kRate); r.setSignalOffset(-generalTuning().tuneOffsetHz); r.setSilent(true);
        r.setDemod(GenNfm); r.setSquelch(true, true, -60);
        r.setListenHz(145.5e6);
        r.setTransmissionGate(1.0, 1.5, true, true);
        r.setTransmissionRecording(true, ".", "", 0.5);
        std::vector<cf32> buf(20000);
        auto feedSecs = [&](std::unique_ptr<ModeSynth>& syn, double secs) {
            for (double t = 0; t < secs; t += buf.size() / kRate) { syn->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        };
        feedSecs(sOff, 1.0); feedSecs(sOn, 1.5); feedSecs(sOff, 0.8); feedSecs(sOn, 1.5); feedSecs(sOff, 2.5);
        r.setTransmissionRecording(false, ".", "");
        GeneralTelemetry g;
        r.telemetry(g, 0);
        const std::string path = g.recordPath;
        FILE* f = fopen(path.c_str(), "rb");
        unsigned char h[44] = {};
        const size_t got = f ? fread(h, 1, 44, f) : 0;
        if (f) fclose(f);
        const uint32_t data = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
        std::string iq = path;
        const auto dot = iq.find_last_of('.');
        if (dot != std::string::npos) iq.resize(dot);
        iq += "_48k.cf32";
        FILE* iqF = fopen(iq.c_str(), "rb");
        long iqSz = 0;
        if (iqF) { fseek(iqF, 0, SEEK_END); iqSz = ftell(iqF); fclose(iqF); }
        std::string csv;
        if (FILE* c = fopen("transmissions.csv", "rb")) {
            char b[512];
            while (fgets(b, sizeof b, c)) csv += b;
            fclose(c);
        }
        printf("  hang file: %s, %u bytes, iq %ld, kept %d\n", path.c_str(), data, iqSz, r.transmissionFiles());
        CHECK(r.transmissionFiles() == 1, "hang kept two bursts as %d files", r.transmissionFiles());
        CHECK(path.find("145.500000MHz") != std::string::npos && path.find("_NFM") != std::string::npos, "name %s", path.c_str());
        CHECK(got == 44 && data > 4u * 48000u * 2u, "pre-roll file %u bytes", data);
        CHECK(iqSz > 48000L * 8L, "channel iq %ld", iqSz);
        CHECK(csv.find("NFM") != std::string::npos && csv.find("145500000") != std::string::npos, "csv [%s]", csv.c_str());
        remove(path.c_str()); remove(iq.c_str()); remove("transmissions.csv");
    }

    // neural noise remover: off (the default, and setNoiseRemover(false)) leaves the audio bit-identical; on gives as many samples, finite
    {
        auto go = [&](int mode) {   // 0 untouched, 1 explicitly off, 2 on
            SynthConfig sc;
            sc.mode = 57; sc.snrDb = 12; sc.modeOpt[0] = 0; sc.modeOpt[1] = 0; sc.modeVal[0] = 0; sc.modeVal[1] = 1000;
            auto syn = makeGeneralSynth(sc, kRate);
            GeneralReceiver r;
            r.configure(kRate);
            r.setSignalOffset(-generalTuning().tuneOffsetHz);
            r.setSilent(true);
            r.setDemod(GenNfm);
            r.setWidthHz(generalDefaultWidth(GenNfm));
            if (mode == 1) r.setNoiseRemover(false, 1.f);
            if (mode == 2) r.setNoiseRemover(true, 1.f);
            std::vector<float> a;
            r.setAudioTap([&](const float* x, size_t n) { a.insert(a.end(), x, x + n); });
            std::vector<cf32> buf(20000);
            for (double t = 0; t < 1.5; t += buf.size() / kRate) { syn->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
            return a;
        };
        const auto a0 = go(0), a1 = go(1), a2 = go(2);
        CHECK(!a0.empty() && a0.size() == a1.size() && std::memcmp(a0.data(), a1.data(), a0.size() * sizeof(float)) == 0, "noise remover off is bit-identical");
        bool fin = a2.size() == a0.size();
        for (float v : a2) fin = fin && std::isfinite(v) && std::fabs(v) <= 1.f;
        CHECK(fin, "noise remover on keeps the sample count (%zu vs %zu) and stays finite", a2.size(), a0.size());
    }

    printf(fails ? "general rx: %d FAILED\n" : "general rx: all passed\n", fails);
    return fails ? 1 : 0;
}
