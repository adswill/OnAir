// PACTOR-I receiver on the generator's signals: the ARQ script clean (connect, speed change, both header values, ASCII and Huffman text, 100 and
// 200 baud, a repeat, a changeover, QRT, the swapped polarity of every second cycle), then the impairments one by one: tuning errors of +-100 Hz,
// a sender clock off by 150 ppm, noise, the unproto (FEC) broadcast, memory ARQ on a weak signal, and noise alone (nothing may be reported).
#include "dect2/pactor/pactor_gen.h"
#include "dect2/pactor/pactor_proto.h"
#include "dect2/pactor/pactor_rx.h"
#include "dect2/pactor/pactor_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 250000;

static PactorTelemetry run(PactorGenConfig c, double extraSec = 2.0) {
    c.rate = kRate;
    c.dialOffsetHz = -20000;
    c.repeat = false;
    PactorGenerator g(c);
    PactorReceiver r;
    r.setSilent(true);
    r.configure(kRate);
    r.setSignalOffset(-20000);
    std::vector<cf32> buf(25000);
    const size_t tot = (size_t)((g.scriptSeconds() + extraSec) * kRate);
    for (size_t i = 0; i < tot; i += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    r.flush();
    PactorTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static int countKind(const PactorTelemetry& t, const char* k) { int n = 0; for (const auto& f : t.frames) if (f.kind == k) n++; return n; }
static bool has(const std::string& s, const char* x) { return s.find(x) != std::string::npos; }

static bool scriptOk(const PactorTelemetry& t, bool strict = true) {
    // 7 packets (one is a repeat), 7 control signals
    const bool a = t.blocksOk == 7 && t.repeats == 1 && t.csCount == 7 && t.changeovers == 1 && t.connects == 1;
    const bool b = has(t.text, "LY2ABC de LY1XYZ") && has(t.text, "tea set at sea") && has(t.text, "ASCII at 200 baud 73") && has(t.text, "a tea set");
    return strict ? a && b : b;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    pactor::huffReset();
    {   // the whole ARQ script, clean
        PactorGenConfig c;
        c.snrDb = 99;
        c.events = pactorTestScript(false);
        const PactorTelemetry t = run(c);
        printf("  %llu packets, %llu CS, %llu repeats, %zu stations, %llu bad\n  text: \"%s\"\n", (unsigned long long)t.blocksOk, (unsigned long long)t.csCount,
               (unsigned long long)t.repeats, t.stations.size(), (unsigned long long)t.blocksBad, t.text.c_str());
        CHECK(scriptOk(t), "script not read: %llu packets %llu CS %llu repeats", (unsigned long long)t.blocksOk, (unsigned long long)t.csCount, (unsigned long long)t.repeats);
        CHECK(t.fastPackets == 2 && t.slowPackets == 5, "speeds %llu fast %llu slow", (unsigned long long)t.fastPackets, (unsigned long long)t.slowPackets);
        CHECK(countKind(t, "CS1") == 4 && countKind(t, "CS2") == 1 && countKind(t, "CS3") == 1 && countKind(t, "CS4") == 1, "control signals");
        CHECK(countKind(t, "connect") == 1 && countKind(t, "QRT") == 1, "connect and QRT");
        bool called = false;
        for (const auto& s : t.stations) if (s.call == "LY1XYZ") called = true;
        CHECK(called, "the called station");
        bool inv = false, nrm = false, h55 = false, haa = false;
        for (const auto& f : t.frames) { (f.inverted ? inv : nrm) = true; if (f.header == 0x55) h55 = true; if (f.header == 0xAA) haa = true; }
        CHECK(inv && nrm && h55 && haa, "both polarities and both headers");
        CHECK(t.blocksBad == 0, "%llu bad", (unsigned long long)t.blocksBad);
        CHECK(!t.fecMode, "ARQ, not FEC");
        CHECK(t.text.find("[2]") != std::string::npos, "the second side after the changeover");
        CHECK(std::fabs(t.pairLowHz - 1400) < 3, "tone pair %.1f", t.pairLowHz);
    }
    // tuning errors of the radio, in noise
    for (double cfo : {-100.0, -47.0, 0.0, 33.0, 100.0}) {
        PactorGenConfig c;
        c.snrDb = 14;
        c.cfoHz = cfo;
        c.events = pactorTestScript(false);
        const PactorTelemetry t = run(c);
        CHECK(scriptOk(t), "cfo %+.0f Hz: %llu packets %llu CS", cfo, (unsigned long long)t.blocksOk, (unsigned long long)t.csCount);
        CHECK(std::fabs(t.pairLowHz - (1400 + cfo)) < 4, "cfo %+.0f read as %.1f", cfo, t.pairLowHz - 1400);
    }
    // the sender's clock
    for (double ppm : {-150.0, 150.0}) {
        PactorGenConfig c;
        c.snrDb = 16; c.clockPpm = ppm; c.events = pactorTestScript(false);
        const PactorTelemetry t = run(c);
        CHECK(scriptOk(t), "clock %+.0f ppm: %llu packets", ppm, (unsigned long long)t.blocksOk);
    }
    // weaker signals
    for (double snr : {10.0, 6.0}) {
        PactorGenConfig c;
        c.snrDb = snr; c.events = pactorTestScript(false);
        const PactorTelemetry t = run(c);
        printf("  snr %.0f dB: %llu packets, %llu CS, %llu bad\n", snr, (unsigned long long)t.blocksOk, (unsigned long long)t.csCount, (unsigned long long)t.blocksBad);
        CHECK(scriptOk(t, snr >= 10), "snr %.0f", snr);
    }
    {   // unproto / FEC: every packet three times in a row, no control signals
        PactorGenConfig c;
        c.snrDb = 99;
        c.events = pactorTestScript(true);
        const PactorTelemetry t = run(c);
        printf("  FEC: %llu packets, %llu repeats\n  text: \"%s\"\n", (unsigned long long)t.blocksOk, (unsigned long long)t.repeats, t.text.c_str());
        CHECK(t.csCount == 0 && t.fecMode, "FEC mode (%llu CS)", (unsigned long long)t.csCount);
        CHECK(t.blocksOk >= 15 && t.repeats >= 10, "%llu packets %llu repeats", (unsigned long long)t.blocksOk, (unsigned long long)t.repeats);
        CHECK(has(t.text, "CQ CQ DE LY2ABC LY2ABC PSE K"), "FEC text once");
        size_t n = 0, p = 0;
        while ((p = t.text.find("CQ CQ DE", p)) != std::string::npos) { n++; p++; }
        CHECK(n == 1, "the repeats must not be added to the text again (%zu)", n);
        bool fecFlag = false;
        for (const auto& f : t.frames) if (f.repeat && f.fec) fecFlag = true;
        CHECK(fecFlag, "repeats flagged as FEC");
    }
    {   // memory ARQ: one packet sent four times near the threshold; no copy needs to be clean, the sum of the copies must give the packet
        pactor::Packet p = pactorTextPacket("ABCDEFGHIJKLMNOPQRST", false, pactor::kHeaderA, 1, true);
        int bestMem = -1, bestOk = 0;
        for (double snr : {-2.5, -2.0, -1.5, -1.0, -0.5, 0.0}) {
            PactorScriptBuilder b;
            for (int i = 0; i < 6; i++) { b.packet(p, true); b.cs(i == 5 ? 1 : 2); b.next(); }
            PactorGenConfig c;
            c.snrDb = snr; c.events = b.ev; c.lengthSec = b.end();
            const PactorTelemetry t = run(c);
            printf("  memory ARQ at %.1f dB: %llu ok, %llu bad, %llu saved\n", snr, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.memArqSaves);
            if (t.memArqSaves > 0 && has(t.text, "ABCDEFGHIJKLMNOPQRST")) { bestMem = (int)t.memArqSaves; break; }
            bestOk = std::max(bestOk, (int)t.blocksOk);
        }
        CHECK(bestMem > 0, "memory ARQ never saved a packet (best %d clean copies)", bestOk);
    }
    {   // noise only: nothing
        PactorGenConfig c;
        c.snrDb = 20;
        c.lengthSec = 20;
        const PactorTelemetry t = run(c, 6.0);
        CHECK(t.frames.empty() && t.blocksOk == 0 && t.pairLowHz == 0, "reports from noise: %zu frames", t.frames.size());
    }
    {   // another tone pair (audio centre 2100 Hz), a lone packet without a control signal
        PactorGenConfig c;
        c.snrDb = 20; c.audioCentreHz = 2100;
        PactorScriptBuilder b;
        b.packet(pactorTextPacket("LY2ABC test", false, pactor::kHeaderA, 0, true), true);
        c.events = b.ev; c.lengthSec = b.end();
        const PactorTelemetry t = run(c);
        CHECK(t.blocksOk == 1 && has(t.text, "LY2ABC test") && std::fabs(t.pairLowHz - 2000) < 3, "other tone pair: %llu packets, pair %.1f", (unsigned long long)t.blocksOk, t.pairLowHz);
    }
    return testSummary("pactor rx");
}
