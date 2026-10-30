// The SDR transmit path of the ham modes: the SSB chain's spectrum (opposite sideband, interpolation images, spurs, harmonics), the ALC and the
// power limit, and SdrHamIo through HamSlotTx into a recording sink (nothing is sent anywhere): keyed only through the gate, tuned to the checked
// frequency, hard stop on a radio error, Halt, the monitor, and an FT8 burst decoded back from the IQ.
#include "../hfdig/hfdig_wsjt_testutil.h"
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_safety.h"
#include "dect2/ham/ham_sdr.h"
#include <chrono>
#include <thread>
#include "../common/cpu_report.h"
#include "../common/test_util.h"
using namespace dect2;
using namespace dect2::ham;
using namespace wsjttest;

namespace {
constexpr double kPi = 3.14159265358979323846;

// in-place radix-2 FFT
void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> wl = std::polar(1.0, -2 * kPi / (double)len);
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w = 1;
            for (size_t k = 0; k < len / 2; k++, w *= wl) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
            }
        }
    }
}
// power spectrum in dB relative to its peak, Blackman-Harris window, frequencies -rate/2 .. +rate/2 (index n/2 = 0 Hz)
struct Spec {
    std::vector<double> db;
    double rate = 0, binHz = 0;
    double at(double hz, double spanHz) const {   // the highest level within +- spanHz of hz
        const long n = (long)db.size(), c = n / 2 + (long)std::lround(hz / binHz), w = std::max(1L, (long)(spanHz / binHz));
        double m = -400;
        for (long i = c - w; i <= c + w; i++) if (i >= 0 && i < n) m = std::max(m, db[(size_t)i]);
        return m;
    }
    double maxOutside(double loHz, double hiHz) const {   // the highest level outside [lo, hi]
        double m = -400;
        for (size_t i = 0; i < db.size(); i++) {
            const double f = ((long)i - (long)db.size() / 2) * binHz;
            if (f < loHz || f > hiHz) m = std::max(m, db[i]);
        }
        return m;
    }
};
Spec spectrum(const std::vector<cf32>& x, double rate, size_t skip, size_t n) {
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; i++) {
        const double t = 2 * kPi * (double)i / (double)(n - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        const cf32 v = skip + i < x.size() ? x[skip + i] : cf32(0, 0);
        a[i] = std::complex<double>(v.real(), v.imag()) * w;
    }
    fft(a);
    Spec s;
    s.rate = rate; s.binHz = rate / (double)n;
    s.db.resize(n);
    double pk = 1e-300;
    for (size_t i = 0; i < n; i++) pk = std::max(pk, std::norm(a[i]));
    for (size_t i = 0; i < n; i++) s.db[(i + n / 2) % n] = 10 * std::log10(std::max(std::norm(a[i]), 1e-300) / pk);
    return s;
}
std::vector<float> tones(const std::vector<double>& hz, double amp, double sec) {
    std::vector<float> a((size_t)(sec * 48000));
    for (size_t i = 0; i < a.size(); i++) {
        double v = 0;
        for (double f : hz) v += std::cos(2 * kPi * f * (double)i / 48000);
        a[i] = (float)(amp * v / (double)hz.size());
    }
    return a;
}

// a radio that records what it is given; it never transmits
struct RecSink : TxSink {
    std::vector<cf32> iq;
    TxTune tune;
    int opens = 0, closes = 0;
    bool failAfter = false;   // the "radio" dies after 2 s of samples
    double paceSec = 0;       // sleep this long per write (a radio paces the caller)
    bool open(const TxTune& t, std::string&) override { tune = t; opens++; return true; }
    bool write(const cf32* x, size_t n, std::string& err) override {
        if (failAfter && iq.size() > (size_t)(2 * tune.sampleRate)) { err = "USB transfer failed"; return false; }
        iq.insert(iq.end(), x, x + n);
        if (paceSec > 0) std::this_thread::sleep_for(std::chrono::duration<double>(paceSec));
        return true;
    }
    void close() override { closes++; }
    double sampleRate() const override { return tune.sampleRate; }
    std::string name() const override { return "recording sink"; }
};

