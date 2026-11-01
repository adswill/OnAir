// FY-3 AHRPT / MPT measurement tool: the sensitivity of the receiver against an ideal chain, and a sweep of real-world impairments.
//   fy3tool theory [--frames N]                   the ideal chain: perfect carrier, timing and sync, soft Viterbi per rail, differential decoder, Reed-Solomon
//   fy3tool rx [--rate R] [--rs S] [--product P] [--lo dB --hi dB --step dB] [--secs T] [--cfo Hz] [--threads N]
//                                                 the receiver on the test signal: share of frames decoded against Es/N0
//   fy3tool imp [--snr dB] [--rate R] [--rs S] [--product P]
//                                                 the receiver with one impairment at a time at a fixed Es/N0
// Es/N0 is the energy of a QPSK symbol over the noise density, the noise bandwidth being the symbol rate. docs/fy3.md has the numbers.
#include "dect2/fy3/fy3_gen.h"
#include "dect2/fy3/fy3_proto.h"
#include "dect2/fy3/fy3_rx.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;

namespace {

// --- the ideal chain -----------------------------------------------------------------------------------------------------------------------
double genieSuccess(double snrDb, int frames, uint32_t seed, double* berOut = nullptr, double jitterDeg = 0) {
    std::mt19937 rng(seed);
    dect2::normal_distribution<float> nd{0.f, 1.f};
    const double snr = std::pow(10.0, snrDb / 10), sigma = std::sqrt(1.0 / snr);
    fy3::DiffEncoder de;
    hrpt::CcsdsEncoder enc[2];
    fy3::RailViterbi vit[2];
    std::vector<uint8_t> sent;
    sent.reserve((size_t)frames * 1020);
    std::vector<uint8_t> dataAll;
    for (int f = 0; f < frames; f++) {
        fy3::Cadu c;
        c.scid = 51; c.vcid = 5; c.counter = (uint32_t)f; c.fhp = 0x7FF;
        for (int i = 0; i < 882; i++) c.data[i] = (uint8_t)rng();
        uint8_t cadu[1024];
        fy3::encodeVcdu(c, cadu);
        sent.insert(sent.end(), cadu + 4, cadu + 1024);
        dataAll.insert(dataAll.end(), c.data, c.data + 882);
        for (int i = 0; i < 1024 * 8; i += 2) {
            const int hi = cadu[i >> 3] >> (7 - (i & 7)) & 1, lo = cadu[(i + 1) >> 3] >> (7 - ((i + 1) & 7)) & 1;
            int x, y;
            de.push(hi, lo, x, y);
            uint8_t a[2], b[2];
            enc[0].push(x, a[0], b[0]);
            enc[1].push(y, a[1], b[1]);
            // the receiver side: two QPSK symbols, each rotated by a carrier phase error, noisy; G2's inversion undone by the transform
            float sa[2], sb[2];
            for (int k = 0; k < 2; k++) {
                const double ph = jitterDeg * M_PI / 180 * nd(rng);
                const double c = std::cos(ph), s = std::sin(ph);
                const double i1 = (a[0] ? 1.0 : -1.0), q1 = (a[1] ? 1.0 : -1.0), i2 = (b[0] ? 1.0 : -1.0), q2 = (b[1] ? 1.0 : -1.0);
                if (k == 0) { sa[0] = (float)(i1 * c - q1 * s + sigma * nd(rng)); sa[1] = (float)(i1 * s + q1 * c + sigma * nd(rng)); }
                else { sb[0] = (float)(i2 * c - q2 * s + sigma * nd(rng)); sb[1] = (float)(i2 * s + q2 * c + sigma * nd(rng)); }
            }
            for (int r = 0; r < 2; r++) {
                int oa, ob;
                fy3::pairXform(4, (int)std::lround(sa[r] * 40), (int)std::lround(sb[r] * 40), oa, ob);
                vit[r].push(oa, ob);
            }
        }
    }
    vit[0].flush(); vit[1].flush();
    const auto& o0 = vit[0].out();
    const auto& o1 = vit[1].out();
    const size_t n = std::min(o0.size(), o1.size());
    fy3::DiffDecoder dd;
    std::vector<uint8_t> out;
    out.reserve(2 * n);
    for (size_t m = 0; m < n; m++) { int hi, lo; dd.push(o0[m], o1[m], hi, lo); out.push_back((uint8_t)hi); out.push_back((uint8_t)lo); }
    // frames: each CADU is 8192 bits, the first 32 are the sync word
    int ok = 0;
    size_t bitErr = 0, bitTot = 0;
    for (int f = 0; f < frames; f++) {
        if ((size_t)(f + 1) * 8192 > out.size()) break;
        uint8_t coded[1020];
        std::memset(coded, 0, sizeof coded);
        for (int i = 0; i < 8160; i++) coded[i >> 3] = (uint8_t)(coded[i >> 3] << 1 | out[(size_t)f * 8192 + 32 + (size_t)i]);
        for (int i = 0; i < 1020; i++) { bitErr += (size_t)__builtin_popcount(coded[i] ^ sent[(size_t)f * 1020 + (size_t)i]); }
        bitTot += 8160;
        fy3::Cadu c;
        if (fy3::decodeCadu(coded, c) && std::memcmp(c.data, &dataAll[(size_t)f * 882], 882) == 0) ok++;
    }
    if (berOut) *berOut = bitTot ? (double)bitErr / (double)bitTot : 0;
    return (double)ok / frames;
}

// --- the receiver --------------------------------------------------------------------------------------------------------------------------
struct Imp {
    double snrAdd = 0;
    double cfo = 0, drift = 0, ppm = 0, iqGainDb = 0, iqPhaseDeg = 0, dc = 0, fadeDb = 0, fadeHz = 0, pnHz = 0, phase = 0;
    bool conj = false, swapIq = false;
};

struct RxResult { double success = 0; uint64_t ok = 0, bad = 0; bool lockedAtEnd = false; double snrEst = 0; double cfoErr = 0; };

RxResult rxRun(double snrDb, double rate, double rs, int product, double secs, const Imp& im, uint32_t seed, int sat = 2, int preset = 0) {
    if (getenv("FY3_SEED")) seed = (uint32_t)atoi(getenv("FY3_SEED"));
    Fy3GenConfig g;
    g.rate = rate; g.product = product; g.symbolRate = rs; g.snrDb = 99; g.cfoHz = im.cfo; g.clockPpm = im.ppm; g.driftHzPerS = im.drift;
    g.conjugate = im.conj; g.swapIq = im.swapIq; g.sat = sat; g.phaseDeg = im.phase; g.duty = 0.5; g.seed = seed;
    Fy3Generator gen(g);
    Fy3Receiver rx(product);
    rx.configure(rate);
    if (getenv("FY3_LOG")) rx.setLogCallback([](const std::string& s) { fprintf(stderr, "  log: %s\n", s.c_str()); });
    rx.setPreset(preset);
    if (product == 1 && rs > 0) rx.setSymbolRate(rs);
    std::mt19937 rng(seed * 7 + 1);
    dect2::normal_distribution<float> nd{0.f, 1.f};
    // signal power: amp^2 (the generator's), Es/N0 over the symbol rate
    const double amp = g.amp, snr = std::pow(10.0, snrDb / 10);
    const double n0 = amp * amp / (snr * (rs > 0 ? rs : g.symbolRate));
    const double sigma = std::sqrt(n0 * rate / 2);
    std::vector<cf32> buf(65536);
    Fy3Telemetry t, t0;
    uint64_t seq = 0;
    double ph = 0;
    const double g1 = std::pow(10.0, im.iqGainDb / 20), eps = im.iqPhaseDeg * M_PI / 180;
    int64_t n = 0;
    bool got0 = false;
    const double tStart = std::min(3.0, secs * 0.5);
    dect2::normal_distribution<float> pn{0.f, 1.f};
    const double pnSig = im.pnHz > 0 ? std::sqrt(2 * M_PI * im.pnHz / rate) : 0;
    for (double done = 0; done < secs * rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        for (auto& v : buf) {
            std::complex<double> s(v.real(), v.imag());
            if (pnSig > 0) { ph += pnSig * pn(rng); s *= std::polar(1.0, ph); }
            if (im.fadeDb > 0) {   // slow fading: the level swings between 0 and -fadeDb
                const double m = 0.5 * (1 + std::sin(2 * M_PI * im.fadeHz * (double)n / rate));
                s *= std::pow(10.0, -im.fadeDb * m / 20);
            }
            if (im.iqGainDb != 0 || im.iqPhaseDeg != 0) {   // gain / phase imbalance: Q path scaled and rotated
                const double I = s.real(), Q = s.imag();
                s = std::complex<double>(I, g1 * (Q * std::cos(eps) + I * std::sin(eps)));
            }
            s += std::complex<double>(im.dc * amp, im.dc * amp);
            s += std::complex<double>(sigma * nd(rng), sigma * nd(rng));
            v = cf32((float)s.real(), (float)s.imag());
            n++;
        }
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) {
            if (getenv("FY3_TRACE")) fprintf(stderr, "t %.2f state %d ok %llu bad %llu lock %.2f cfo %.0f snr %.1f rail %d frame %d asm %llu\n", t.timeSec, t.state, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.carrierLock, t.cfoHz, t.snrDb, (int)t.railLock, (int)t.frameLock, (unsigned long long)t.asmHits);
            seq = t.seq; if (!got0 && t.timeSec >= tStart) { t0 = t; got0 = true; } }
    }
    RxResult r;
    r.ok = t.blocksOk - (got0 ? t0.blocksOk : 0); r.bad = t.blocksBad - (got0 ? t0.blocksBad : 0);
    const double span = t.timeSec - (got0 ? t0.timeSec : 0);
    const double expect = span * (rs > 0 ? rs : g.symbolRate) / 8192.0;
    r.success = expect > 0 ? std::min(1.0, (double)r.ok / expect) : 0;
    r.lockedAtEnd = t.state >= 3;
    r.snrEst = t.snrDb;
    r.cfoErr = t.cfoHz - im.cfo;
    return r;
}

