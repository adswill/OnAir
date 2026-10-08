// Inmarsat Aero demodulators: Eb/N0 sweep per rate (Eb/N0 per channel bit, the rate the carrier is keyed at).
// The demodulator runs from the carrier search on (lines of the squared signal) through Viterbi and the SU checks.
// Asserted: at 5 dB (600 / 1200 bit/s, differential MSK) and 3 dB (10500 bit/s, coherent OQPSK) at least 99 % of the SUs are good.
// For reference, a K = 7 rate 1/2 code with soft decisions needs about 4.4 dB per information bit for a bit error rate of 1e-5 on coherent
// BPSK, i.e. about 1.4 dB per channel bit; differential detection of MSK costs about 2 to 3 dB more.
#include "dect2/aero_demod.h"
#include "dect2/aero_phy.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
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

int main() {
    for (int rate : {600, 1200, 10500}) {
        const double secs = rate == 10500 ? 16 : rate == 1200 ? 30 : 60;
        const double fs = rate == 10500 ? 48000 : 9600;
        double lastGood = 99;
        for (double eb = 8; eb >= 0; eb -= 1) {
            const Result r = run(rate, fs, secs, eb, -170, 0, 0, 11 + (uint32_t)eb);
            const double good = (double)r.susOk / std::max(1, r.susOk + r.susBad);
            const double frac = (double)r.susOk / std::max(1, (r.sent - 2) * (aeroFrameFormat(rate)->infoBytes() / 12));
            printf("rate %5d  Eb/N0 %4.1f dB: frames %3d of %3d, exact %3d, SUs good %5d bad %3d (%.2f%% bad), %.0f%% of sent SUs good, estimate %.1f dB\n", rate, eb, r.frames,
                   r.sent - 2, r.exact, r.susOk, r.susBad, 100.0 * (1 - good), 100 * frac, r.ebn0);
            if (good >= 0.99 && r.frames >= 0.8 * (r.sent - 2)) lastGood = eb;
            if ((rate == 10500 && eb == 3) || (rate != 10500 && eb == 5))
                CHECK(good >= 0.99 && frac >= 0.85, "rate %d at %.0f dB: %.2f%% bad, %.0f%% received", rate, eb, 100 * (1 - good), 100 * frac);
            if (eb >= 6) CHECK(std::fabs(r.ebn0 - eb) < 1.0, "rate %d estimate %.1f at %.0f dB", rate, r.ebn0, eb);
        }
        printf("rate %5d: 99%% of the SUs good and 80%% of the frames down to %.0f dB\n", rate, lastGood);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
