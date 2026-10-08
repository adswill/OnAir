// Inmarsat Aero demodulators on one carrier: modulator -> carrier offset, clock offset, noise -> demodulator -> frames, bit-exact.
// Each rate at its own sample rate, chunks of 4096; a carrier 230 Hz off.
#include "dect2/aero_demod.h"
#include "dect2/aero_phy.h"
#include "dect2/gen_util.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Result { int frames = 0, exact = 0, sent = 0, susOk = 0, susBad = 0; double freq = 0; float ebn0 = 0; };

// secs of signal at fs; the carrier at cfo Hz (plus drift Hz/s), bit clock off by ppm
static Result run(int rate, double fs, double secs, double ebn0Db, double cfo, double drift, double ppm, uint32_t seed, size_t chunk = 4096) {
    std::mt19937 g(seed);
    AeroFrameEncoder enc(rate);
    AeroModulator mod(rate, fs, ppm);
    AeroDemod dem(rate, fs, 700);
    Result r;
    std::vector<std::vector<uint8_t>> sent;
    std::vector<uint8_t> fifoMatch;
    dem.setCallback([&](AeroFrameEvent& e) {
        r.frames++; r.susOk += e.susOk; r.susBad += e.susBad; r.freq = e.freqHz; r.ebn0 = e.ebn0Db;
        for (const auto& s : sent) if (s == e.bytes) { r.exact++; break; }
    });
    const AeroFrameFormat* f = aeroFrameFormat(rate);
    const double sigma = std::sqrt(fs / (2.0 * rate * std::pow(10.0, ebn0Db / 10)));
    genutil::NoiseSource noise(seed + 1);
    const size_t total = (size_t)(secs * fs);
    std::vector<cf32> buf(chunk);
    double ph = 0;
    int fno = 0;
    for (size_t done = 0; done < total;) {
        while (mod.queuedBits() < 2u * f->totalBits()) {
            std::vector<uint8_t> info(f->infoBytes());
            for (size_t k = 0; k + 12 <= info.size(); k += 12) { for (int i = 0; i < 10; i++) info[k + i] = (uint8_t)g(); aeroSuSetCrc(&info[k]); }
            sent.push_back(info);
            const auto bits = enc.frame(info.data(), aeroHeader(1, 0, fno >> 4, fno & 15));
            fno++;
            mod.pushBits(bits.data(), bits.size());
        }
        const size_t n = std::min(chunk, total - done);
        mod.generate(buf.data(), n);
        for (size_t i = 0; i < n; i++) {
            const double t = (double)(done + i) / fs;
            buf[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += 2 * M_PI * (cfo + drift * t) / fs;
            if (ph > M_PI) ph -= 2 * M_PI;
        }
        noise.add(buf.data(), n, (float)sigma);
        dem.feed(buf.data(), n);
        done += n;
    }
    r.sent = (int)sent.size();
    return r;
}


// MSK as the physics define it, not through AeroModulator: constant envelope, the phase moves +-90 degrees per bit
// (here bit 1 = -90, as JAERO's decoder reads it; the unique word search takes either sign). A demodulator whose bit rule
// differs from the real MSK law by a sign on every second bit finds nothing here.
static int runPhysicalMsk(int rate, double fs, double secs) {
    std::mt19937 g(5);
    AeroFrameEncoder enc(rate);
    AeroDemod dem(rate, fs, 700);
    int exact = 0;
    std::vector<std::vector<uint8_t>> sent;
    dem.setCallback([&](AeroFrameEvent& e) { for (const auto& s : sent) if (s == e.bytes) { exact++; break; } });
    const AeroFrameFormat* f = aeroFrameFormat(rate);
    std::vector<uint8_t> bits;
    int fno = 0;
    while ((double)bits.size() < secs * rate + 2.0 * f->totalBits()) {
        std::vector<uint8_t> info(f->infoBytes());
        for (size_t k = 0; k + 12 <= info.size(); k += 12) { for (int i = 0; i < 10; i++) info[k + i] = (uint8_t)g(); aeroSuSetCrc(&info[k]); }
        sent.push_back(info);
        const auto b = enc.frame(info.data(), aeroHeader(1, 0, fno >> 4, fno & 15));
        fno++;
        bits.insert(bits.end(), b.begin(), b.end());
    }
    const size_t total = (size_t)(secs * fs);
    genutil::NoiseSource noise(9);
    std::vector<cf32> buf(4096);
    double phase = 0.7;                                   // start phase: any
    size_t done = 0;
    const double cfo = 150, sigma = std::sqrt(fs / (2.0 * rate * std::pow(10.0, 2.0)));
    while (done < total) {
        const size_t n = std::min<size_t>(buf.size(), total - done);
        for (size_t i = 0; i < n; i++) {
            const double t = (double)(done + i) * rate / fs;           // time in bits
            const size_t k = (size_t)t;
            const double step = (bits[k] ? -1.0 : 1.0) * (M_PI / 2) * (t - (double)k);
            // phase at the start of bit k: sum of the earlier steps, kept as a running value
            static size_t lastK = 0; static double base = 0;
            if (done + i == 0) { lastK = 0; base = phase; }
            while (lastK < k) { base += (bits[lastK] ? -1.0 : 1.0) * (M_PI / 2); lastK++; }
            const double ph = base + step + 2 * M_PI * cfo * (double)(done + i) / fs;
            buf[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        noise.add(buf.data(), n, (float)sigma);
        dem.feed(buf.data(), n);
        done += n;
    }
    return exact;
}

int main() {
    for (int rate : {600, 1200, 10500}) {
        const double fs = rate == 10500 ? 48000 : rate == 1200 ? 9600 : 4900;
        const double secs = rate == 10500 ? 6 : rate == 1200 ? 14 : 26;
        const Result r = run(rate, fs, secs, 20, 230, 0, 0, 3);
        printf("rate %5d clean: frames %d of %d sent, exact %d, SUs %d/%d, carrier %.1f Hz, Eb/N0 est %.1f dB\n", rate, r.frames, r.sent, r.exact, r.susOk, r.susBad, r.freq, r.ebn0);
        CHECK(r.frames >= r.sent - 5 && r.exact == r.frames && r.susBad == 0, "rate %d clean round trip", rate);
        CHECK(std::fabs(r.freq - 230) < 3, "rate %d carrier estimate %.1f", rate, r.freq);
    }
    for (int rate : {600, 1200}) {
        const int ex = runPhysicalMsk(rate, rate == 1200 ? 9600 : 4900, rate == 1200 ? 14 : 26);
        printf("rate %d physical MSK: %d frames exact\n", rate, ex);
        CHECK(ex >= 4, "rate %d: physical MSK gave %d exact frames", rate, ex);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
