// fldigi 8PSK125 / 8PSK250 (no FEC) on simulated signals: the phase map against psk.cxx (symbol k = 180 + 45 k degrees in fldigi's baseband,
// the opposite on the air), the MFSK varicode round trip, then the receiver over tuning errors, drift, a clock error, a noise sweep, two signals
// at once and noise alone. Simulated only: no recording of an on-air 8PSK signal was available.
#include "dect2/psk/psk_gen.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include "dect2/psk/psk_varicode.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 250000;
constexpr double kOff = -20000;

static PskTelemetry run(const std::vector<PskGenSignal>& sigs, double secs, int mode, double cfo = 0, uint32_t seed = 3) {
    PskGenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.cfoHz = cfo; c.signals = sigs; c.seed = seed;
    PskGenerator g(c);
    PskReceiver r;
    r.configure(kRate);
    r.setSignalOffset(kOff);
    r.setMode(mode);
    PskTelemetry res;
    std::vector<cf32> x(25000);
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.1) {
        g.generate(x.data(), x.size());
        r.feed(x.data(), x.size());
        PskTelemetry tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; res = std::move(tt); }
    }
    return res;
}

static PskGenSignal sig(double hz, const std::string& text, double snr, int mode) {
    PskGenSignal s;
    s.offsetHz = hz; s.text = text; s.snrDb = snr; s.mode = mode; s.repeat = false; s.idleSec = 1.0;
    return s;
}

static std::string textNear(const PskTelemetry& t, double hz, double tol = 25) {
    const PskSignal* b = nullptr;
    for (const auto& s : t.signals) if (std::fabs(s.offsetHz - hz) <= tol && (!b || s.chars > b->chars)) b = &s;
    return b ? b->text : std::string();
}

int main() {
    // 1. the phase map and the bit order
    CHECK(psk::psk8Eighths(0) == 4 && psk::psk8Eighths(4) == 0 && psk::psk8Eighths(1) == 3 && psk::psk8Eighths(7) == 5, "phase map");
    for (int k = 0; k < 8; k++) CHECK(psk::psk8Symbol(psk::psk8Eighths(k)) == k, "phase map back %d", k);
    CHECK(pskBaud(7) == 125 && pskBaud(8) == 250 && psk8psk(7) && psk8psk(8) && !psk8psk(6) && std::string(pskModeName(8)) == "8PSK250", "mode table");
    {   // the steps of a sending decode back to its text (MFSK varicode, 3 bits a symbol, least significant first)
        const std::string msg = "CQ DE LY2ABC 8PSK test, 73 <>?";
        const auto st = psk::psk8Steps(msg, 10, 5);
        CHECK(st.size() > 15 && st[0] == 4 && st.back() == 0, "preamble and postamble");
        psk::MfskVaricodeRx rx;
        std::string out;
        for (size_t i = 10; i + 5 < st.size(); i++) {   // up to the postamble (its steady symbols read as spaces)
            const int s = psk::psk8Symbol(st[i]);
            for (int b = 0; b < 3; b++) { const int c = rx.bit((s >> b) & 1); if (c > 0) out += (char)c; }
        }
        CHECK(out == msg, "varicode round trip \"%s\"", out.c_str());
    }
    const std::string m1 = "CQ CQ DE LY2ABC LY2ABC KO24 PSE K\n";
    // 2. tuning errors, drift, clock error
    struct Case { int mode; double hz, cfo, drift, ppm, snr; } cases[] = {
        {7, 1000, 0, 0, 0, 18}, {7, 1500, 60, 0, 0, 18}, {7, 800, -45, 3, 40, 18}, {8, 1200, 0, 0, 0, 20}, {8, 2000, 35, 2, 30, 20}};
    for (const auto& c : cases) {
        auto s = sig(c.hz, m1, c.snr, c.mode);
        s.driftHzPerMin = c.drift; s.clockPpm = c.ppm;
        const PskTelemetry t = run({s}, c.mode == 7 ? 7 : 5, c.mode, c.cfo);
        const std::string got = textNear(t, c.hz + c.cfo);
        std::printf("  %s %.0f Hz cfo %+.0f drift %.0f/min clock %.0f ppm: \"%s\"\n", pskModeName(c.mode), c.hz, c.cfo, c.drift, c.ppm, got.c_str());
        CHECK(got.find("DE LY2ABC LY2ABC") != std::string::npos, "%s at %.0f Hz, cfo %.0f", pskModeName(c.mode), c.hz, c.cfo);
    }
    // 3. noise sweep (SNR in the baud-rate bandwidth), 8PSK125: the lowest SNR that still gives the call sign
    {
        const bool lng = std::getenv("ONAIR_TEST_LONG") != nullptr;
        const double snrs[] = {20, 17, 14, 11};
        int okAt[4] = {};
        const int seeds = lng ? 6 : 2;
        for (int i = 0; i < 4; i++)
            for (int sd = 1; sd <= seeds; sd++) {
                const PskTelemetry t = run({sig(1300, m1, snrs[i], 7)}, 7, 7, 0, (uint32_t)(10 + sd));
                if (textNear(t, 1300).find("LY2ABC") != std::string::npos) okAt[i]++;
            }
        std::printf("8PSK125 noise sweep (ok of %d):", seeds);
        for (int i = 0; i < 4; i++) std::printf(" %.0f dB: %d", snrs[i], okAt[i]);
        std::printf("\n");
        CHECK(okAt[0] == seeds && okAt[1] == seeds, "strong 8PSK125 must decode: %d %d", okAt[0], okAt[1]);
    }
    // 4. two signals at once, 8PSK250 and 8PSK125 cannot share a mode setting: two 8PSK250 signals 900 Hz apart
    {
        const PskTelemetry t = run({sig(1000, "FIRST DE SP3XYZ SP3XYZ K\n", 20, 8), sig(1900, "SECOND DE OH1ABC OH1ABC K\n", 18, 8)}, 5, 8);
        CHECK(textNear(t, 1000).find("SP3XYZ") != std::string::npos && textNear(t, 1900).find("OH1ABC") != std::string::npos, "two signals: \"%s\" \"%s\"",
              textNear(t, 1000).c_str(), textNear(t, 1900).c_str());
    }
    // 5. a BPSK31 signal set to 8PSK125 gives no readable call sign; noise alone gives no text
    {
        const PskTelemetry t = run({sig(1100, m1, 20, 0)}, 8, 7);
        CHECK(textNear(t, 1100).find("LY2ABC") == std::string::npos, "BPSK31 read as 8PSK125 \"%s\"", textNear(t, 1100).c_str());
        const PskTelemetry n = run({}, 8, 7);
        size_t chars = 0;
        for (const auto& s : n.signals) chars += s.text.size();
        CHECK(chars < 10, "noise alone gave %zu characters", chars);
    }
    return testSummary("8PSK");
}
