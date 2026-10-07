// Front end: transmitter model -> square-root raised cosine at the radio's rate -> matched filter and resampling -> symbols again.
#include "dect2/dtmb_front.h"
#include "dect2/dtmb_gen.h"
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    for (double rate : {8e6, 10e6, 12.5e6, 15.12e6, 16e6, 20e6}) {
        SignalConfig sc;
        sc.rate = rate;
        sc.snrDb = 200;
        sc.rms = 0.25f;
        sc.tx.header = Header::Pn945;
        sc.tx.profile.map = Mapping::Qam4;
        sc.tx.profile.rate = Rate::R04;
        Signal sig(sc, testPacketSource(1));
        // the same symbols, directly
        FrameTx ref(sc.tx, testPacketSource(1));
        const int L = ref.frameLength();
        const int frames = 12;
        std::vector<cf32> symbols((size_t)frames * (size_t)L);
        for (int f = 0; f < frames; f++) ref.nextFrame(&symbols[(size_t)f * (size_t)L]);
        const size_t nIn = (size_t)(frames * L * rate / kSymbolRate) - 4000;
        std::vector<cf32> in(nIn);
        sig.generate(in.data(), nIn);
        SrrcResampler rs;
        CHECK(rs.configure(rate), "configure");
        rs.setDcRemoval(false);
        std::vector<cf32> out;
        // odd chunk sizes
        size_t pos = 0, chunk = 7;
        const auto t0 = std::chrono::steady_clock::now();
        while (pos < nIn) { const size_t k = std::min(chunk, nIn - pos); rs.process(&in[pos], k, out); pos += k; chunk = chunk * 5 + 3; if (chunk > 70000) chunk = 1; }
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        // The pulse shaping filter and the matched filter leave a short, fixed intersymbol interference, which the receiver's channel
        // estimate absorbs. Fit the output as a 41 tap filter of the transmitted symbols (least squares) and look at what is left over.
        const int W = 20, K = 2 * W + 1;
        const size_t m0 = 3000, m1 = out.size() - 3000;
        std::vector<std::complex<double>> A((size_t)K * K), b((size_t)K);
        for (size_t m = m0; m < m1; m++) {
            for (int i = 0; i < K; i++) {
                const std::complex<double> xi = symbols[(size_t)((long)m - W + i)];
                b[(size_t)i] += std::conj(xi) * std::complex<double>(out[m]);
                for (int j = 0; j < K; j++) A[(size_t)i * K + (size_t)j] += std::conj(xi) * std::complex<double>(symbols[(size_t)((long)m - W + j)]);
            }
        }
        // Gauss-Jordan
        std::vector<std::complex<double>> c = b;
        for (int col = 0; col < K; col++) {
            int piv = col;
            for (int r = col + 1; r < K; r++) if (std::abs(A[(size_t)r * K + (size_t)col]) > std::abs(A[(size_t)piv * K + (size_t)col])) piv = r;
            for (int j = 0; j < K; j++) std::swap(A[(size_t)col * K + (size_t)j], A[(size_t)piv * K + (size_t)j]);
            std::swap(c[(size_t)col], c[(size_t)piv]);
            for (int r = 0; r < K; r++) {
                if (r == col) continue;
                const std::complex<double> f = A[(size_t)r * K + (size_t)col] / A[(size_t)col * K + (size_t)col];
                for (int j = col; j < K; j++) A[(size_t)r * K + (size_t)j] -= f * A[(size_t)col * K + (size_t)j];
                c[(size_t)r] -= f * c[(size_t)col];
            }
        }
        for (int i = 0; i < K; i++) c[(size_t)i] /= A[(size_t)i * K + (size_t)i];
        double e = 0, p0 = 0, isi = 0;
        for (size_t m = m0; m < m1; m++) {
            std::complex<double> y = 0;
            for (int i = 0; i < K; i++) y += c[(size_t)i] * std::complex<double>(symbols[(size_t)((long)m - W + i)]);
            e += std::norm(std::complex<double>(out[m]) - y); p0 += std::norm(std::complex<double>(out[m]));
        }
        for (int i = 0; i < K; i++) if (i != W) isi += std::norm(c[(size_t)i]);
        const double best = e / p0;
        const int bestLag = 0;
        printf("  main tap %.3f, ISI taps %.1f dB below it,", std::abs(c[(size_t)W]), 10 * std::log10(isi / std::norm(c[(size_t)W])));
        printf(" rate %6.2f Msps: %d taps, residual %.1f dB (lag %d), %.2f Msym/s output rate in %.0f ms\n", rate / 1e6, rs.taps(), 10 * std::log10(best), bestLag, out.size() / dt / 1e6, dt * 1e3);
        CHECK(10 * std::log10(best) < -45.0, "rate %g: after a 41 tap fit %.1f dB are left", rate, 10 * std::log10(best));
        CHECK(10 * std::log10(isi / std::norm(c[(size_t)W])) < -25.0, "ISI too large");
    }
    printf(failures ? "dtmb_front: %d FAILED\n" : "dtmb_front: all passed\n", failures);
    return failures ? 1 : 0;
}
