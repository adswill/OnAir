// Offset tuning (offset_tune.h): the plan puts the channel clear of DC with the rate it needs, refuses radios that are too slow, and the
// mixer brings a signal at +offset back to DC with no phase jump between blocks and no drift in level over a long run.
#include "dect2/offset_tune.h"
#include "dect2/dab.h"
#include "dect2/dab_gen.h"
#include "dect2/fm_gen.h"
#include "dect2/fm_rx.h"
#include "dect2/iq_correct.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    {   // DAB on a HackRF: 1.536 MHz channel, mode rate 2.048 Msps
        const OffsetPlan p = planOffset(1.536e6, 2.048e6, 20e6);
        printf("DAB: offset %.0f Hz, rate %.3f Msps\n", p.offsetHz, p.rateHz / 1e6);
        CHECK(p.ok && p.offsetHz - 1.536e6 / 2 >= 25e3, "DAB: the channel's lower edge is not clear of DC");
        CHECK(p.rateHz / 2 >= p.offsetHz + 1.536e6 / 2, "DAB: the channel does not fit below the upper band edge");
        CHECK(p.rateHz <= 4e6, "DAB: more rate than needed (%.2f Msps)", p.rateHz / 1e6);
    }
    {   // FM: the mode's own rate already holds the offset channel
        const OffsetPlan p = planOffset(200e3, 2.4e6, 3.2e6);
        CHECK(p.ok && p.rateHz == 2.4e6, "FM: the rate changed to %.3f Msps", p.rateHz / 1e6);
    }
    {   // DVB-T 8 MHz: a HackRF can, an SDRplay (10 Msps) cannot
        const OffsetPlan h = planOffset(8e6, 10e6, 20e6), s = planOffset(8e6, 10e6, 10e6);
        printf("DVB-T 8 MHz: HackRF %d (%.2f Msps), SDRplay %d (%s)\n", h.ok, h.rateHz / 1e6, s.ok, s.why.c_str());
        CHECK(h.ok && h.rateHz <= 20e6, "DVB-T on a HackRF refused");
        CHECK(!s.ok && !s.why.empty(), "DVB-T on a 10 Msps radio accepted");
    }
    {   // the mixer: a tone at +offset comes out as a constant at DC, block after block, for 10^8 samples
        const double fs = 3.5e6, off = 845e3;
        OffsetMixer m; m.set(off, fs);
        std::vector<cf32> x(65536);
        double t = 0, maxPhaseErr = 0, minMag = 1e9, maxMag = 0;
        for (int blk = 0; blk < 1600; blk++) {
            for (size_t i = 0; i < x.size(); i++, t += 1) x[i] = std::polar(1.0f, (float)std::fmod(2 * M_PI * off * t / fs, 2 * M_PI));
            m.mix(x.data(), x.size());
            for (size_t i = 0; i < x.size(); i += 997) {
                maxPhaseErr = std::max(maxPhaseErr, (double)std::fabs(std::arg(x[i])));
                minMag = std::min(minMag, (double)std::abs(x[i])); maxMag = std::max(maxMag, (double)std::abs(x[i]));
            }
        }
        printf("mixer: max phase error %.4f rad, level %.5f..%.5f over %.0f samples\n", maxPhaseErr, minMag, maxMag, t);
        CHECK(maxPhaseErr < 0.01, "the tone is not at DC (phase error %.4f rad)", maxPhaseErr);
        CHECK(minMag > 0.999 && maxMag < 1.001, "the level drifted (%.5f..%.5f)", minMag, maxMag);
    }
    {   // DAB the way a radio delivers it with offset tuning: the ensemble at +offset at the planned rate, with a DC spike at the radio's
        // centre; mixed back, it decodes as well as without the offset (same FIBs, same services) and the spike is outside the channel
        auto runDab = [](bool offset) {
            const OffsetPlan p = planOffset(1.7e6, 2.048e6, 20e6);
            const double rate = offset ? p.rateHz : 2.048e6, off = offset ? p.offsetHz : 0;
            dabgen::TxConfig tc; tc.utcSeconds = 1700000000;
            SynthConfig sc; sc.snrDb = 25;
            auto syn = makeDabSynth(tc, sc, rate);
            DabReceiver rx; rx.configure(rate); rx.audio().setSilent(true); rx.select(0);
            OffsetMixer m; m.set(off, rate);
            std::vector<cf32> x(65536);
            double t = 0;
            for (size_t done = 0; done < (size_t)(6 * rate); done += x.size()) {
                syn->generate(x.data(), x.size());
                for (auto& v : x) { v *= std::polar(1.0f, (float)std::fmod(2 * M_PI * off * t / rate, 2 * M_PI)); v += cf32(0.05f, -0.03f); t += 1; }   // the radio: channel at +off, DC spike
                m.mix(x.data(), x.size());
                rx.feed(x.data(), x.size());
            }
            DabTelemetry tel; rx.telemetry(tel, 0);
            return tel;
        };
        const DabTelemetry a = runDab(false), b = runDab(true);
        printf("DAB: centre fibOk %llu services %d snr %.1f | offset fibOk %llu services %d snr %.1f\n", (unsigned long long)a.fibOk, a.services, a.snrDb,
               (unsigned long long)b.fibOk, b.services, b.snrDb);
        CHECK(b.state == 2 && b.services == a.services && b.fibBad == 0 && b.fibOk + 10 >= a.fibOk, "DAB with offset tuning decodes worse than on the centre");
    }
    {   // FM the same way: audio S/N with offset tuning is not below the station on the centre without a spike
        auto runFm = [](bool offset) {
            const OffsetPlan p = planOffset(250e3, 2.4e6, 20e6);
            const double rate = p.rateHz, off = offset ? p.offsetHz : 0;
            FmGenConfig cfg; cfg.rate = rate; cfg.cnrDb = 40;
            FmGenerator gen(cfg); FmReceiver rx; rx.setSilent(true); rx.configure(rate);
            std::vector<float> l; rx.setAudioTap([&](const float* a, const float*, size_t n) { l.insert(l.end(), a, a + n); });
            OffsetMixer m; m.set(off, rate);
            std::vector<cf32> buf; double t = 0;
            for (size_t done = 0; done < (size_t)(4 * rate); done += 65536) {
                buf.clear(); gen.generate(65536, buf);
                if (offset) for (auto& v : buf) { v *= std::polar(1.0f, (float)std::fmod(2 * M_PI * off * t / rate, 2 * M_PI)); v += cf32(0.05f, -0.03f); t += 1; }
                m.mix(buf.data(), buf.size());
                rx.feed(buf.data(), buf.size());
            }
            const size_t n = std::min(l.size(), (size_t)96000);
            double re = 0, im = 0, tot = 0;
            for (size_t i = 0; i < n; i++) { const double v = l[l.size() - n + i], ph = 2 * M_PI * 1000.0 * i / 48000.0; re += v * std::cos(ph); im += v * std::sin(ph); tot += v * v; }
            const double a = 2 * std::sqrt(re * re + im * im) / n, noise = std::sqrt(std::max(1e-12, tot / n - a * a / 2));
            return 20 * std::log10(a / std::sqrt(2.0) / noise);
        };
        const double a = runFm(false), b = runFm(true);
        printf("FM: audio S/N centre %.1f dB, offset tuning (with a DC spike) %.1f dB\n", a, b);
        CHECK(b > a - 1.0, "FM with offset tuning: %.1f dB against %.1f on the centre", b, a);
    }
    {   // DAB at the radios' own rates: 2.5 Msps (Airspy R2) and 3.5 Msps (the offset plan) must decode as cleanly as 2.048 — the resampler
        // used to settle for a fraction 100 ppm off there, which cost 13 dB of SNR
        auto snrAt = [](double rate) {
            dabgen::TxConfig tc; tc.utcSeconds = 1700000000; SynthConfig sc; sc.snrDb = 25;
            auto syn = makeDabSynth(tc, sc, rate);
            DabReceiver rx; rx.configure(rate); rx.audio().setSilent(true); rx.select(0);
            std::vector<cf32> x(65536);
            for (size_t done = 0; done < (size_t)(4 * rate); done += x.size()) { syn->generate(x.data(), x.size()); rx.feed(x.data(), x.size()); }
            DabTelemetry tel; rx.telemetry(tel, 0);
            return tel.snrDb;
        };
        const double s0 = snrAt(2.048e6), s25 = snrAt(2.5e6), s30 = snrAt(3.0e6);
        printf("DAB SNR: 2.048 Msps %.1f, 2.5 Msps %.1f, 3.0 Msps %.1f dB\n", s0, s25, s30);
        CHECK(s25 > s0 - 2 && s30 > s0 - 2, "DAB loses SNR at 2.5/3.0 Msps (%.1f / %.1f against %.1f)", s25, s30, s0);
    }
    printf(fails ? "offset tune: FAILED\n" : "offset tune: ok\n");
    return fails ? 1 : 0;
}
