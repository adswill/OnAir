// Analog TV, sound: the FM carrier at +5.5 / +6.0 / +6.5 / +4.5 MHz from the vision carrier, discriminator, de-emphasis, 48 kHz out.
// The generator sends a 1 kHz tone at half of the nominal deviation (through the pre-emphasis of the system), so the receiver must give back a tone
// of half scale at 1 kHz. Sound that is not there must not appear and sound that stops must stop.
#include "dect2/atv_testkit.h"
#include <cmath>
#include <cstdio>

using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static double rms(const std::vector<float>& x, size_t a, size_t b) {
    b = std::min(b, x.size());
    if (b <= a) return 0;
    double s = 0;
    for (size_t i = a; i < b; i++) s += (double)x[i] * x[i];
    return std::sqrt(s / (double)(b - a));
}

int main() {
    // ---- a continuous 1 kHz tone, in each system
    struct S { int sys, col; const char* name; double spacing; };
    const S systems[] = {{kAtvG, kAtvPal, "B/G", 5.5}, {kAtvB, kAtvPal, "B", 5.5}, {kAtvI, kAtvPal, "I", 6.0}, {kAtvDK, kAtvPal, "D/K", 6.5}, {kAtvM, kAtvNtsc, "M", 4.5}, {kAtvN, kAtvPal, "N", 4.5}};
    for (const S& s : systems) {
        AtvGenConfig c; c.sys = s.sys; c.colour = s.col; c.sound = 4; c.cnrDb = 40; c.rate = 10e6;
        const Run r = run(c, 2.5);
        const size_t n = r.audio.size();
        AtvFormat f; atvMakeFormat(s.sys, s.col, f);
        CHECK(n > 48000 * 2, "%s: %zu samples of sound", s.name, n);
        if (n < 48000 * 2) continue;
        const size_t a = 48000, b = 48000 + 24000;
        const double hz = peakTone(r.audio, a, b);
        const double amp = toneAmp(r.audio, 1000, a, b);
        double e = 0;
        for (size_t i = a; i < b; i++) e += (double)r.audio[i] * r.audio[i];
        e /= (double)(b - a);
        const double snr = 10 * std::log10((amp * amp / 2) / std::max(1e-12, e - amp * amp / 2));
        printf("%s: tone %.1f Hz amplitude %.3f (sent 0.5), tone to the rest %.1f dB, carrier %.0f Hz, deviation %.1f kHz (nominal %.0f), level %.1f dB\n", s.name, hz, amp, snr,
               r.tel.soundHz - r.tel.visionHz, r.tel.soundDevKhz, f.soundDevKhz, r.tel.soundLevelDb);
        CHECK(near(hz, 1000, 2), "%s: tone at %.1f Hz", s.name, hz);
        CHECK(near(amp, 0.5, 0.04), "%s: tone amplitude %.3f (sent 0.5)", s.name, amp);
        CHECK(snr > 38, "%s: tone to the rest %.1f dB", s.name, snr);
        CHECK(r.tel.soundPresent && near(r.tel.soundSpacingMhz, s.spacing, 1e-6), "%s: sound present %d spacing %.4f", s.name, (int)r.tel.soundPresent, r.tel.soundSpacingMhz);
        CHECK(near(r.tel.soundHz - r.tel.visionHz, s.spacing * 1e6, 3000), "%s: sound carrier %.0f Hz above the vision carrier", s.name, r.tel.soundHz - r.tel.visionHz);
        CHECK(near(r.tel.soundDevKhz, 0.5 * f.soundDevKhz * std::sqrt(1 + std::pow(2 * M_PI * 1000 * f.preEmphUs * 1e-6, 2)), 0.1 * f.soundDevKhz), "%s: deviation %.1f kHz", s.name, r.tel.soundDevKhz);
        CHECK(near(r.tel.soundLevelDb, -9.0, 1.5), "%s: sound level %.1f dB (a tone of half scale is -9.0)", s.name, r.tel.soundLevelDb);
    }
    // ---- the melody: three different notes, each at its frequency and at the same level (the de-emphasis undoes the pre-emphasis at every pitch)
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.sound = 1; c.cnrDb = 40; c.rate = 10e6;
        const Run r = run(c, 3.4);
        struct N { double t0, t1, hz; };
        // notes of 0.35 s: 261.63 twice, 392.00 twice, 440.00 twice, 392.00, a rest, 349.23 twice, ... (the windows keep clear of the changes)
        const N notes[] = {{0.10, 0.60, 261.63}, {0.80, 1.30, 392.00}, {1.50, 2.00, 440.00}, {2.20, 2.40, 392.00}, {2.90, 3.10, 349.23}};
        for (const N& nn : notes) {
            const size_t a = (size_t)(nn.t0 * 48000), b = (size_t)(nn.t1 * 48000);
            const double hz = peakTone(r.audio, a, b, 150, 800);
            const double amp = toneAmp(r.audio, hz, a, b);
            printf("melody: %.2f s: %.1f Hz (note %.2f), amplitude %.3f\n", nn.t0, hz, nn.hz, amp);
            CHECK(near(hz, nn.hz, 2.5), "melody at %.2f s: %.1f Hz, expected %.2f", nn.t0, hz, nn.hz);
            CHECK(near(amp, 0.5, 0.05), "melody at %.2f s: amplitude %.3f", nn.t0, amp);
        }
    }
    // ---- beeps: the tone stops for 0.2 s in every 1.5 s, and the sound is quiet then
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.sound = 0; c.cnrDb = 40; c.rate = 10e6;
        const Run r = run(c, 3.2);
        const double on = rms(r.audio, (size_t)(0.3 * 48000), (size_t)(1.0 * 48000)), off = rms(r.audio, (size_t)(1.36 * 48000), (size_t)(1.46 * 48000));
        const double on2 = rms(r.audio, (size_t)(1.7 * 48000), (size_t)(2.5 * 48000)), off2 = rms(r.audio, (size_t)(2.86 * 48000), (size_t)(2.96 * 48000));
        printf("beeps: rms %.3f during the tone, %.4f in the gap (%.4f in the second gap)\n", on, off, off2);
        CHECK(near(on, 0.354, 0.04) && near(on2, 0.354, 0.04), "tone rms %.3f %.3f (half scale: 0.354)", on, on2);
        CHECK(off < 0.02 && off2 < 0.02, "gap rms %.4f %.4f", off, off2);
    }
    // ---- an unmodulated carrier gives silence, no carrier gives nothing and no sound flag
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.sound = 2; c.cnrDb = 40; c.rate = 10e6;
        const Run r = run(c, 2.0);
        CHECK(r.tel.soundPresent && rms(r.audio, 48000, 90000) < 0.01, "unmodulated carrier: present %d rms %.4f", (int)r.tel.soundPresent, rms(r.audio, 48000, 90000));
        c.sound = 3;
        const Run q = run(c, 2.0);
        double mx = 0;
        for (float v : q.audio) mx = std::max(mx, (double)std::fabs(v));
        CHECK(!q.tel.soundPresent && q.tel.soundHz == 0 && mx < 0.01, "no sound carrier: present %d carrier %.0f peak %.4f", (int)q.tel.soundPresent, q.tel.soundHz, mx);
        CHECK(q.tel.state == 2 && q.tel.system == "PAL B/G 625/50", "the picture does not need the sound: state %d '%s'", q.tel.state, q.tel.system.c_str());
    }
    // ---- carrier offset, sample rates and noise
    for (double cfo : {-120e3, 150e3}) {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.sound = 4; c.cnrDb = 40; c.rate = 10e6; c.cfoHz = cfo;
        const Run r = run(c, 2.5);
        const double amp = toneAmp(r.audio, 1000, 48000, 72000);
        CHECK(near(amp, 0.5, 0.04) && near(peakTone(r.audio, 48000, 72000), 1000, 2), "carrier offset %+.0f kHz: amplitude %.3f", cfo / 1e3, amp);
    }
    for (double rate : {8e6, 12.5e6, 16e6, 20e6}) {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.sound = 4; c.cnrDb = 40; c.rate = rate;
        const Run r = run(c, 2.5);
        const double amp = toneAmp(r.audio, 1000, 48000, 72000);
        CHECK(near(amp, 0.5, 0.04) && near(peakTone(r.audio, 48000, 72000), 1000, 2), "%.1f Msps: amplitude %.3f", rate / 1e6, amp);
    }
    for (double cnr : {30.0, 20.0, 17.0}) {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.sound = 4; c.cnrDb = cnr; c.rate = 10e6;
        const Run r = run(c, 2.5);
        const size_t a = 60000, b = 60000 + 24000;
        const double amp = toneAmp(r.audio, 1000, a, b);
        double e = 0;
        for (size_t i = a; i < b; i++) e += (double)r.audio[i] * r.audio[i];
        e /= (double)(b - a);
        const double snr = 10 * std::log10((amp * amp / 2) / std::max(1e-12, e - amp * amp / 2));
        printf("C/N %.0f dB: sound tone to the rest %.1f dB (the sound carrier is 13 dB below the vision carrier)\n", cnr, snr);
        CHECK(near(amp, 0.5, 0.06), "C/N %.0f: amplitude %.3f", cnr, amp);
        CHECK(snr > (cnr >= 30 ? 38 : cnr >= 20 ? 30 : 24), "C/N %.0f: tone to the rest %.1f dB", cnr, snr);
    }
    // ---- the controls: volume, mute and silent do not change what is decoded (the tap sees the same samples)
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.sound = 4; c.cnrDb = 40; c.rate = 10e6;
        Options o; o.onTime = [](AtvReceiver& rx, double secs) { rx.setVolume(secs < 1 ? 0.2f : 1.f); rx.setMuted(secs > 1.5); rx.setSilent(true); };
        const Run r = run(c, 2.5, o);
        CHECK(near(toneAmp(r.audio, 1000, 72000, 96000), 0.5, 0.04), "the tap is not affected by volume and mute: %.3f", toneAmp(r.audio, 1000, 72000, 96000));
    }
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
