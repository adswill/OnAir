// Channel estimate, header leakage removal, FFT and equaliser on the test signal (front end included), against the transmitted carriers.
#include "dect2/dtmb_demod.h"
#include "dect2/dtmb_front.h"
#include "dect2/dtmb_gen.h"
#include "dect2/dtmb_map.h"
#include <cmath>
#include <cstdio>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Run { double mer = 0, snrEst = 0; int siBest = -1; float siScore = 0, siMargin = 0; double tapErr = 0; };

// frames 3 .. 3+n-1 of a signal: equalised carriers against the transmitted ones
static Run run(const SignalConfig& sc, int nFrames, int pre, int post, double echoPos = 0) {
    Signal sig(sc, testPacketSource(1));
    FrameTx ref(sc.tx, testPacketSource(1));
    const int Lf = frameLength(sc.tx.header), Lh = headerInfo(sc.tx.header).length;
    const int total = nFrames + 6;
    const long base = ref.frameNumber();   // number of the first frame (the transmitter has warmed up)
    std::vector<std::vector<cf32>> carriers((size_t)total);
    std::vector<cf32> tmp((size_t)Lf);
    for (int f = 0; f < total; f++) { ref.nextFrame(tmp.data()); carriers[(size_t)f] = ref.lastCarriers(); }
    const size_t nIn = (size_t)((double)total * Lf * sc.rate / kSymbolRate) + 8000;
    std::vector<cf32> in(nIn), s;
    sig.generate(in.data(), nIn);
    SrrcResampler rs;
    rs.configure(sc.rate);
    rs.setDcRemoval(false);
    rs.process(in.data(), nIn, s);
    HeaderEstimator est(sc.tx.header);
    est.setWindow(pre, post);
    BodyEqualizer eq;
    Run out;
    double err = 0, pw = 0;
    std::vector<cf32> bins((size_t)kBody);
    std::vector<float> var((size_t)kBody);
    Taps ta, tb;
    for (int f = 3; f < 3 + nFrames; f++) {
        const long S = (long)f * Lf, S2 = (long)(f + 1) * Lf;
        const int phA = sc.tx.phaseRotate ? pnPhase(sc.tx.header, (int)((base + f) % headerInfo(sc.tx.header).framesPerSuper)) : 0;
        const int phB = sc.tx.phaseRotate ? pnPhase(sc.tx.header, (int)((base + f + 1) % headerInfo(sc.tx.header).framesPerSuper)) : 0;
        est.estimate(&s[(size_t)S], phA, ta);
        est.estimate(&s[(size_t)S2], phB, tb);
        HeaderEstimator::clean(ta, ta.tapVar, 12.f);
        HeaderEstimator::clean(tb, tb.tapVar, 12.f);
        int8_t ca[945], cb[945];
        pnHeader(sc.tx.header, phA, ca);
        pnHeader(sc.tx.header, phB, cb);
        eq.run(&s[(size_t)(S + Lh)], ta, ca, tb, cb, Lh, std::sqrt(headerInfo(sc.tx.header).powerRatio / 2.0), bins.data(), var.data());
        const auto& truth = carriers[(size_t)f];
        for (int k = 0; k < kBody; k++) { err += std::norm(bins[(size_t)k] - truth[(size_t)k]); pw += std::norm(truth[(size_t)k]); }
        out.snrEst += 10 * std::log10(1.0 / (0.5 * (ta.noise + tb.noise) * 1.0));
        if (f == 3) {
            // system information of this frame
            cf32 si[36]; float w[36]; cf32 dd[3744];
            splitBody(bins.data(), si, dd);
            const auto& pos = siPositions();
            for (int i = 0; i < 36; i++) w[i] = 1.f / std::max(var[(size_t)carrierMap()[(size_t)pos[(size_t)i]]], 1e-9f);
            float sc22[22];
            siScores(si, w, sc22);
            int best = 0, second = 1;
            if (sc22[1] > sc22[0]) std::swap(best, second);
            for (int i = 2; i < 22; i++) { if (sc22[i] > sc22[(size_t)best]) { second = best; best = i; } else if (sc22[i] > sc22[(size_t)second]) second = i; }
            out.siBest = best + 3; out.siScore = sc22[best]; out.siMargin = sc22[best] - sc22[second];
        }
    }
    out.mer = 10 * std::log10(pw / err);
    out.snrEst /= nFrames;
    (void)echoPos;
    return out;
}

int main() {
    struct Case { Header h; double snr; double rate; Mapping m; Rate r; bool mode2; int echoDb; double echoDelay; int pre, post; double minMer; };
    const Case cases[] = {
        {Header::Pn945, 200, 10e6, Mapping::Qam64, Rate::R06, false, 0, 0, 16, 64, 38},
        {Header::Pn420, 200, 10e6, Mapping::Qam16, Rate::R04, true, 0, 0, 16, 64, 38},
        {Header::Pn595, 200, 10e6, Mapping::Qam64, Rate::R08, false, 0, 0, 16, 64, 38},
        {Header::Pn945, 30, 10e6, Mapping::Qam64, Rate::R06, false, 0, 0, 16, 64, 27},
        {Header::Pn420, 25, 16e6, Mapping::Qam64, Rate::R06, false, 0, 0, 16, 64, 22},
        {Header::Pn945, 30, 10e6, Mapping::Qam64, Rate::R06, false, 6, 40, 16, 80, 26},
        {Header::Pn420, 30, 10e6, Mapping::Qam16, Rate::R06, true, 10, 55, 16, 80, 26},
        {Header::Pn945, 30, 12.5e6, Mapping::Qam64, Rate::R06, false, 3, 190, 16, 240, 22},
        {Header::Pn945, 30, 10e6, Mapping::Qam64, Rate::R06, false, 0, 0, 16, 64, 27},
        {Header::Pn595, 28, 10e6, Mapping::Qam64, Rate::R06, false, 6, 60, 16, 90, 24},
    };
    for (const Case& c : cases) {
        SignalConfig sc;
        sc.rate = c.rate; sc.snrDb = c.snr;
        sc.tx.header = c.h; sc.tx.profile.map = c.m; sc.tx.profile.rate = c.r; sc.tx.profile.mode2 = c.mode2;
        sc.tx.phaseRotate = c.h != Header::Pn595;
        if (c.echoDb) sc.echoes.push_back({(double)c.echoDb, c.echoDelay * c.rate / 10e6});
        const Run r = run(sc, 4, c.pre, c.post);
        Profile p; p.map = c.m; p.rate = c.r; p.mode2 = c.mode2;
        printf("  %s snr %5.0f rate %4.1f echo %d@%.0f: carrier MER %.1f dB, header SNR estimate %.1f dB, SI index %d (expected %d) score %.2f margin %.2f\n", headerInfo(c.h).name, c.snr, c.rate / 1e6, c.echoDb, c.echoDelay,
               r.mer, r.snrEst, r.siBest, siIndex(p), r.siScore, r.siMargin);
        CHECK(r.mer > c.minMer, "MER %.1f dB below %.1f", r.mer, c.minMer);
        CHECK(r.siBest == siIndex(p), "system information %d instead of %d", r.siBest, siIndex(p));
    }
    printf(failures ? "dtmb_demod: %d FAILED\n" : "dtmb_demod: all passed\n", failures);
    return failures ? 1 : 0;
}
