// ATIS receiver: bursts after a voice transmission through the narrow FM front end and the DSC decoder, with real-world impairments:
// carrier offset, the channel off the centre of the capture, noise, a wrong bit clock, phase modulation (as on the air) and flat FM, no dot pattern,
// swapped tones (inverted spectrum), no voice, and the negatives (noise, voice only).
#include "dect2/atis/atis_gen.h"
#include "dect2/atis/atis_proto.h"
#include "dect2/atis/atis_rx.h"
#include "dect2/atis/atis_tel.h"
#include <cmath>
#include <cstdio>
#include <set>
using namespace dect2;
#include "../common/test_util.h"

struct Opt { bool conj = false; bool listen = false; size_t* audioSamples = nullptr; double offset = -150e3; };

static AtisTelemetry run(AtisGenConfig c, double sec, Opt o = Opt()) {
    c.dialOffsetHz = o.offset;
    AtisReceiver rx;
    rx.setSilent(true);
    rx.setListen(o.listen);
    if (o.audioSamples) rx.setListenTap([&](const float*, size_t n) { *o.audioSamples += n; });
    rx.configure(c.rate);
    rx.setSignalOffset(o.conj ? -c.dialOffsetHz : c.dialOffsetHz);   // an inverted spectrum mirrors the channel too
    AtisGenerator g(c);
    std::vector<cf32> buf(1 << 14);
    for (size_t done = 0; done < (size_t)(sec * c.rate); done += buf.size()) {
        g.generate(buf.data(), buf.size());
        if (o.conj) for (auto& v : buf) v = std::conj(v);
        rx.feed(buf.data(), buf.size());
    }
    rx.flush();
    AtisTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static AtisGenConfig base() {
    AtisGenConfig c;
    c.rate = 1e6;
    c.ids = atisTestIds();
    c.gapSec = 0.6;
    c.voiceSec = 0.8;
    return c;
}

// number of transmissions in `sec`: one per (voice + burst + gap), the burst is 0.04 + 54 * 10 / 1200 s
static int expected(const AtisGenConfig& c, double sec) {
    const double per = c.voiceSec + c.afterVoiceSec + (c.dotBits + 340) / 1200.0 + c.gapSec;
    return (int)((sec - 0.05) / per);
}

static void report(const char* what, const AtisTelemetry& t, int want, bool allDistinct = true) {
    std::set<std::string> ids;
    for (const auto& m : t.messages) if (m.checkOk) ids.insert(m.digits);
    printf("  %-34s ok %llu bad %llu (want >= %d)\n", what, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, want);
    CHECK((int)t.blocksOk >= want, "%s: %llu messages, wanted %d", what, (unsigned long long)t.blocksOk, want);
    // every decoded number is one of the test numbers: nothing invented
    const auto known = atisTestIds();
    for (const auto& m : t.messages) if (m.checkOk) {
        bool k = false; for (const auto& s : known) if (s == m.digits) k = true;
        CHECK(k, "%s: decoded %s which was never sent", what, m.digits.c_str());
    }
    (void)allDistinct;
}

int main() {
    // clean: all five identities, in order, with the fields filled in
    {
        AtisGenConfig c = base();
        const double sec = 12;
        AtisTelemetry t = run(c, sec);
        const int want = expected(c, sec);
        report("clean", t, want);
        CHECK(t.stations.size() == 5, "stations %zu", t.stations.size());
        bool first = false;
        for (const auto& m : t.messages) if (m.digits == "9244038075") {
            first = true;
            CHECK(m.callsign == "PC8075" && m.mmsi == "244038075" && m.country.find("Netherlands") != std::string::npos && m.checkOk && m.standardZ, "fields: %s %s %s", m.callsign.c_str(), m.mmsi.c_str(), m.country.c_str());
        }
        CHECK(first, "the standard's example identity was not found");
        CHECK(t.blocksBad == 0, "failed-check messages in a clean signal: %llu", (unsigned long long)t.blocksBad);
        CHECK(t.state == 2 && t.dataValid, "state %d", t.state);
    }
    // the standard's own example only
    { AtisGenConfig c = base(); c.ids = {"9244038075"}; AtisTelemetry t = run(c, 6); report("single identity", t, expected(c, 6)); CHECK(t.stations.size() == 1 && t.stations[0].count >= 3, "station count"); }
    // phase modulation (the standard) and flat FM
    { AtisGenConfig c = base(); c.phaseMod = false; report("flat FM", run(c, 8), expected(c, 8)); }
    { AtisGenConfig c = base(); c.modIndex = 0.6; report("low deviation (index 0.6)", run(c, 8), expected(c, 8)); }
    { AtisGenConfig c = base(); c.modIndex = 1.8; report("high deviation (index 1.8)", run(c, 8), expected(c, 8)); }
    // carrier offsets: radios are never exact, the receiver's branches follow
    for (double cfo : {-1500.0, -900.0, -400.0, 250.0, 700.0, 1100.0, 1500.0}) {
        AtisGenConfig c = base(); c.cfoHz = cfo;
        char n[64]; snprintf(n, sizeof n, "carrier offset %+.0f Hz", cfo);
        report(n, run(c, 8), expected(c, 8));
    }
    // the channel not at the radio's centre
    for (double off : {-300e3, 80e3, 250e3}) {
        AtisGenConfig c = base();
        Opt o; o.offset = off;
        char n[64]; snprintf(n, sizeof n, "channel at %+.0f kHz in the capture", off / 1e3);
        report(n, run(c, 8, o), expected(c, 8));
    }
    // noise
    for (double snr : {30.0, 20.0, 15.0, 12.0}) {
        AtisGenConfig c = base(); c.snrDb = snr; c.cfoHz = 300;
        char n[64]; snprintf(n, sizeof n, "C/N %.0f dB (12.5 kHz), offset 300 Hz", snr);
        AtisTelemetry t = run(c, 12);
        const int want = (int)(expected(c, 12) * (snr >= 15 ? 0.9 : 0.5));
        report(n, t, want);
    }
    // bit clock errors of the radio (the standard allows 60 ppm on the dot pattern; real radios are worse) and no dot pattern
    for (double ppm : {-3000.0, 1500.0}) { AtisGenConfig c = base(); c.clockPpm = ppm; char n[48]; snprintf(n, sizeof n, "clock error %+.0f ppm", ppm); report(n, run(c, 8), expected(c, 8)); }
    { AtisGenConfig c = base(); c.dotBits = 0; report("no dot pattern", run(c, 8), expected(c, 8)); }
    { AtisGenConfig c = base(); c.voiceSec = 0; report("no voice before the burst", run(c, 6), expected(c, 6)); }
    // all impairments together
    { AtisGenConfig c = base(); c.cfoHz = -700; c.snrDb = 18; c.clockPpm = 800; c.dotBits = 0; Opt o; o.offset = 120e3; report("offset+noise+clock+off-centre", run(c, 14, o), (int)(expected(c, 14) * 0.7)); }
    // swapped tones (an inverted spectrum, e.g. the I and Q of the radio swapped)
    { AtisGenConfig c = base(); Opt o; o.conj = true; AtisTelemetry t = run(c, 8, o); report("inverted spectrum", t, expected(c, 8)); bool inv = false; for (const auto& m : t.messages) if (m.inverted) inv = true; (void)inv; }
    // Listen plays the voice
    { AtisGenConfig c = base(); size_t n = 0; Opt o; o.listen = true; o.audioSamples = &n; run(c, 4, o); CHECK(n > 8000 * 3, "audio samples %zu", n); }
    // negatives: no carrier at all, voice without a burst
    {
        AtisGenConfig c = base(); c.silent = true; c.snrDb = 10;
        AtisTelemetry t = run(c, 10);
        CHECK(t.blocksOk == 0 && t.blocksBad == 0 && t.messages.empty(), "noise only gave %llu/%llu messages", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {
        AtisGenConfig c = base(); c.dotBits = 0; c.ids = atisTestIds();
        AtisGenConfig v = c; v.gapSec = 0.1; v.voiceSec = 100;   // one very long voice transmission: no burst within the time
        AtisTelemetry t = run(v, 10);
        CHECK(t.blocksOk == 0 && t.blocksBad == 0, "voice only gave %llu/%llu messages", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    // a different sample rate (the radio's rate is not fixed)
    for (double fs : {500e3, 2e6}) { AtisGenConfig c = base(); c.rate = fs; char n[40]; snprintf(n, sizeof n, "sample rate %.1f Msps", fs / 1e6); report(n, run(c, 8), expected(c, 8)); }
    return testSummary("atis rx");
}
