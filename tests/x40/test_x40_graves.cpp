// GRAVES carrier and meteor echoes: carrier offset and drift, echo Doppler and strength, noise; noise alone and a carrier alone give no echo.
#include "x40_gen.h"
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// Noise power per input sample so that a tone of amplitude amp stands snrBinDb above the mean noise in its FFT bin: the receiver filters to about 2.2 kHz
// (noise fraction 2202 / fs) and the Blackman-Harris window has (sum w)^2 / sum w^2 = 510.
static double noisePower(double fs, double amp, double snrBinDb) { return amp * amp * 510.0 / std::pow(10.0, snrBinDb / 10) * fs / 2202.0; }

int main() {
    const double fs = 250000;
    // echoes of different Doppler and strength; carrier off-centre and drifting; the carrier is much stronger than the echoes
    for (double carrier : {0.0, 150.0, -380.0}) for (double dop : {-700.0, -90.0, 60.0, 420.0}) for (double amp : {0.05, 0.02}) {
        std::vector<Echo> e = {{6.0, 1.5, dop, amp}, {14.0, 2.5, dop * 0.5 + 30, amp}};
        auto x = graves(fs, 22.0, carrier, 1.0, e);
        addNoise(x, noisePower(fs, 0.02, 20), 21);   // the weaker echoes stand 20 dB over the noise in a bin
        auto t = run(x40::Graves, x, fs);
        int got = countKind(t, "Meteor echo");
        CHECK(got == 2, "carrier %.0f dop %.0f amp %.2f: %d echoes", carrier, dop, amp, got);
        CHECK(std::fabs(t.cfoHz - carrier) < 4.0, "carrier %.1f measured %.1f", carrier, t.cfoHz);
        // the first echo's Doppler (rows are newest first)
        if (got == 2) { const double d = atof(t.rows.back().id.c_str()); CHECK(std::fabs(d - dop) < 12.0, "dop %.0f measured %.0f", dop, d); }
    }
    // sensitivity: count echoes found against echo strength over the noise (per 2.3 Hz bin), 10 echoes each
    for (double snr : {24.0, 20.0, 17.0, 14.0, 10.0}) {
        std::vector<Echo> e; for (int i = 0; i < 10; i++) e.push_back({5.0 + i * 5.0, 1.2, (i % 2 ? 1 : -1) * (100.0 + 30 * i), 0.02});
        auto x = graves(fs, 58.0, 40.0, 1.0, e); addNoise(x, noisePower(fs, 0.02, snr), 5);
        auto t = run(x40::Graves, x, fs);
        printf("echo %.0f dB over the noise in a bin: %d of 10 found, %d extra\n", snr, std::min(10, countKind(t, "Meteor echo")), std::max(0, countKind(t, "Meteor echo") - 10));
        if (snr >= 20) CHECK(countKind(t, "Meteor echo") == 10, "snr %.0f: %d of 10", snr, countKind(t, "Meteor echo"));
    }
    // false decodes: a carrier with noise only, 10 minutes of it
    {
        auto x = graves(fs, 600.0, 55.0, 1.0, {}); addNoise(x, noisePower(fs, 0.02, 14), 8);
        auto t = run(x40::Graves, x, fs); CHECK(countKind(t, "Meteor echo") == 0, "echoes from noise: %d", countKind(t, "Meteor echo"));
    }
    {
        std::vector<cf32> z((size_t)(fs * 120)); addNoise(z, 1.0, 4);
        auto t = run(x40::Graves, z, fs); CHECK(t.rows.empty(), "noise only: %zu rows", t.rows.size());
    }
    printf(fails ? "x40 graves: %d FAILED\n" : "x40 graves: ok\n", fails);
    return fails ? 1 : 0;
}