DeviceInfo hackrf() {
    DeviceInfo d;
    d.kind = DeviceInfo::HackRF; d.board = "HackRF One"; d.name = "HackRF One"; d.canTransmit = true;
    d.minFreqHz = 1e6; d.maxFreqHz = 6e9; d.txMinRateHz = 2e6; d.txMaxRateHz = 20e6; d.txGainMinDb = 0; d.txGainMaxDb = 47;
    return d;
}
HamTxCheck gate(double dial = 14.074e6) {
    HamTxCheck g; g.licenceConfirmed = true; g.armed = true; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = dial; g.audioHz = 1500; g.country = "LT"; return g;
}
SdrTxConfig cfgOk() { SdrTxConfig c; c.bandFilterConfirmed = true; c.rateHz = 960000; c.gainDb = 20; return c; }

// SSB demodulation of the recorded IQ (the real part at 48 kHz), then 8 kHz for the decoder as test_ham_modes does
std::string decodeIq(const std::vector<cf32>& iq, double rate, int mode) {
    const int r = (int)std::lround(rate / 48000);
    std::vector<float> a48(iq.size() / (size_t)r);
    for (size_t i = 0; i < a48.size(); i++) { double s = 0; for (int k = 0; k < r; k++) s += iq[i * (size_t)r + (size_t)k].real(); a48[i] = (float)(s / r); }
    const double P = ftxPeriod(mode), t0 = t0Of(mode, 0);
    std::vector<float> a((size_t)((t0 + P + 4) * 8000), 0.f);
    std::vector<double> h;
    const int N = 96;
    for (int k = -N; k <= N; k++) {
        const double x = k * 2 * 3500.0 / 48000.0 * kPi, w = 0.5 + 0.5 * std::cos(kPi * k / (N + 1));
        h.push_back((k == 0 ? 1.0 : std::sin(x) / x) * w * 2 * 3500.0 / 48000.0);
    }
    for (size_t o = 0; o < a.size(); o++) {
        const long c = (long)(o - (size_t)(t0 * 8000)) * 6;
        if (c < 0 && -c > N) continue;
        double acc = 0;
        for (long k = -N; k <= N; k++) { const long i = c + k; if (i >= 0 && i < (long)a48.size()) acc += h[(size_t)(k + N)] * a48[(size_t)i]; }
        a[o] = (float)acc;
    }
    ftxtest::Gauss g(7);
    for (auto& v : a) v += (float)(ftxtest::kSigma * g.g());
    Imp f;
    const auto d = decode(a, mode, f);
    return d.empty() ? std::string() : d[0].msg;
}
}  // namespace

