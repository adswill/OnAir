// Frame acquisition on the test signal: header mode, super-frame position, carrier offset, noise, echo, all three headers.
#include "dect2/dtmb_front.h"
#include "dect2/dtmb_gen.h"
#include "dect2/dtmb_sync.h"
#include <cmath>
#include <cstdio>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

// symbols of the test signal at the symbol rate, from a given start frame (the stream starts `skipSymbols` into the signal)
static std::vector<cf32> symbolsOf(const SignalConfig& sc, size_t skipSymbols, size_t n) {
    Signal sig(sc, testPacketSource(1));
    SrrcResampler rs;
    rs.configure(sc.rate);
    const size_t total = skipSymbols + n + 3000;
    std::vector<cf32> in((size_t)(total * sc.rate / kSymbolRate) + 4000), out;
    sig.generate(in.data(), in.size());
    rs.process(in.data(), in.size(), out);
    return std::vector<cf32>(out.begin() + (long)skipSymbols, out.begin() + (long)(skipSymbols + n));
}

int main() {
    Acquirer acq;
    struct Case { Header h; bool rotate; double snr, cfo, rate; int echoDb, echoDelay; size_t skip; double ppm = 0; };
    const Case cases[] = {
        {Header::Pn945, true, 30, 0, 10e6, 0, 0, 0},
        {Header::Pn945, true, 30, 0, 10e6, 0, 0, 123457},
        {Header::Pn420, true, 30, 0, 10e6, 0, 0, 98765},
        {Header::Pn595, false, 30, 0, 10e6, 0, 0, 55555},
        {Header::Pn945, false, 30, 0, 10e6, 0, 0, 77777},
        {Header::Pn420, false, 30, 0, 10e6, 0, 0, 31000},
        {Header::Pn420, true, 5, 0, 10e6, 0, 0, 40000},
        {Header::Pn945, true, 0, 0, 10e6, 0, 0, 40000},
        {Header::Pn595, false, 3, 0, 10e6, 0, 0, 40000},
        {Header::Pn945, true, 25, 3000, 10e6, 0, 0, 20000},
        {Header::Pn420, true, 25, -4000, 10e6, 0, 0, 20000},
        {Header::Pn595, false, 25, 2500, 10e6, 0, 0, 20000},
        {Header::Pn945, true, 25, 7000, 10e6, 0, 0, 33333},
        {Header::Pn420, true, 25, -9000, 10e6, 0, 0, 33333},
        {Header::Pn595, false, 25, 11000, 10e6, 0, 0, 33333},
        {Header::Pn945, true, 10, -12500, 10e6, 0, 0, 33333},
        {Header::Pn945, true, 8, 3990, 10e6, 0, 0, 14000},    // half way between two tried offsets
        {Header::Pn420, true, 8, -4010, 10e6, 0, 0, 14000},
        {Header::Pn595, false, 8, 12010, 10e6, 0, 0, 14000},
        {Header::Pn945, true, 15, 20000, 10e6, 0, 0, 14000},
        {Header::Pn420, true, 15, -27500, 10e6, 0, 0, 14000},
        // two paths of about the same strength (a single-frequency network) with a clock offset: the stronger of the two changes from frame to frame
        {Header::Pn420, true, 25, 0, 10e6, 0, 60, 41000, 40},
        {Header::Pn945, true, 25, 0, 10e6, 0, 150, 41000, -60},
        {Header::Pn595, false, 20, 0, 10e6, 2, 100, 41000, 40},
        {Header::Pn945, true, 25, 0, 8e6, 0, 0, 0},
        {Header::Pn420, true, 25, 0, 20e6, 6, 60, 12345},
        {Header::Pn945, true, 25, 0, 12.5e6, 3, 200, 22222},
    };
    for (const Case& c : cases) {
        SignalConfig sc;
        sc.rate = c.rate; sc.snrDb = c.snr; sc.cfoHz = c.cfo; sc.sroPpm = c.ppm;
        sc.tx.header = c.h; sc.tx.phaseRotate = c.rotate;
        sc.tx.profile.map = Mapping::Qam4; sc.tx.profile.rate = Rate::R04;
        if (c.echoDb || c.echoDelay) sc.echoes.push_back({(double)c.echoDb + (c.echoDb == 0 && c.echoDelay ? 0.3 : 0.0), c.echoDelay * c.rate / 10e6});
        auto r = symbolsOf(sc, c.skip, Acquirer::kBlock);
        // the receiver tries offsets 8 kHz apart: derotate by the nearest one, as it would, and add it back to the estimate
        const double hyp = 8000.0 * std::round(c.cfo / 8000.0);
        if (hyp != 0) for (size_t i = 0; i < r.size(); i++) { const double ph = -2.0 * 3.14159265358979 * hyp * (double)i / kSymbolRate; r[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
        AcqResult res;
        const bool ok = acq.run(r.data(), res);
        res.cfoHz += hyp;
        const HeaderInfo& hi = headerInfo(c.h);
        const int Lf = frameLength(c.h);
        // truth: frame f starts at symbol f * Lf; the block begins at symbol `skip`
        bool right = ok && res.header == c.h;
        const long firstNumber = FrameTx(sc.tx, testPacketSource(1)).frameNumber();   // the transmitter has run its interleaver warm-up
        long expectStart = 0; int expectFrame = 0;
        if (right) {
            // find the frame the result points at
            const long absStart = res.start + (long)c.skip;
            const long f = (absStart + Lf / 2) / Lf;
            expectStart = f * Lf - (long)c.skip;
            expectFrame = hi.cyclic() && c.rotate ? (int)((f + firstNumber) % hi.framesPerSuper) : 0;
            // an echo shifts the strongest correlation; allow it
            const long tol = (c.echoDb || c.echoDelay) ? c.echoDelay + 2 : 1;
            right = std::labs(res.start - expectStart) <= tol && res.frame == expectFrame && res.rotates == (hi.cyclic() && c.rotate);
        }
        printf("  %s %-7s snr %4.0f cfo %5.0f rate %5.1f echo %d: %s  start %ld (truth %ld) frame %d (truth %d) hits %d metric %.2f cfo estimate %.0f\n", hi.name, c.rotate ? "rotate" : "fixed", c.snr, c.cfo, c.rate / 1e6, c.echoDb,
               ok ? "found" : "NOT found", res.start, expectStart, res.frame, expectFrame, res.hits, res.metric, res.cfoHz);
        CHECK(right, "wrong acquisition");
        if (c.cfo != 0) CHECK(std::fabs(res.cfoHz - c.cfo) < 150, "cfo estimate %.0f vs %.0f", res.cfoHz, c.cfo);
    }
    {   // noise only: nothing found
        SignalConfig sc; sc.rate = 10e6; sc.snrDb = -60;
        const auto r = symbolsOf(sc, 1000, Acquirer::kBlock);
        AcqResult res;
        CHECK(!acq.run(r.data(), res), "found a signal in noise");
    }
    printf(failures ? "dtmb_sync: %d FAILED\n" : "dtmb_sync: all passed\n", failures);
    return failures ? 1 : 0;
}
