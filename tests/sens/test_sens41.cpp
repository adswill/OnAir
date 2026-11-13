// 4.1 sensitivity (docs/SENSITIVITY-4.1.md): the gaps 4.0 left open. Each section measures the new path against the 4.0 one in the same run (same seeds, same
// signals: the receivers' own switches turn the 4.1 change off), prints the curve, and fails when the new path is not better at its guard point or noise alone
// decodes anything. Simulated signals only. ONAIR_SENS41=<names> picks sections (default all), ONAIR_SENS41_TRIALS sets the trials a point.
#include "sens_modes_d.h"
#include "dect2/iridium/iridium_frame.h"
#include "dect2/iridium/iridium_rx.h"
#include "dect2/general/general_rx.h"
#include "dect2/general/general_tel.h"
#include "dect2/wxsat/wxsat_gen.h"
#include "dect2/wxsat/wxsat_proto.h"
#include "dect2/wxsat/wxsat_rx.h"
#include "dect2/wxsat/wxsat_tel.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
using namespace sens;

namespace {
int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

bool want(const char* name) {
    const char* e = getenv("ONAIR_SENS41");
    if (!e || !*e) return true;
    return strstr(e, name) != nullptr;
}
int trialsDefault(int n) { const char* e = getenv("ONAIR_SENS41_TRIALS"); return e && atoi(e) > 0 ? atoi(e) : n; }

struct Case { Mode m; std::shared_ptr<const Sig> base; double power = 0; size_t trialN = 0, maxOff = 0; double ref = 1; };   // the signal is shared by the 4.0 and 4.1 cases

double trial(const Case& C, double snrDb, double cfoHz, double sroPpm, bool noiseOnly, uint64_t seed) {
    std::mt19937 rng((uint32_t)(seed ^ (seed >> 32)));
    const size_t off = C.maxOff ? (size_t)(mix64(seed ^ 0x51ed) % C.maxOff) : 0;
    const Sig& B = *C.base;
    Sig x(B.begin() + (long)off, B.begin() + (long)std::min(B.size(), off + C.trialN + 64));
    if (sroPpm != 0) x = clockError(x, sroPpm);
    if (x.size() > C.trialN) x.resize(C.trialN);
    if (noiseOnly) std::fill(x.begin(), x.end(), cf32(0, 0));
    std::uniform_real_distribution<double> U(0, 2 * M_PI);
    shift(x, cfoHz, C.m.rate, U(rng));
    addNoise(x, C.power * (C.m.rate / C.m.bwHz) / std::pow(10.0, snrDb / 10), rng);
    auto s = C.m.make(C.m.rate);
    for (size_t o = 0; o < x.size(); o += 65536) s->feed(x.data() + o, std::min<size_t>(65536, x.size() - o));
    s->finish();
    return s->poll().good;
}

Mode findMode(const char* id) {
    auto all = allModes();
    for (auto& m : extraModesD()) all.push_back(std::move(m));
    for (const auto& m : all) if (m.id == id) return m;
    printf("no mode %s\n", id);
    return Mode{};
}
void prepare(Case& C) {
    C.trialN = (size_t)(C.m.secs * C.m.rate);
    C.maxOff = (size_t)(0.5 * C.m.secs * C.m.rate);
    auto b = std::make_shared<Sig>(C.m.gen(C.trialN + C.maxOff + 64));
    scaleTo(*b, 0.2);
    C.power = activePower(*b);
    C.base = b;
    C.ref = std::max(1.0, trial(C, 60, 0, 0, false, 77));
}
// the same signal through another receiver set-up
Case withMake(const Case& c, Factory f) {
    Case C = c;
    C.m.make = std::move(f);
    C.ref = std::max(1.0, trial(C, 60, 0, 0, false, 77));
    return C;
}
double frac(const Case& C, double snr, double cfo, double sro, int n, uint64_t salt, double ref = 0) {
    double s = 0;
    if (ref <= 0) ref = C.ref;
    for (int i = 0; i < n; i++) s += std::min(1.0, trial(C, snr, cfo, sro, false, mix64(salt * 1000 + (uint64_t)((snr + 50) * 10) + (uint64_t)i * 7919)) / ref);
    return s / n;
}
double falses(const Case& C, int n, uint64_t salt) {
    double f = 0;
    for (int i = 0; i < n; i++) f += trial(C, 10, 0, 0, true, mix64(salt + (uint64_t)i));
    return f;
}
// one curve: the 4.0 path (`oldC`) and the 4.1 path (`newC`) at every point, both printed; returns the sum of (new - old) over the points
// (both measured against the 4.0 path's clean count, so a path that decodes more of a clean trial does not lower its own share)
double curve(const char* name, const Case& oldC, const Case& newIn, const std::vector<double>& snrs, double cfo, double sro, int n, uint64_t salt) {
    const Case& newC = newIn;
    double gain = 0;
    printf("%-14s carrier %+6.0f Hz clock %3.0f ppm (clean units %.0f / %.0f):", name, cfo, sro, oldC.ref, newIn.ref);
    for (double snr : snrs) {
        const double a = frac(oldC, snr, cfo, sro, n, salt), b = frac(newC, snr, cfo, sro, n, salt, oldC.ref);
        printf("  %g dB %.0f->%.0f %%", snr, 100 * a, 100 * b);
        gain += b - a;
    }
    printf("\n");
    return gain;
}

// ------------------------------------------------------------------------------------------- 1. iMet-54 (and MRZ): soft symbols
Factory sondeMake(bool soft) {
    return factory<SondeReceiver, SondeTelemetry>([soft](SondeReceiver& rx, double r) {
        rx.setSynchronous(true); rx.setSignalOffset(0); rx.configure(r); rx.setCenterMhz(403); rx.setSoftDecisions(soft); },
        blocks<SondeTelemetry>, false, [](SondeReceiver& rx) { rx.flush(); });
}
Case sondeCase(int mask, double secs, bool soft) {
    Case C;
    C.m = findMode("sonde");
    C.m.secs = secs;
    C.m.gen = [mask](size_t n) { SynthConfig c; c.modeOpt[0] = mask; c.modeOpt[1] = 1; return clean(15, c, 8e6, n); };
    C.m.make = sondeMake(soft);
    prepare(C);
    return C;
}
void sectionSonde() {
    const int n = trialsDefault(4);
    {   // RS41 for the comparison (no change)
        const Case r = sondeCase(1, 12.0, true);
        printf("RS41 reference:");
        for (double snr : {9.0, 7.0, 5.0, 3.0}) printf("  %g dB %.0f %%", snr, 100 * frac(r, snr, 0, 0, n, 0x41a1));
        printf("\n");
    }
    {
        const Case io = sondeCase(128, 12.0, false), in = withMake(io, sondeMake(true));
        CHECK(in.ref >= 3, "iMet-54 clean reference %.1f", in.ref);
        double g = curve("iMet-54", io, in, {13, 11, 9, 7, 5}, 0, 0, n, 0x41a1);
        g += curve("iMet-54", io, in, {11, 9, 7}, 8000, 0, n, 0x41a2);
        g += curve("iMet-54", io, in, {11, 9, 7}, 0, 50, n, 0x41a3);
        printf("iMet-54 sum of gains: %+.2f\n", g);
        CHECK(g >= -0.15, "iMet-54 soft path worse: %+.2f", g);
        const double f = falses(in, 4, 0x41f0);
        printf("iMet-54 noise-only units: %.0f\n", f);
        CHECK(f == 0, "iMet-54 noise-only %.0f", f);
    }
    {
        const Case mo = sondeCase(256, 24.0, false), mn = withMake(mo, sondeMake(true));
        CHECK(mn.ref >= 3, "MRZ clean reference %.1f", mn.ref);
        curve("MRZ", mo, mn, {10, 8, 6, 4, 2}, 0, 0, n, 0x41b1);
        curve("MRZ", mo, mn, {8, 6, 4}, 8000, 0, n, 0x41b2);
        curve("MRZ", mo, mn, {8, 6, 4}, 0, 50, n, 0x41b3);
        const double gm = frac(mn, 8, 0, 0, n, 0x41b4);
        printf("MRZ guard 8 dB: %.0f %%\n", 100 * gm);
        CHECK(gm >= 0.85, "MRZ 8 dB soft %.2f", gm);
        const double f = falses(mn, 4, 0x41f1);
        printf("MRZ noise-only units: %.0f\n", f);
        CHECK(f == 0, "MRZ noise-only %.0f", f);
    }
}

// ------------------------------------------------------------------------------------------- 2a. Iridium: soft repair of the BCH blocks
// Frame level: the bits of ring alerts, broadcasts and pager frames through a soft channel (value +-1 plus Gaussian noise; hard bit = sign,
// reliability = |value|), decoded with and without the reliabilities; and random bits with random reliabilities (false frames).
void iridiumFrames() {
    std::mt19937 g(4141);
    std::vector<std::vector<uint8_t>> frames;
    frames.push_back(iridiumBuildIra(17, 22, 48.1, 11.6, 790, {0x12345678u, 0x0badf00du}));
    frames.push_back(iridiumBuildIbc(17, 22, 1.7e9));
    frames.push_back(iridiumBuildMsg(4242, 7, 0, 1, "HELLO 4.1 SOFT BCH"));
    const char* names[3] = {"IRA", "IBC", "MSG"};
    for (double sigma : {0.45, 0.5, 0.55}) {
        printf("Iridium frames, sigma %.2f:", sigma);
        for (size_t fi = 0; fi < frames.size(); fi++) {
            int okH = 0, okS = 0, n = 300;
            for (int t = 0; t < n; t++) {
                IridiumBurstBits b;
                std::normal_distribution<double> N(0, sigma);
                for (uint8_t x : frames[fi]) { const double v = (x ? 1.0 : -1.0) + N(g); b.bits.push_back(v > 0); b.rel.push_back((float)std::fabs(v)); }
                IridiumBurstBits h = b; h.rel.clear();
                if (decodeIridiumBurst(h).ok) okH++;
                if (decodeIridiumBurst(b).ok) okS++;
            }
            printf("  %s %d->%d of %d", names[fi], okH, okS, n);
            if (sigma == 0.5) CHECK(okS > okH, "Iridium %s soft %d hard %d", names[fi], okS, okH);
        }
        printf("\n");
    }
    int falseH = 0, falseS = 0;
    const int nf = 20000;
    for (int t = 0; t < nf; t++) {
        IridiumBurstBits b;
        const size_t len = 128 + 64 * (size_t)(g() % 12);
        std::exponential_distribution<double> E(1.0);
        for (size_t i = 0; i < len; i++) { b.bits.push_back((uint8_t)(g() & 1)); b.rel.push_back((float)E(g)); }
        // half of them start like a pager frame (the header is not protected), so that the pager path is tried too
        // (the header is compared in the decoder's order: both bits of each symbol swapped)
        if (t & 1) { const char* hm = "00110011111100110011001111110011"; for (int i = 0; i < 32; i++) b.bits[(size_t)(i ^ 1)] = (uint8_t)(hm[i] == '1'); }
        IridiumBurstBits h = b; h.rel.clear();
        const IridiumFrame fh = decodeIridiumBurst(h), fs = decodeIridiumBurst(b);
        if (fh.ok && fh.type != IridiumType::Voice) falseH++;
        if (fs.ok && fs.type != IridiumType::Voice) falseS++;
    }
    printf("Iridium random bursts: %d hard, %d soft frames of %d\n", falseH, falseS, nf);
    CHECK(falseS <= falseH + nf / 4000, "Iridium soft false frames %d (hard %d)", falseS, falseH);
}
Factory iridiumMake(bool soft) {
    return factory<IridiumReceiver, IridiumTelemetry>([soft](IridiumReceiver& rx, double r) {
        rx.setSignalOffset(0); rx.setOffline(true); rx.configure(r); rx.setCenterMhz(1622); rx.setSoftDecisions(soft); },
        blocks<IridiumTelemetry>, false, [](IridiumReceiver& rx) { rx.flush(); });
}
void sectionIridium() {
    iridiumFrames();
    const int n = trialsDefault(4);
    Case o; o.m = findMode("iridium"); o.m.make = iridiumMake(false); prepare(o);
    const Case s = withMake(o, iridiumMake(true));
    curve("Iridium", o, s, {14, 12, 10, 8, 6}, 0, 0, n, 0x41c1);
    curve("Iridium", o, s, {12, 10, 8}, 0, 50, n, 0x41c2);
    const double f = falses(s, 2, 0x41cf);
    printf("Iridium noise-only units: %.0f\n", f);
    CHECK(f == 0, "Iridium noise-only %.0f", f);
}

// ------------------------------------------------------------------------------------------- 2b. Zigbee: second-choice symbols on an FCS failure
Factory zigbeeMake(bool soft) {
    return factory<ZigbeeReceiver, ZigbeeTelemetry>([soft](ZigbeeReceiver& rx, double r) { prepGeneric(rx, r, 67); rx.setSoftDecisions(soft); }, blocks<ZigbeeTelemetry>);
}
void sectionZigbee() {
    const int n = trialsDefault(4);
    Case o; o.m = findMode("zigbee"); o.m.make = zigbeeMake(false); prepare(o);
    const Case s = withMake(o, zigbeeMake(true));
    curve("Zigbee", o, s, {3, 1.5, 0, -1.5}, 0, 0, n, 0x41d1);
    curve("Zigbee", o, s, {1.5, 0}, 48000, 0, n, 0x41d2);
    curve("Zigbee", o, s, {1.5, 0}, 0, 50, n, 0x41d3);
    const double f = falses(s, 6, 0x41df);
    printf("Zigbee noise-only units: %.0f\n", f);
    CHECK(f == 0, "Zigbee noise-only %.0f", f);
}

// ------------------------------------------------------------------------------------------- 4. Aero: acquisition on short trials
Factory aeroMake(bool fast) {
    const double off = modeTuning(20) ? modeTuning(20)->tuneOffsetHz : 0;
    return factory<AeroReceiver, AeroTelemetry>([off, fast](AeroReceiver& rx, double r) { rx.setSignalOffset(-off); rx.configure(r); rx.setFastAcquisition(fast); }, blocks<AeroTelemetry>);
}
void sectionAero() {
    const int n = trialsDefault(4);
    for (double secs : {3.0, 6.0}) {
        Case o; o.m = findMode("aero"); o.m.secs = secs; o.m.make = aeroMake(false); prepare(o);
        const Case s = withMake(o, aeroMake(true));
        char name[32]; snprintf(name, sizeof name, "Aero %.0f s", secs);
        curve(name, o, s, {6, 3, 0, -2}, 0, 0, n, 0x41e1 + (uint64_t)secs);
        curve(name, o, s, {3, 0}, 0, 50, n, 0x41e5 + (uint64_t)secs);
        const double f = falses(s, 2, 0x41ef);
        printf("%s noise-only units: %.0f\n", name, f);
        CHECK(f == 0, "%s noise-only %.0f", name, f);
    }
}

// ------------------------------------------------------------------------------------------- 5. Globalstar: clock error at other sample rates
// QA-4.0: the generator at a rate other than 6 / 10 Msps, through a 4-point Lagrange resampler with a few ppm of clock error (what the QA sweep does,
// and what a radio's own fractional resampler or anti-alias filter does to the noise near the band edges): 0 events. Events counted with the 4.0
// floor cap and the 4.1 one; the channel floors printed.
Sig lagrange(const Sig& x, double ppm) {
    Sig y;
    const double step = 1.0 + ppm * 1e-6;
    for (double pos = 1; pos + 2 < (double)x.size(); pos += step) {
        const size_t i0 = (size_t)pos;
        const float f = (float)(pos - (double)i0);
        const float w0 = -f * (f - 1) * (f - 2) / 6, w1 = (f + 1) * (f - 1) * (f - 2) / 2, w2 = -(f + 1) * f * (f - 2) / 2, w3 = (f + 1) * f * (f - 1) / 6;
        y.push_back(x[i0 - 1] * w0 + x[i0] * w1 + x[i0 + 1] * w2 + x[i0 + 2] * w3);
    }
    return y;
}
void sectionGlobalstar() {
    for (double rate : {10e6, 13.7e6, 8e6, 12e6, 20e6}) {
        const double secs = rate > 12e6 ? 3.0 : 5.0;
        const Sig clean0 = clean(64, SynthConfig(), rate, (size_t)(secs * rate) + 64);
        for (double ppm : {0.0, 5.0, 50.0}) {
            Sig x = ppm != 0 ? lagrange(clean0, ppm) : clean0;
            scaleTo(x, 0.2);
            const double pw = activePower(x);
            std::mt19937 rng(4164 + (uint32_t)ppm);
            addNoise(x, pw * (rate / 1.23e6) / std::pow(10.0, 6.0 / 10), rng);   // 6 dB in a channel
            uint64_t ev[2] = {0, 0};
            std::string fl[2];
            for (int k = 0; k < 2; k++) {
                GlobalstarReceiver rx;
                rx.configure(rate); rx.setSignalOffset(0); rx.setLocalFloorCap(k == 1);
                for (size_t o = 0; o < x.size(); o += 65536) rx.feed(x.data() + o, std::min<size_t>(65536, x.size() - o));
                GlobalstarTelemetry t;
                rx.telemetry(t, 0);
                ev[k] = t.blocksOk;
                for (const auto& c : t.chan) if (c.inView) { char b[48]; snprintf(b, sizeof b, " %d:%.1f/%.1f%s", c.index, c.powerDb, c.noiseDb, c.active ? "*" : ""); fl[k] += b; }
            }
            printf("Globalstar %.1f Msps, Lagrange %.0f ppm: events %llu -> %llu\n   4.0 floors%s\n   4.1 floors%s\n", rate / 1e6, ppm,
                   (unsigned long long)ev[0], (unsigned long long)ev[1], fl[0].c_str(), fl[1].c_str());
            if (rate == 10e6 && ppm == 0) CHECK(ev[1] >= ev[0], "Globalstar 10 Msps lost events %llu -> %llu", (unsigned long long)ev[0], (unsigned long long)ev[1]);
        }
    }
    // noise only, also with the edge roll-off of the resampler
    for (double rate : {10e6, 13.7e6}) {
        Sig x((size_t)(3 * rate), cf32(0, 0));
        std::mt19937 rng(77);
        addNoise(x, 1e-3, rng);
        x = lagrange(x, 5);
        GlobalstarReceiver rx;
        rx.configure(rate); rx.setSignalOffset(0);
        for (size_t o = 0; o < x.size(); o += 65536) rx.feed(x.data() + o, std::min<size_t>(65536, x.size() - o));
        GlobalstarTelemetry t;
        rx.telemetry(t, 0);
        printf("Globalstar noise only %.1f Msps: %llu events, %d active\n", rate / 1e6, (unsigned long long)t.blocksOk, t.activeNow);
        CHECK(t.blocksOk == 0 && t.activeNow == 0, "Globalstar noise-only %.1f Msps: %llu events", rate / 1e6, (unsigned long long)t.blocksOk);
    }
}

// ------------------------------------------------------------------------------------------- 6. FM threshold extension: NFM voice (PLL) and NOAA APT (FM feedback)
// NFM: a 1 kHz tone at 2.5 kHz deviation, carrier amplitude 1, white noise for the SNR in 12.5 kHz, 240 kS/s, the general receiver (NFM, 12.5 kHz,
// squelch off); SINAD of the last 2 s of audio: the tone (found within +-2 Hz) against everything else.
double sinadDb(const std::vector<float>& a, double fs) {
    if (a.size() < (size_t)(2.5 * fs)) return -99;
    const size_t n = (size_t)(2 * fs), o = a.size() - n;
    double mean = 0;
    for (size_t i = 0; i < n; i++) mean += a[o + i];
    mean /= (double)n;
    double tot = 0;
    for (size_t i = 0; i < n; i++) { const double v = a[o + i] - mean; tot += v * v; }
    tot /= (double)n;
    double best = 0;
    for (double f = 998; f <= 1002.001; f += 0.1) {
        double c = 0, sn = 0;
        for (size_t i = 0; i < n; i++) { const double w = 2 * M_PI * f * (double)i / fs, v = a[o + i] - mean; c += v * std::cos(w); sn += v * std::sin(w); }
        best = std::max(best, 2 * (c * c + sn * sn) / ((double)n * (double)n));
    }
    return 10 * std::log10(tot / std::max(1e-30, tot - best));
}
double nfmSinad(double snr, int pll, uint64_t seed) {
    const double rate = 240e3, secs = 3.5, dev = 2500;
    std::mt19937 rng((uint32_t)seed);
    std::normal_distribution<double> N(0, std::sqrt((rate / 12500.0) / std::pow(10.0, snr / 10) / 2));
    GeneralReceiver r;
    r.configure(rate);
    r.setSignalOffset(0);
    r.setSilent(true);
    r.setDemod(GenNfm);
    r.setWidthHz(12500);
    r.setSquelch(false, false, -100);
    r.setFmPll(pll);
    std::vector<float> aud;
    r.setAudioTap([&](const float* x, size_t n) { aud.insert(aud.end(), x, x + n); });
    std::vector<cf32> buf(24000);
    double ph = 0, t = 0;
    for (double done = 0; done < secs * rate; done += (double)buf.size()) {
        for (auto& v : buf) { ph += 2 * M_PI * dev * std::sin(2 * M_PI * 1000 * t) / rate; t += 1 / rate; v = cf32((float)(std::cos(ph) + N(rng)), (float)(std::sin(ph) + N(rng))); }
        r.feed(buf.data(), buf.size());
        GeneralTelemetry g;
        r.telemetry(g, 0);
    }
    return sinadDb(aud, kGeneralAudioRate);
}
// APT: as wxsat_apt40 (the receiver's test signal, 2.5 kHz offset, 30 Hz/s Doppler rate, 80 ppm clock), PSNR of image A and B
double aptPsnr(double snr, bool fb) {
    WxsatGenConfig g; g.profile = kWxApt; g.rate = 960e3; g.snrDb = snr; g.cfoHz = 2500; g.driftHzPerS = 30; g.clockPpm = 80; g.seed = 11;
    WxsatGenerator gen(g);
    WxsatReceiver rx(kWxApt);
    rx.configure(g.rate);
    rx.setAptFmFeedback(fb);
    std::vector<cf32> buf(1 << 16);
    WxsatTelemetry t;
    uint64_t seq = 0;
    for (double done = 0; done < 16 * g.rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    rx.flush();
    if (rx.telemetry(t, seq)) seq = t.seq;
    const WxImage* im = t.image.get();
    if (!im) return 0;
    std::vector<const WxLine*> ls;
    for (const auto& l : im->line) if (l && (int)l->px.size() >= 2080) ls.push_back(l.get());
    if (ls.size() < 4) return 0;
    ls.erase(ls.begin());
    auto ref = [](int seg, int x, int y) { return wxsat::wxTestPixel(seg, x, y) / 1023.0; };
    int best = 0; double bestE = 1e300;
    for (int off = 0; off < 64; off++) {
        double e = 0;
        for (size_t k = 0; k < std::min<size_t>(ls.size(), 6); k++)
            for (int x = 0; x < 909; x += 9) { const double d = ls[k]->px[(size_t)(86 + x)] / 255.0 - ref(0, x, ls[k]->idx + off); e += d * d; }
        if (e < bestE) { bestE = e; best = off; }
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0, n = 0;
    for (const WxLine* l : ls) for (int seg = 0; seg < 2; seg++) for (int x = 0; x < 909; x++) {
        const double r = ref(seg, x, l->idx + best), y = l->px[(size_t)((seg ? 1126 : 86) + x)];
        sx += r; sy += y; sxx += r * r; sxy += r * y; syy += y * y; n++;
    }
    const double a = (n * sxy - sx * sy) / (n * sxx - sx * sx), b = (sy - a * sx) / n;
    const double res = (syy - 2 * a * sxy - 2 * b * sy + a * a * sxx + 2 * a * b * sx + n * b * b) / n;
    return 10 * std::log10(255.0 * 255.0 / std::max(1e-9, res));
}
void sectionFm() {
    printf("NFM SINAD (dB) discriminator / PLL / auto:");
    double hi[3] = {0, 0, 0};
    for (double snr : {16.0, 12.0, 10.0, 8.0, 6.0, 4.0, 2.0}) {
        double v[3];
        for (int m = 0; m < 3; m++) v[m] = nfmSinad(snr, m, 4100 + (uint64_t)snr);
        printf("  %g dB %.1f / %.1f / %.1f", snr, v[0], v[1], v[2]);
        if (snr == 16.0) for (int m = 0; m < 3; m++) hi[m] = v[m];
    }
    printf("\n");
    CHECK(hi[0] > 15 && hi[2] >= hi[0] - 0.5, "NFM 16 dB SINAD %.1f, auto %.1f", hi[0], hi[2]);
    printf("APT PSNR (dB) discriminator / FM feedback:");
    for (double snr : {15.0, 10.0, 7.0, 5.0, 2.0, 0.0}) printf("  %g dB %.2f / %.2f", snr, aptPsnr(snr, false), aptPsnr(snr, true));
    printf("\n");
}
} // namespace

int main() {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (want("sonde")) sectionSonde();
    if (want("iridium")) sectionIridium();
    if (want("zigbee")) sectionZigbee();
    if (want("aero")) sectionAero();
    if (want("globalstar")) sectionGlobalstar();
    if (want("fm")) sectionFm();
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