int main() {
    std::string err;
    // ---- the chain: one tone ----
    for (double rate : {960000.0, 1000000.0}) {   // the integer path (x20) and the fractional one
        SdrTxChain ch(rate, cfgOk());
        std::vector<cf32> out;
        const auto a = tones({1500}, 0.5, 1.2);
        const auto t0 = std::chrono::steady_clock::now();
        ch.process(a.data(), a.size(), out);
        const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        cpuCal("ham_sdr", "1.2 s of audio at " + std::to_string((long long)rate) + " Hz", cpu, "wall-s", '<', 1.2);
        CHECK(cpu < 1.2, "rate %.0f: 1.2 s of audio took %.2f s of CPU (must run faster than real time)", rate, cpu);
        CHECK(std::fabs((double)out.size() - 1.2 * rate) < 4 * rate / 48000, "rate %.0f: %zu samples for 1.2 s", rate, out.size());
        float early = 0;
        for (size_t i = 0; i < (size_t)(rate * 0.001); i++) early = std::max(early, std::abs(out[i]));
        CHECK(early < 0.05f, "rate %.0f: the start ramps up (first ms peak %.3f)", rate, early);
        const Spec s = spectrum(out, rate, (size_t)(0.1 * rate), 1u << 19);
        CHECK(s.at(1500, 4) > -1, "rate %.0f: the tone is on the upper sideband at +1500 Hz", rate);
        CHECK(s.at(-1500, 20) < -75, "rate %.0f: opposite sideband %.1f dBc", rate, s.at(-1500, 20));
        CHECK(s.at(48000 - 1500, 30) < -100 && s.at(48000 + 1500, 30) < -100 && s.at(-48000 + 1500, 30) < -100,
              "rate %.0f: interpolation images %.1f / %.1f dBc", rate, s.at(48000 - 1500, 30), s.at(48000 + 1500, 30));
        CHECK(s.at(96000 + 1500, 30) < -110, "rate %.0f: second image %.1f dBc", rate, s.at(96000 + 1500, 30));
        CHECK(s.at(3000, 10) < -60 && s.at(4500, 10) < -60, "rate %.0f: harmonics %.1f / %.1f dBc", rate, s.at(3000, 10), s.at(4500, 10));
        CHECK(s.at(0, 10) < -60, "rate %.0f: no carrier at the centre (%.1f dBc)", rate, s.at(0, 10));
        CHECK(s.maxOutside(1500 - 40, 1500 + 40) < -75, "rate %.0f: spectral mask, the highest spur %.1f dBc", rate, s.maxOutside(1500 - 40, 1500 + 40));
        printf("rate %.0f: opposite sideband %.1f, images %.1f / %.1f, 2nd image %.1f, centre %.1f, worst spur %.1f dBc, CPU %.0f ms\n", rate,
               s.at(-1500, 20), s.at(48000 - 1500, 30), s.at(48000 + 1500, 30), s.at(96000 + 1500, 30), s.at(0, 10), s.maxOutside(1460, 1540), cpu * 1e3);
        const auto st = ch.stats();
        CHECK(std::fabs(st.outPeak - 0.5f) < 0.03f && st.alcDb > -0.1f, "rate %.0f: level kept (peak %.3f, ALC %.2f dB)", rate, st.outPeak, st.alcDb);
        CHECK(ch.latencySec() > 0.003 && ch.latencySec() < 0.005, "chain latency %.4f s", ch.latencySec());
    }
    {   // LSB
        SdrTxConfig c = cfgOk(); c.lsb = true;
        SdrTxChain ch(960000, c);
        std::vector<cf32> out;
        const auto a = tones({1500}, 0.5, 0.6);
        ch.process(a.data(), a.size(), out);
        const Spec s = spectrum(out, 960000, 48000, 1u << 18);
        CHECK(s.at(-1500, 4) > -1 && s.at(1500, 20) < -60, "LSB: the tone at -1500 Hz, +1500 %.1f dBc", s.at(1500, 20));
    }
    // ---- ALC and the power limit ----
    {
        SdrTxConfig c = cfgOk(); c.powerLimitPct = 25; c.drive = 4;   // 4x too much drive into a 25 % power limit (amplitude 0.5)
        SdrTxChain ch(960000, c);
        std::vector<cf32> out;
        const auto a = tones({1500}, 0.9, 1.2);
        ch.process(a.data(), a.size(), out);
        float pk = 0;
        for (const auto& v : out) pk = std::max(pk, std::abs(v));
        const auto st = ch.stats();
        CHECK(pk <= 0.5f + 1e-4f && pk > 0.45f, "power limit 25 %%: peak %.4f (limit 0.5)", pk);
        CHECK(st.alcDb < -10 && st.alcSamples > 0, "ALC reduced the gain: %.1f dB", st.alcDb);
        const Spec s = spectrum(out, 960000, (size_t)(0.1 * 960000), 1u << 19);
        CHECK(s.at(3000, 10) < -60 && s.at(4500, 10) < -60 && s.at(-1500, 20) < -60, "overdriven tone stays clean: %.1f / %.1f dBc", s.at(3000, 10), s.at(4500, 10));
    }
    {   // two tones overdriven: the ALC is a gain control, not a clipper (intermodulation stays low)
        SdrTxConfig c = cfgOk(); c.drive = 3; c.powerLimitPct = 50;
        SdrTxChain ch(960000, c);
        std::vector<cf32> out;
        const auto a = tones({1000, 1700}, 0.9, 1.5);
        ch.process(a.data(), a.size(), out);
        const Spec s = spectrum(out, 960000, (size_t)(0.4 * 960000), 1u << 19);
        const double imd = std::max(s.at(300, 6), s.at(2400, 6));
        printf("two tones, ALC %.1f dB: IMD3 %.1f dBc\n", ch.stats().alcDb, imd);
        CHECK(imd < -50, "two-tone IMD3 %.1f dBc", imd);
        float pk = 0;
        for (const auto& v : out) pk = std::max(pk, std::abs(v));
        CHECK(pk <= (float)std::sqrt(0.5) + 1e-4f, "two tones: peak %.4f within the 50 %% power limit", pk);
    }
    {   // the monitor gets what is sent
        SdrTxChain ch(960000, cfgOk());
        size_t got = 0; float pk = 0;
        ch.monitor = [&](const float* m, size_t n) { got += n; for (size_t i = 0; i < n; i++) pk = std::max(pk, std::fabs(m[i])); };
        std::vector<cf32> out;
        const auto a = tones({1500}, 0.5, 0.5);
        ch.process(a.data(), a.size(), out);
        CHECK(got == a.size() && std::fabs(pk - 0.5f) < 0.03f, "monitor: %zu frames, peak %.3f", got, pk);
    }

    // ---- refusals and warnings ----
    {
        const DeviceInfo d = hackrf();
        std::string why;
        SdrTxConfig c = cfgOk(); c.bandFilterConfirmed = false;
        CHECK(!sdrTxRefused(d, c, 14.074e6, 1500, 50, why), "no band filter confirmed: a warning (the prepare text), not a refusal (%s)", why.c_str());
        CHECK(!sdrTxRefused(d, cfgOk(), 14.074e6, 1500, 50, why), "a HackRF with a filter: allowed (%s)", why.c_str());
        CHECK(!sdrTxRefused(d, cfgOk(), 14.074e6, 2950, 50, why), "audio above 2900 Hz: not refused (3.2)");
        CHECK(sdrTxLimitWarnings(d, cfgOk(), 14.074e6, 2950, 50).find("300 to 2900") != std::string::npos, "audio above 2900 Hz: a warning");
        CHECK(!sdrTxRefused(d, cfgOk(), 14.074e6, 200, 50, why) && !sdrTxLimitWarnings(d, cfgOk(), 14.074e6, 200, 50).empty(), "audio below 300 Hz: a warning");
        CHECK(sdrTxLimitWarnings(d, cfgOk(), 14.074e6, 1500, 50).empty(), "inside the window: no limit warning");
        c = cfgOk(); c.gainDb = 60;
        CHECK(!sdrTxRefused(d, c, 14.074e6, 1500, 50, why) && sdrTxLimitWarnings(d, c, 14.074e6, 1500, 50).find("damage") != std::string::npos, "gain above the radio's range: a warning that says it can damage the radio");
        c = cfgOk(); c.powerLimitPct = 0;
        CHECK(!sdrTxRefused(d, c, 14.074e6, 1500, 50, why) && !sdrTxLimitWarnings(d, c, 14.074e6, 1500, 50).empty(), "power limit 0: a warning");
        DeviceInfo rx = d; rx.canTransmit = false;
        CHECK(sdrTxRefused(rx, cfgOk(), 14.074e6, 1500, 50, why), "a receive-only radio cannot: refused (nothing to send with)");
        DeviceInfo pluto = d; pluto.board = "PlutoSDR"; pluto.name = "PlutoSDR"; pluto.minFreqHz = 325e6; pluto.maxFreqHz = 3.8e9;
        CHECK(!sdrTxRefused(pluto, cfgOk(), 14.074e6, 1500, 50, why) && !sdrTxLimitWarnings(pluto, cfgOk(), 14.074e6, 1500, 50).empty(), "outside the radio's range: a warning");
        c = cfgOk(); c.ampOn = true;
        CHECK(!sdrTxRefused(pluto, c, 432.174e6, 1500, 50, why) && sdrTxLimitWarnings(pluto, c, 432.174e6, 1500, 50).find("HackRF") != std::string::npos, "the amp switch on a radio that has none: a warning");
        CHECK(sdrTxLimitWarnings(d, c, 14.074e6, 1500, 50).find("overdriving") != std::string::npos, "the HackRF amplifier: a warning, no gain cap");
        const auto w = sdrTxWarnings(d, cfgOk(), 7.074e6);
        bool pa = false, filt = false, hf = false, duplex = false;
        for (const auto& s : w) { pa |= s.find("no power amplifier") != std::string::npos; filt |= s.find("filter") != std::string::npos;
                                  hf |= s.find("harmonics are strong") != std::string::npos; duplex |= s.find("duplex") != std::string::npos; }
        CHECK(pa && filt && hf && duplex, "HackRF warnings: no PA, band filter, HF harmonics, duplex note");
        CHECK(sdrTxRate(d, SdrTxConfig()) == 2016000, "HackRF rate %.0f (42 x 48 kHz)", sdrTxRate(d, SdrTxConfig()));
        DeviceInfo slow = d; slow.txMinRateHz = 521e3; slow.txMaxRateHz = 61.44e6;
        CHECK(sdrTxRate(slow, SdrTxConfig()) == 1920000, "a radio from 521 kHz: %.0f", sdrTxRate(slow, SdrTxConfig()));
    }

    // ---- SdrHamIo through HamSlotTx ----
    RecSink* last = nullptr;
    int made = 0;
    auto factory = [&](std::string&) -> std::unique_ptr<TxSink> { auto s = std::make_unique<RecSink>(); last = s.get(); made++; return s; };
    std::vector<cf32> sent;   // sinks are owned by SdrHamIo and destroyed at the end of a burst: this one copies what it recorded
    double sentRate = 0, sentHz = 0;
    auto recording = [&](std::string&) -> std::unique_ptr<TxSink> {
        struct Copy : RecSink { std::vector<cf32>* out; double* r; double* hz; ~Copy() override { *out = iq; *r = tune.sampleRate; *hz = tune.centerHz; } };
        auto s = std::make_unique<Copy>(); s->out = &sent; s->r = &sentRate; s->hz = &sentHz; made++; return s;
    };
    {   // keyed only through the gate: direct calls refuse and make no sink
        SdrHamIo io(hackrf(), factory, cfgOk());
        made = 0;
        CHECK(!io.ptt(true, err) && made == 0 && !io.keyed(), "ptt without prepare: refused, no radio opened (%s)", err.c_str());
        float z[480] = {};
        CHECK(io.write(z, 480) < 0, "audio while not keyed is an error");
        HamTxCheck g = gate(); g.armed = false;
        CHECK(!io.prepare(g, err) && !io.ptt(true, err) && made == 0, "prepare with a refused gate: nothing keyed");
        g = gate();
        CHECK(io.prepare(g, err) && io.ptt(true, err) && made == 1 && io.keyed(), "prepared then keyed (%s)", err.c_str());
        CHECK(last && std::fabs(last->tune.centerHz - 14.074e6) < 1e-3 && last->tune.sampleRate == 960000 && last->tune.gainDb == 20, "tuned to the checked frequency");
        io.ptt(false, err);
        CHECK(!io.keyed() && !io.ptt(true, err) && made == 1, "one prepare is one burst: keying again needs the gate again");
    }
    {   // every combination of licence, Arm and band: only Arm keys the SDR (the rest are warnings)
        int keyed = 0;
        for (int m = 0; m < 8; m++) {
            made = 0; sent.clear();
            SdrHamIo io(hackrf(), recording, cfgOk());
            HamSlotTx tx(io);
            HamTxCheck g = gate(m & 4 ? 14.074e6 : 14.36e6);   // 14.36 MHz: above 20 m
            g.licenceConfirmed = m & 1; g.armed = m & 2;
            const bool ok = tx.send(g, 2, "CQ LY1ABC KO24", 1500, 0.5, err);   // FT2: short
            const bool arm = (m & 2) != 0;
            CHECK(ok == arm && (made > 0) == arm && sent.empty() == !arm, "licence %d arm %d in band %d: sent %d, radio opened %d",
                  m & 1, (m >> 1) & 1, (m >> 2) & 1, ok, made);
            keyed += made;
        }
        CHECK(keyed == 4, "the radio opened in the 4 armed combinations of 8");
    }
    {   // the country plan: 7.25 MHz is outside Region 1's 40 m (Lithuania), inside it in the US
        SdrHamIo io(hackrf(), recording, cfgOk());
        HamSlotTx tx(io);
        HamTxCheck g = gate(7.25e6);
        made = 0;
        CHECK(tx.send(g, 2, "CQ LY1ABC KO24", 1500, 0.5, err) && made == 1, "LT 7.25 MHz is outside the plan: warned, then sent (%s)", err.c_str());
        g.country = "US";
        made = 0;
        CHECK(tx.send(g, 2, "CQ LY1ABC KO24", 1500, 0.5, err) && made == 1, "US 7.25 MHz sent (%s)", err.c_str());
    }
    {   // a full FT8 burst over the SDR path, decoded back from the IQ
        SdrHamIo io(hackrf(), recording, cfgOk());
        size_t mon = 0;
        io.setMonitor([&](const float*, size_t n) { mon += n; });
        HamSlotTx tx(io);
        sent.clear();
        CHECK(tx.send(gate(), 0, "CQ LY1ABC KO24", 1500, 0.5, err), "FT8 over the SDR: %s", err.c_str());
        CHECK(sentRate == 960000 && std::fabs(sentHz - 14.074e6) < 1e-3, "FT8: rate %.0f at %.0f Hz", sentRate, sentHz);
        const double sec = (double)sent.size() / 960000;
        CHECK(sec > 12.6 && sec < 13.3, "FT8 burst %.2f s of IQ (12.64 s + the filter tail)", sec);
        CHECK(mon > (size_t)(12.6 * 48000), "monitor: %zu frames", mon);
        float pk = 0;
        for (const auto& v : sent) pk = std::max(pk, std::abs(v));
        CHECK(pk > 0.4f && pk <= 0.99f, "FT8 peak %.3f", pk);
        const Spec s = spectrum(sent, 960000, 960000, 1u << 22);
        printf("FT8 over the SDR: outside 1400..1650 Hz %.1f dBc, opposite sideband %.1f dBc, peak %.3f\n", s.maxOutside(1400, 1650), s.at(-1525, 60), pk);
        CHECK(s.maxOutside(1400, 1650) < -70, "FT8 spectral mask: the highest level outside 1400..1650 Hz %.1f dBc", s.maxOutside(1400, 1650));
        CHECK(s.at(-1525, 60) < -70, "FT8 opposite sideband %.1f dBc", s.at(-1525, 60));
        const std::string got = decodeIq(sent, 960000, 0);
        CHECK(got == "CQ LY1ABC KO24", "FT8 decoded back from the IQ: \"%s\"", got.c_str());
        bool audited = false;
        for (const auto& e : txAuditRecent(5)) audited |= e.path == "sdr" && e.result == "sent" && e.mode == "FT8" && e.seconds > 12;
        CHECK(audited, "the SDR burst is in the audit log");
    }
    {   // the radio fails during the burst: hard stop, closed, reported
        SdrHamIo io(hackrf(), [&](std::string&) -> std::unique_ptr<TxSink> { auto s = std::make_unique<RecSink>(); s->failAfter = true; last = s.get(); return s; }, cfgOk());
        HamSlotTx tx(io);
        CHECK(!tx.send(gate(), 0, "CQ LY1ABC KO24", 1500, 0.5, err) && err.find("radio stopped") != std::string::npos && !io.keyed(), "radio error: stopped (%s)", err.c_str());
        bool failed = false;
        for (const auto& e : txAuditRecent(3)) failed |= e.result == "failed" && e.path == "sdr";
        CHECK(failed, "the failure is in the audit log");
    }
    {   // Halt during a paced burst: the radio is closed within a block
        int closes = 0;
        struct Slow : RecSink { int* c; ~Slow() override { (*c)++; } };
        SdrHamIo io(hackrf(), [&](std::string&) -> std::unique_ptr<TxSink> { auto s = std::make_unique<Slow>(); s->c = &closes; s->paceSec = 0.01; return s; }, cfgOk());
        HamSlotTx tx(io);
        bool ok = true;
        std::thread th([&] { std::string e; ok = tx.send(gate(), 0, "CQ LY1ABC KO24", 1500, 0.5, e); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto t0 = std::chrono::steady_clock::now();
        tx.abort();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        th.join();
        CHECK(!ok && !io.keyed() && closes == 1 && ms < 100, "Halt: closed in %.0f ms", ms);
    }
    {   // the chain must keep up with the radio: 2 MHz output faster than real time with room to spare
        SdrTxChain ch(2016000, cfgOk());
        std::vector<cf32> out;
        const auto a = tones({1500}, 0.5, 2.0);
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < a.size(); i += 480) { out.clear(); ch.process(a.data() + i, std::min<size_t>(480, a.size() - i), out); }
        const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("SSB chain at 2.016 MHz: %.1f %% of real time\n", cpu / 2.0 * 100);
        cpuCal("ham_sdr", "SSB chain, 2 s of audio to 2.016 MHz", cpu, "wall-s", '<', 1.0);
        CHECK(cpu < 1.0, "2 s of audio to 2.016 MHz IQ in %.2f s", cpu);
    }
    return testSummary("ham_sdr");
}
