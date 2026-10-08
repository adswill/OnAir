// Iridium radio layer: known answers (unique words, DQPSK mapping, sync word rule) and single bursts through the detector and the
// demodulator at the rates the radios offer.
#include "dect2/iridium_frame.h"
#include "dect2/iridium_gen.h"
#include "dect2/iridium_phy.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
using namespace dect2;
using namespace dect2::iridium;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> randomBits(std::mt19937& r, int n) {
    std::vector<uint8_t> b(n);
    for (auto& v : b) v = (uint8_t)(r() & 1);
    return b;
}

// one burst in noise; returns bit errors (-1: not detected, -2: no unique word)
static int oneBurst(double rate, double freq, double esn0Db, int preamble, bool dl, uint32_t seed, double* fErr = nullptr, double* tErr = nullptr, double dopRate = 0) {
    std::mt19937 r(seed);
    IridiumTestBurst b;
    b.bits = randomBits(r, dl ? 358 : 300);
    b.downlink = dl;
    b.preamble = preamble;
    b.startSec = 0.0031 + (r() % 1000) * 1e-7;
    b.freqHz = freq;
    b.dopplerRate = dopRate;
    b.phase = (r() % 628) / 100.0;
    const double sigma = 0.05;
    b.amplitude = (float)(sigma * std::sqrt(std::pow(10, esn0Db / 10) * kSymbolRate * 2 / rate));
    const size_t n = (size_t)(0.03 * rate);
    std::vector<cf32> x(n, cf32(0, 0));
    iridiumRenderBursts({b}, rate, 0, x.data(), n);
    std::normal_distribution<float> nd(0, (float)sigma);
    for (auto& v : x) v += cf32(nd(r), nd(r));
    BurstDetector det;
    det.configure(rate);
    std::vector<DetectedBurst> found;
    det.feed(x.data(), n, found);
    const DetectedBurst* hit = nullptr;
    for (const auto& f : found)
        if (std::fabs(f.freqHz - freq) < 20e3 && (!hit || f.peakDb > hit->peakDb)) hit = &f;
    if (!hit) return -1;
    BurstDemod dm;
    dm.configure(rate);
    DemodResult d;
    const uint64_t e = std::min<uint64_t>(hit->end, n);
    dm.demod(&x[hit->start], e - hit->start, hit->freqHz, preamble == 64 ? kMaxSymbolsSimplex : kMaxSymbolsNormal, d);
    if (std::getenv("IRDEBUG")) printf("  dbg rate %.1f f %.0f det %.0f peak %.1f found %d uw %d dl %d snr %.1f conf %.0f nsym %d fe %.1f\n", rate / 1e6, freq, hit->freqHz, hit->peakDb, d.found, d.uwOk, d.downlink, d.snrDb, d.confidence, d.nSymbols, d.freqHz - freq);
    if (!d.uwOk || d.downlink != dl) return -2;
    if (fErr) *fErr = d.freqHz - freq;
    if (tErr) *tErr = (hit->start / rate + d.uwTime) - (b.startSec + preamble / kSymbolRate);
    int err = 0;
    const size_t nb = std::min(d.bits.size(), b.bits.size());
    for (size_t i = 0; i < nb; i++) err += d.bits[i] != b.bits[i];
    err += (int)(b.bits.size() - nb);
    return err;
}

