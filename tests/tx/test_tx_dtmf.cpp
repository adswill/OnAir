// DTMF keypad of the analog voice transmitter (tx_dtmf.h): the keys go through the real TX generators (NFM of a digital-voice mode's
// "analog" box, airband AM, QO-100 USB) and are read back by OnAir's own tone decoder (tones_rx.h) from the IQ, with a carrier offset,
// noise, every key, several tone / gap lengths and twist; the microphone (a 1 kHz tone) is muted while a key plays. In memory only:
// NOTHING is transmitted.
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_dtmf.h"
#include "dect2/tones/tones_rx.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// generate `sec` seconds of mode `id` at fs with keys queued at the start; offset the carrier by cfo and add noise for snrDb (in the whole band)
static std::vector<cf32> run(const char* id, TxRequest r, double fs, double sec, const std::string& keys, const TxDtmfTiming& t,
                             double cfo, double snrDb, uint32_t seed) {
    auto& q = TxDtmfQueue::global();
    q.clear();
    q.setTiming(t);
    std::string err;
    const TxModeSpec* m = txModeById(id);
    if (!m) { CHECK(false, "no mode %s", id); return {}; }
    auto g = makeTxGenerator(*m, r, fs, err);
    if (!g) { CHECK(false, "no generator for %s: %s", id, err.c_str()); return {}; }
    q.push(keys);
    std::vector<cf32> x((size_t)(sec * fs));
    for (size_t i = 0; i < x.size(); i += 65536) g->generate(x.data() + i, std::min<size_t>(65536, x.size() - i));
    double p = 0;
    for (const auto& v : x) p += std::norm(v);
    p /= (double)x.size();
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.f, (float)std::sqrt(p / std::pow(10.0, snrDb / 10.0) / 2));
    for (size_t i = 0; i < x.size(); i++) {
        x[i] *= std::polar(1.f, (float)(2 * M_PI * cfo * (double)i / fs));
        if (snrDb < 90) x[i] += cf32(nd(rng), nd(rng));
    }
    return x;
}

static std::string decode(const std::vector<cf32>& x, double fs, double sigOff, int demod) {
    TonesReceiver rx;
    rx.configure(fs);
    rx.setDemod(demod);
    rx.setSignalOffset(sigOff);
    for (size_t i = 0; i < x.size(); i += 50000) rx.feed(x.data() + i, std::min<size_t>(50000, x.size() - i));
    std::vector<cf32> z(50000, cf32(0, 0));
    for (int k = 0; k < (int)(fs / 50000); k++) rx.feed(z.data(), z.size());   // a second of silence closes the last key
    TonesTelemetry tel;
    rx.telemetry(tel, 0);
    std::string all;
    for (const auto& e : tel.events) if (e.kind == TkDtmf) all += e.text;
    return all;
}