template <class F> void parallel(int n, int threads, F f) {
    std::atomic<int> next{0};
    std::vector<std::thread> th;
    for (int i = 0; i < threads; i++) th.emplace_back([&] { for (int k; (k = next++) < n;) f(k); });
    for (auto& t : th) t.join();
}

double argd(int argc, char** argv, const char* name, double def) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return atof(argv[i + 1]);
    return def;
}

// the Es/N0 at which the share crosses a level, by linear interpolation of the grid
double crossing(const std::vector<double>& snr, const std::vector<double>& p, double level) {
    for (size_t i = 1; i < snr.size(); i++)
        if (p[i - 1] < level && p[i] >= level) return snr[i - 1] + (snr[i] - snr[i - 1]) * (level - p[i - 1]) / std::max(1e-9, p[i] - p[i - 1]);
    return p.empty() || p[0] >= level ? snr.front() : NAN;
}

} // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    const int threads = (int)argd(argc, argv, "--threads", 3);
    if (cmd == "theory") {
        const int frames = (int)argd(argc, argv, "--frames", 300);
        const double lo = argd(argc, argv, "--lo", -1.0), hi = argd(argc, argv, "--hi", 4.0), step = argd(argc, argv, "--step", 0.25);
        std::vector<double> snr, p, ber;
        for (double s = lo; s <= hi + 1e-9; s += step) snr.push_back(s);
        p.resize(snr.size()); ber.resize(snr.size());
        const double jit = argd(argc, argv, "--jitter", 0);
        parallel((int)snr.size(), threads, [&](int i) { p[(size_t)i] = genieSuccess(snr[(size_t)i], frames, 100 + (uint32_t)i, &ber[(size_t)i], jit); });
        printf("ideal chain (%d frames a point): Es/N0 dB, share of frames decoded, bit error rate before Reed-Solomon\n", frames);
        for (size_t i = 0; i < snr.size(); i++) printf("  %5.2f  %5.3f  %.2e\n", snr[i], p[i], ber[i]);
        printf("threshold (50%%) %.2f dB, (90%%) %.2f dB, (99%%) %.2f dB\n", crossing(snr, p, 0.5), crossing(snr, p, 0.9), crossing(snr, p, 0.99));
        return 0;
    }
    const double rate = argd(argc, argv, "--rate", 8e6), rs = argd(argc, argv, "--rs", 2.6e6);
    const int product = (int)argd(argc, argv, "--product", 0);
    const double secs = argd(argc, argv, "--secs", 8.0);
    if (cmd == "rx") {
        const double lo = argd(argc, argv, "--lo", 0.0), hi = argd(argc, argv, "--hi", 5.0), step = argd(argc, argv, "--step", 0.5);
        Imp im; im.cfo = argd(argc, argv, "--cfo", 0);
        im.drift = argd(argc, argv, "--drift", 0); im.ppm = argd(argc, argv, "--ppm", 0); im.pnHz = argd(argc, argv, "--pn", 0);
        std::vector<double> snr, p;
        for (double s = lo; s <= hi + 1e-9; s += step) snr.push_back(s);
        p.resize(snr.size());
        std::vector<RxResult> res(snr.size());
        parallel((int)snr.size(), threads, [&](int i) { res[(size_t)i] = rxRun(snr[(size_t)i], rate, rs, product, secs, im, 20 + (uint32_t)i, 2, product ? 0 : 0); p[(size_t)i] = res[(size_t)i].success; });
        printf("receiver %s, %.2f Msps, %.3f Msym/s, cfo %.0f Hz, drift %.0f Hz/s, %.0f ppm: Es/N0 dB, share of frames decoded\n", product ? "MPT" : "AHRPT", rate / 1e6, rs / 1e6, im.cfo, im.drift, im.ppm);
        for (size_t i = 0; i < snr.size(); i++) printf("  %5.2f  %5.3f  (%llu ok, %llu bad)\n", snr[i], p[i], (unsigned long long)res[i].ok, (unsigned long long)res[i].bad);
        printf("threshold (50%%) %.2f dB, (90%%) %.2f dB, (99%%) %.2f dB\n", crossing(snr, p, 0.5), crossing(snr, p, 0.9), crossing(snr, p, 0.99));
        return 0;
    }
    if (cmd == "imp") {
        const double snr = argd(argc, argv, "--snr", 6.0);
        struct Sc { const char* name; Imp im; double rate; };
        std::vector<Sc> sc;
        auto add = [&](const char* n, Imp im, double r = 0) { sc.push_back({n, im, r > 0 ? r : rate}); };
        add("clean", Imp{});
        { Imp i; i.cfo = 40000; add("offset +40 kHz", i); }
        { Imp i; i.cfo = -40000; add("offset -40 kHz", i); }
        { Imp i; i.cfo = 60000; add("offset +60 kHz", i); }
        { Imp i; i.cfo = -12345; i.drift = 100; add("Doppler ramp +100 Hz/s", i); }
        { Imp i; i.cfo = 12345; i.drift = -100; add("Doppler ramp -100 Hz/s", i); }
        { Imp i; i.cfo = 30000; i.drift = 500; add("Doppler ramp +500 Hz/s (5x the worst L-band)", i); }
        { Imp i; i.ppm = 50; add("clock +50 ppm", i); }
        { Imp i; i.ppm = -50; add("clock -50 ppm", i); }
        { Imp i; i.ppm = 300; add("clock +300 ppm", i); }
        { Imp i; i.iqGainDb = 1.0; i.iqPhaseDeg = 5; add("IQ imbalance 1 dB / 5 deg", i); }
        { Imp i; i.iqGainDb = 2.0; i.iqPhaseDeg = 10; add("IQ imbalance 2 dB / 10 deg", i); }
        { Imp i; i.dc = 0.1; add("DC offset 10% of the signal", i); }
        { Imp i; i.dc = 0.5; add("DC offset 50% of the signal", i); }
        { Imp i; i.pnHz = 10; add("phase noise, 10 Hz linewidth (a poor LNB)", i); }
        { Imp i; i.pnHz = 30; add("phase noise, 30 Hz linewidth", i); }
        { Imp i; i.fadeDb = 3; i.fadeHz = 0.5; i.snrAdd = 3; add("fading 3 dB at 0.5 Hz (dips at the given Es/N0)", i); }
        { Imp i; i.fadeDb = 6; i.fadeHz = 5; i.snrAdd = 6; add("fading 6 dB at 5 Hz (dips at the given Es/N0)", i); }
        { Imp i; i.conj = true; add("spectrum inverted (Q negated)", i); }
        { Imp i; i.swapIq = true; add("I and Q exchanged", i); }
        { Imp i; i.conj = true; i.swapIq = true; i.cfo = 25000; i.phase = 77; add("both, +25 kHz", i); }
        { Imp i; i.cfo = 1.8e6; add("off-centre +1.8 MHz at 8 Msps (the whole span)", i); }
        { Imp i; i.cfo = -1.8e6; add("off-centre -1.8 MHz at 8 Msps", i); }
        { Imp i; i.cfo = 6.0e6; add("off-centre +6 MHz at 20 Msps", i, 20e6); }
        { Imp i; i.cfo = -7.5e6; i.drift = 100; add("off-centre -7.5 MHz at 20 Msps, Doppler ramp", i, 20e6); }
        for (double r : {4e6, 5e6, 6e6, 8e6, 10e6, 12.5e6, 16e6, 20e6}) { Imp i; i.cfo = 15000; char* nm = new char[40]; snprintf(nm, 40, "sample rate %.1f Msps", r / 1e6); add(nm, i, r); }
        std::vector<RxResult> res(sc.size());
        parallel((int)sc.size(), threads, [&](int i) { res[(size_t)i] = rxRun(snr + sc[(size_t)i].im.snrAdd, sc[(size_t)i].rate, rs, product, secs, sc[(size_t)i].im, 50 + (uint32_t)i); });
        printf("impairments at Es/N0 %.1f dB, %s, %.3f Msym/s: share of frames decoded\n", snr, product ? "MPT" : "AHRPT", rs / 1e6);
        for (size_t i = 0; i < sc.size(); i++) printf("  %-48s %5.3f  (%llu ok %llu bad, carrier error %+.0f Hz)\n", sc[i].name, res[i].success, (unsigned long long)res[i].ok, (unsigned long long)res[i].bad, res[i].cfoErr);
        return 0;
    }
    fprintf(stderr, "usage: fy3tool theory|rx|imp [options], see the top of tools/fy3tool.cpp\n");
    return 2;
}