int main() {
    // unique words: the symbols of gr-iridium iridium.h after differential decoding are the bit strings of iridium-toolkit bitsparser.py
    {
        std::vector<uint8_t> bits;
        symbolsToBits(kUwDl, kUwLen, 0, bits);
        std::string s; for (auto v : bits) s += char('0' + v);
        CHECK(s == kUwDlBits && s == "001100000011000011110011", "downlink unique word %s", s.c_str());
        symbolsToBits(kUwUl, kUwLen, 0, bits);
        s.clear(); for (auto v : bits) s += char('0' + v);
        CHECK(s == kUwUlBits && s == "110011000011110011111100", "uplink unique word %s", s.c_str());
    }
    // DQPSK as decode_deqpsk: steps 0,1,2,3 quarter turns -> 00, 10, 11, 01
    {
        const int pairs[4] = {0, 2, 3, 1};
        for (int st = 0; st < 4; st++) {
            CHECK(dqpskPair(st) == pairs[st], "step %d -> %d", st, dqpskPair(st));
            CHECK(dqpskStep(pairs[st] >> 1, pairs[st] & 1) == st, "pair %d -> step", pairs[st]);
        }
        std::mt19937 r(5);
        const std::vector<uint8_t> b = randomBits(r, 400);
        const std::vector<uint8_t> sym = burstSymbols(b, true, 16);
        CHECK(sym.size() == 16 + 12 + 200, "burst symbols %zu", sym.size());
        bool pre = true;
        for (int i = 0; i < 16; i++) pre &= sym[i] == 0;
        CHECK(pre && std::memcmp(&sym[16], kUwDl, 12) == 0, "preamble / unique word symbols");
        std::vector<uint8_t> back;
        symbolsToBits(&sym[28], sym.size() - 28, sym[27], back);
        CHECK(back == b, "bits -> symbols -> bits");
        const std::vector<uint8_t> ul = burstSymbols(b, false, 16);
        CHECK(ul[14] == 2 && ul[15] == 0 && std::memcmp(&ul[16], kUwUl, 12) == 0, "uplink preamble ends 2, 0 (burst_downmix generate_sync_word)");
        // the toolkit's RAW line of this burst = the unique word bits followed by these bits
        std::vector<uint8_t> all;
        symbolsToBits(&sym[16], sym.size() - 16, 0, all);
        std::string s; for (int i = 0; i < 24; i++) s += char('0' + all[i]);
        CHECK(s == kUwDlBits && std::equal(b.begin(), b.end(), all.begin() + 24), "RAW bit string");
    }
    // the frame layer's own mapping (iridium_frame.h) agrees with this one
    {
        std::vector<uint8_t> steps = {0, 1, 2, 3, 3, 2, 1, 0}, bits, mine, back;
        iridiumStepsToBits(steps, bits);
        for (uint8_t st : steps) { const int p = dqpskPair(st); mine.push_back((uint8_t)(p >> 1)); mine.push_back((uint8_t)(p & 1)); }
        CHECK(bits == mine, "step mapping differs from the frame layer");
        iridiumBitsToSteps(bits, back);
        CHECK(back == steps, "bits to steps");
        std::vector<uint8_t> uw;
        symbolsToBits(kUwDl, kUwLen, 0, uw);
        CHECK(uw == iridiumUniqueWordBits(true), "unique word bits");
    }
    // the sync word rule of check_sync_word: 90 degrees counts 1, 180 counts 2, at most 2 accepted
    {
        uint8_t s[12];
        std::memcpy(s, kUwDl, 12);
        CHECK(uwDistance(s, true) == 0 && uwDistance(s, false) > 2, "exact unique word");
        s[3] = (s[3] + 1) & 3; s[7] = (s[7] + 3) & 3;
        CHECK(uwDistance(s, true) == 2, "two 90 degree errors: %d", uwDistance(s, true));
        s[9] = (s[9] + 2) & 3;
        CHECK(uwDistance(s, true) == 4, "plus one 180 degree error: %d", uwDistance(s, true));
    }
    // channel grid (util.py): access 1 of sub-band 1 at 1616.020833 MHz, simplex access 7 at 1626.270833 MHz
    CHECK(std::fabs(channelHz(0) - 1616020833.3) < 1 && std::fabs(channelHz(246) - 1626270833.3) < 1, "channel grid");
    CHECK(nearestChannel(1626.27e6 + 15e3) == 246 && nearestChannel(1616.0e6 + 100) == 0, "nearest channel");

    // single bursts, clean, at the radio rates, with odd offsets
    const double rates[] = {2e6, 2.4e6, 4e6, 8e6, 10e6, 20e6, 3.2e6};
    for (double rate : rates) {
        for (int pre : {16, 64})
            for (bool dl : {true, false}) {
                const double f = (dl ? 0.31 : -0.27) * rate + 1234.5;
                double fe = 0, te = 0;
                const int e = oneBurst(rate, f, 25, pre, dl, 11 + (int)(rate / 1e5) + pre, &fe, &te);
                CHECK(e == 0 && std::fabs(fe) < 30 && std::fabs(te) < 2e-6, "%.1f Msps preamble %d %s: errors %d, freq error %.1f Hz, time error %.2f us",
                      rate / 1e6, pre, dl ? "DL" : "UL", e, fe, te * 1e6);
            }
    }
    // carrier offsets across a detector bin, Doppler rate 350 Hz/s
    for (int i = 0; i < 20; i++) {
        const double f = 1.6e6 + i * 517.3;
        double fe = 0;
        const int e = oneBurst(10e6, f, 20, 16, true, 100 + i, &fe, nullptr, i % 2 ? 350 : -350);
        CHECK(e == 0 && std::fabs(fe) < 60, "offset %.1f: errors %d, freq error %.1f", f, e, fe);
    }
    // weaker bursts: bit errors at Es/N0 12 dB (QPSK, differential: about 1e-4 per bit expected)
    {
        int tot = 0, bits = 0, miss = 0;
        for (int i = 0; i < 40; i++) {
            const int e = oneBurst(10e6, -2.2e6 + i * 41666.7, 12, 16, true, 300 + i);
            if (e < 0) miss++; else { tot += e; bits += 358; }
        }
        for (double sn : {60.0, 30.0, 20.0, 15.0, 12.0, 9.0}) { const int e = oneBurst(10e6, 1.1e6, sn, 16, true, 77); printf("  Es/N0 %.0f: %d errors\n", sn, e); }
        printf("Es/N0 12 dB, 40 bursts at 10 Msps: %d missed, %d bit errors in %d bits\n", miss, tot, bits);
        CHECK(miss <= 1 && tot <= bits * 2e-3, "12 dB: %d missed, %d errors", miss, tot);
    }
    printf(fails ? "iridium phy: %d FAILED\n" : "iridium phy: all passed\n", fails);
    return fails ? 1 : 0;
}