int main() {
    // ---- tables and the queue
    {
        double r, c;
        CHECK(txDtmfFreqs('5', r, c) && r == 770 && c == 1336, "5 = 770 + 1336");
        CHECK(txDtmfFreqs('d', r, c) && r == 941 && c == 1633, "d = D = 941 + 1633");
        CHECK(!txDtmfValidKey('E') && !txDtmfValidKey(' '), "E and space are not keys");
        CHECK(txDtmfClean("12 3a-#*x") == "123A#*", "clean: %s", txDtmfClean("12 3a-#*x").c_str());
        TxDtmfQueue q;
        CHECK(q.push(std::string(300, '1')) == 256, "at most 256 keys wait");
        TxDtmfTiming t; t.toneMs = 1; t.gapMs = 5000; t.levelPct = 99;
        q.setTiming(t);
        CHECK(q.timing().toneMs == 10 && q.timing().gapMs == 2000 && q.timing().levelPct == 50, "timing kept in range");
        // the audio length is exactly the keys' tones and gaps
        TxDtmfTiming u; u.toneMs = 80; u.gapMs = 40;
        const auto a = txDtmfAudio("123", 8000, u);
        CHECK(a.size() == 3 * (640 + 320), "audio length %zu", a.size());
        float pk = 0; for (float v : a) pk = std::max(pk, std::fabs(v));
        CHECK(pk > 0.6f && pk <= 0.71f, "two tones at 35 %% each: peak %.3f", pk);
    }
    const std::string all16 = "123A456B789C*0#D";
    // ---- the tone pairs alone, decoded as audio: every key at three tone lengths
    for (double ms : {50.0, 100.0, 200.0}) {
        TxDtmfTiming t; t.toneMs = ms; t.gapMs = ms;
        const auto a = txDtmfAudio(all16, 48000, t);
        TonesReceiver rx;
        rx.setAudioInput(true);
        rx.configure(48000);
        std::vector<cf32> b(a.size() + 48000, cf32(0, 0));
        for (size_t i = 0; i < a.size(); i++) b[i] = cf32(a[i], 0);
        rx.feed(b.data(), b.size());
        TonesTelemetry tel; rx.telemetry(tel, 0);
        std::string got; for (const auto& e : tel.events) if (e.kind == TkDtmf) got += e.text;
        CHECK(got == all16, "audio %g ms: %s", ms, got.c_str());
    }
    // ---- NFM (DMR's "send analog NFM" box): carrier offsets, noise, tone lengths, twist; the microphone is a 1 kHz tone
    {
        const double fs = 1e6;
        TxRequest r; r.modeId = "dmr"; r.analogVoice = true; r.audioSrc = 2; r.toneHz = 1000;
        const double off = -txModeTuneOffsetHz(*txModeById("dmr"));
        int ok = 0, n = 0;
        for (double cfo : {0.0, 1500.0, -2500.0})
            for (double snr : {30.0, 15.0})
                for (double ms : {60.0, 100.0}) {
                    TxDtmfTiming t; t.toneMs = ms; t.gapMs = ms; t.twistDb = (n % 3) - 1.0;
                    const double sec = 0.3 + all16.size() * 2 * ms * 1e-3;
                    const auto x = run("dmr", r, fs, sec, all16, t, cfo, snr, 10 + n);
                    const std::string got = decode(x, fs, off, TonesFm);
                    n++;
                    if (got == all16) ok++; else CHECK(false, "NFM cfo %g snr %g tone %g ms twist %g: %s", cfo, snr, ms, t.twistDb, got.c_str());
                }
        printf("nfm: %d of %d sweeps decode all 16 keys\n", ok, n);
        // the 1 kHz microphone tone is muted while keys play and comes back after them
        TxDtmfTiming t; t.toneMs = 100; t.gapMs = 100;
        const auto x = run("dmr", r, fs, 1.0, "55", t, 0, 99, 1);
        // FM discriminator, then the 1 kHz content in 20 ms windows inside a key (60..80 ms) and after the keys (600..620 ms)
        auto tone1k = [&](double t0) {
            std::complex<double> s = 0;
            const size_t a = (size_t)(t0 * fs), b = a + (size_t)(0.02 * fs);
            for (size_t i = a + 1; i < b; i++) {
                const cf32 d = x[i] * std::conj(x[i - 1]);
                const double sh = std::arg(std::complex<double>(d) * std::polar(1.0, -2 * M_PI * off / fs));
                s += sh * std::polar(1.0, -2 * M_PI * 1000.0 * (double)i / fs);
            }
            return std::abs(s) / (double)(b - a);
        };
        const double in = tone1k(0.06), after = tone1k(0.6);
        CHECK(after > 20 * in, "mic muted during the key: 1 kHz %.2e in the key, %.2e after", in, after);
    }
    // ---- airband AM and QO-100 USB: the same keys through the other analog modulations
    {
        TxRequest r; r.modeId = "airband"; r.audioSrc = 3;
        const double fs = 1e6, off = -txModeTuneOffsetHz(*txModeById("airband"));
        TxDtmfTiming t; t.toneMs = 80; t.gapMs = 80;
        for (double snr : {30.0, 15.0}) {
            const auto x = run("airband", r, fs, 0.3 + all16.size() * 0.16, all16, t, 300, snr, 77);
            const std::string got = decode(x, fs, off, TonesAm);
            CHECK(got == all16, "AM snr %g: %s", snr, got.c_str());
        }
        TxRequest u; u.modeId = "qo100"; u.kind = 2; u.audioSrc = 3;
        const double fs2 = 2.5e6, off2 = -txModeTuneOffsetHz(*txModeById("qo100"));
        const auto y = run("qo100", u, fs2, 0.3 + 5 * 0.16, "#0123", t, 0, 30, 78);
        const std::string got = decode(y, fs2, off2, TonesUsb);
        CHECK(got == "#0123", "USB: %s", got.c_str());
    }
    // ---- dtmf.off: the generator ignores the queue
    {
        TxRequest r; r.modeId = "dmr"; r.analogVoice = true; r.audioSrc = 3; r.opt["dtmf.off"] = 1;
        TxDtmfTiming t;
        const auto x = run("dmr", r, 1e6, 0.6, "1", t, 0, 99, 2);
        CHECK(decode(x, 1e6, -txModeTuneOffsetHz(*txModeById("dmr")), TonesFm).empty(), "dtmf.off sends no key");
        TxDtmfQueue::global().clear();
    }
    printf("%s (%d failures)\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
