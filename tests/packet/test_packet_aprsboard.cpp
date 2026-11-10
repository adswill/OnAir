// APRS weather stations and messages (aprs_board.h, docs/APRS-BOARD-3.9.md): the unit conversions, the merge and the 24 h history of the weather
// values, the addressee kinds, "to me", retries, acks and rejects, and then the generator's cycle through the real receiver under offsets, noise and
// clock error: the weather station must carry its values and the message must show as acknowledged. Simulated signals only.
#include "dect2/packet/aprs_board.h"
#ifndef APRSBOARD_LOGIC_ONLY
#include "dect2/packet/packet_gen.h"
#include "dect2/packet/packet_rx.h"
#include "../common/impair.h"
#endif
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool nearV(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static aprs::Info msg(const std::string& to, const std::string& text, const std::string& no = "", bool ack = false, bool rej = false) {
    aprs::Info a;
    a.type = "Message"; a.name = to; a.text = text; a.msgNo = no; a.isAck = ack; a.isRej = rej;
    return a;
}

static void kinds() {
    CHECK(aprsAddresseeKind("BLN0") == AprsMsgKind::Bulletin, "BLN0");
    CHECK(aprsAddresseeKind("BLN9     ") == AprsMsgKind::Bulletin, "BLN9 padded");
    CHECK(aprsAddresseeKind("BLNA") == AprsMsgKind::Announcement, "BLNA");
    CHECK(aprsAddresseeKind("blnq") == AprsMsgKind::Announcement, "lower case");
    CHECK(aprsAddresseeKind("BLN3WX") == AprsMsgKind::GroupBulletin, "BLN3WX");
    CHECK(aprsAddresseeKind("NWS-TSTRM") == AprsMsgKind::Nws, "NWS-");
    CHECK(aprsAddresseeKind("SKYMRX") == AprsMsgKind::Nws, "SKY");
    CHECK(aprsAddresseeKind("CWAOKC") == AprsMsgKind::Nws, "CWA");
    CHECK(aprsAddresseeKind("LY2XX-7") == AprsMsgKind::Message, "station");
    CHECK(aprsAddresseeKind("BLN") == AprsMsgKind::Message, "BLN alone is a callsign");
    CHECK(aprsAddresseeKind("SKY") == AprsMsgKind::Message, "SKY alone");
    CHECK(aprsIsForCall("LY2XX-7", "LY2XX"), "no SSID matches every SSID");
    CHECK(aprsIsForCall("ly2xx", "LY2XX"), "case");
    CHECK(aprsIsForCall("LY2XX  ", "LY2XX-0"), "-0 is the bare call");
    CHECK(!aprsIsForCall("LY2XX-7", "LY2XX-9"), "another SSID");
    CHECK(aprsIsForCall("LY2XX-9", "LY2XX-9"), "same SSID");
    CHECK(!aprsIsForCall("LY2XXA", "LY2XX"), "longer call");
    CHECK(!aprsIsForCall("LY2XX", ""), "no call set");
}

static void messages() {
    AprsMessageLog log;
    log.setMyCall("LY2XX");
    CHECK(log.add(10, "LY1AB-9", msg("LY2XX-7", "hello", "12")) == AprsMessageLog::Added::New, "new message");
    CHECK(log.list().size() == 1 && log.list()[0].toMe && !log.list()[0].fromMe, "to me");
    CHECK(log.add(40, "LY1AB-9", msg("LY2XX-7", "hello", "12")) == AprsMessageLog::Added::Again, "a retry");
    CHECK(log.list()[0].copies == 2 && log.list()[0].lastSec == 40, "copies %u", log.list()[0].copies);
    CHECK(log.add(41, "LY2XX-7", msg("LY1AB-9", "", "13", true)) == AprsMessageLog::Added::No, "ack of another number");
    CHECK(!log.list()[0].acked, "not acked by number 13");
    CHECK(log.add(42, "LY3ZZ", msg("LY1AB-9", "", "12", true)) == AprsMessageLog::Added::No, "ack from a third station");
    CHECK(log.add(43, "ly2xx-7", msg("LY1AB-9", "", "12", true)) == AprsMessageLog::Added::Ack && log.list()[0].acked, "acked");
    CHECK(log.add(50, "LY2XX-7", msg("LY1AB-9", "hi back", "5")) == AprsMessageLog::Added::New && log.list()[1].fromMe && !log.list()[1].toMe, "reply");
    CHECK(aprsThreadKey(log.list()[0]) == aprsThreadKey(log.list()[1]), "one conversation: %s / %s", aprsThreadKey(log.list()[0]).c_str(), aprsThreadKey(log.list()[1]).c_str());
    CHECK(log.add(51, "LY1AB-9", msg("LY2XX-7", "", "5", false, true)) == AprsMessageLog::Added::Ack && log.list()[1].rejected && !log.list()[1].acked, "rejected");
    CHECK(log.add(60, "LY4WX", msg("LY4WX", "PARM.Volt,Temp")) == AprsMessageLog::Added::No, "telemetry definitions are not messages");
    CHECK(log.add(70, "LY1AB-9", msg("LY2XX-7", "hello", "12")) == AprsMessageLog::Added::Again, "a late retry");
    CHECK(log.add(70 + 3700, "LY1AB-9", msg("LY2XX-7", "hello", "12")) == AprsMessageLog::Added::New, "the same text an hour later is new");
    CHECK(log.add(100, "LY5B", msg("BLN1", "Net tonight 20:00")) == AprsMessageLog::Added::New && log.list().back().kind == AprsMsgKind::Bulletin && !log.list().back().toMe, "bulletin");
    CHECK(log.add(100 + 6 * 3600, "LY5B", msg("BLN1", "Net tonight 20:00")) == AprsMessageLog::Added::Again, "bulletins repeat for hours");
    CHECK(log.add(110, "LY5B", msg("", "x")) == AprsMessageLog::Added::No, "no addressee");
    aprs::Info pos; pos.type = "Position"; pos.name = "X";
    CHECK(log.add(111, "LY5B", pos) == AprsMessageLog::Added::No, "not a message");
    // the callsign set later marks the listed messages again
    AprsMessageLog l2;
    l2.add(1, "A", msg("B-3", "x", "1"));
    CHECK(!l2.list()[0].toMe, "no call yet");
    l2.setMyCall("b");
    CHECK(l2.list()[0].toMe, "marked after setMyCall");
    l2.shiftTimes(-1);
    CHECK(l2.list()[0].timeSec == 0 && l2.list()[0].lastSec == 0, "shift");
    // the cap: the oldest leave
    AprsMessageLog l3; l3.setMax(20);
    for (int i = 0; i < 50; i++) l3.add(i, "A", msg("B", "m" + std::to_string(i), std::to_string(i)));
    CHECK(l3.list().size() == 20 && l3.list().front().text == "m30", "cap %zu %s", l3.list().size(), l3.list().front().text.c_str());
}

static void weather() {
    aprs::Weather w;
    w.windDirDeg = 220; w.windMph = 4; w.gustMph = 5; w.tempF = 98; w.rain1hIn = 0.10; w.rain24hIn = 1; w.rainMidnightIn = 0.5; w.humidityPct = 45; w.pressureMbar = 1009.8;
    const AprsWx x = aprsWxFromReport(w, 100);
    CHECK(nearV(x.tempC, 36.67, 0.01) && nearV(x.windKmh, 6.437, 0.01) && nearV(x.gustKmh, 8.047, 0.01) && nearV(x.rain1hMm, 2.54, 0.001) && nearV(x.rain24hMm, 25.4, 0.001) &&
          nearV(x.rainMidnightMm, 12.7, 0.001) && nearV(x.humidityPct, 45, 0) && nearV(x.pressureHpa, 1009.8, 0.01) && nearV(x.windDirDeg, 220, 0), "units");
    CHECK(aprsWxLabel(x) == "36.7 \xC2\xB0""C  SW 6 km/h", "label '%s'", aprsWxLabel(x).c_str());
    aprs::Weather bad; bad.tempF = 400; bad.humidityPct = 140; bad.pressureMbar = 20;
    CHECK(!aprsWxFromReport(bad, 0).any(), "impossible values dropped");
    CHECK(std::string(aprsCompass(0)) == "N" && std::string(aprsCompass(359)) == "N" && std::string(aprsCompass(90)) == "E" && std::string(aprsCompass(NAN)).empty(), "compass");
    // a report without temperature keeps the last one for the table, the history keeps the gap
    AprsWx last; std::vector<AprsWx> hist;
    aprsWxAdd(last, hist, x);
    aprs::Weather w2; w2.windMph = 10;
    aprsWxAdd(last, hist, aprsWxFromReport(w2, 1000));
    CHECK(nearV(last.tempC, 36.67, 0.01) && nearV(last.windKmh, 16.09, 0.01), "merged");
    CHECK(hist.size() == 2 && std::isnan(hist[1].tempC), "history %zu", hist.size());
    aprsWxAdd(last, hist, aprsWxFromReport(w2, 1000 + 3 * 3600));
    CHECK(std::isnan(last.tempC), "old values expire after 2 h");
    // 26 h of reports every 60 s: one point per 5 minutes, the last 24 h
    AprsWx l2; std::vector<AprsWx> h2;
    for (int i = 0; i < 26 * 60; i++) { aprs::Weather q; q.tempF = 50 + (i % 10); aprsWxAdd(l2, h2, aprsWxFromReport(q, i * 60.0)); }
    CHECK(h2.size() <= (size_t)(kAprsWxHistorySec / kAprsWxStepSec) + 1 && h2.size() >= 280, "points %zu", h2.size());
    CHECK(h2.back().timeSec - h2.front().timeSec <= kAprsWxHistorySec, "span %.0f", h2.back().timeSec - h2.front().timeSec);
    for (size_t i = 1; i < h2.size(); i++) if (h2[i].timeSec < h2[i - 1].timeSec) { CHECK(false, "order at %zu", i); break; }
    // a delayed copy goes into its place
    AprsWx l3; std::vector<AprsWx> h3;
    aprs::Weather q; q.tempF = 50;
    aprsWxAdd(l3, h3, aprsWxFromReport(q, 0)); aprsWxAdd(l3, h3, aprsWxFromReport(q, 1000)); aprsWxAdd(l3, h3, aprsWxFromReport(q, 500));
    CHECK(h3.size() == 3 && h3[1].timeSec == 500, "out of order");
    aprsWxAdd(l3, h3, AprsWx());
    CHECK(h3.size() == 3, "an empty report adds nothing");
}

#ifndef APRSBOARD_LOGIC_ONLY
using V = std::vector<cf32>;
static PacketTelemetry run(const V& x, double rate, const std::string& myCall) {
    PacketReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-packetTuning().tuneOffsetHz);
    rx.setMyCall(myCall);
    PacketTelemetry t; uint64_t seq = 0;
    auto take = [&] { PacketTelemetry n; if (rx.telemetry(n, seq)) { seq = n.seq; t = n; } };
    for (size_t i = 0; i < x.size(); i += 65536) { rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i)); take(); }
    V z(65536);
    for (int k = 0; k < 8; k++) { rx.feed(z.data(), z.size()); take(); }
    return t;
}

static void roundTrip() {
    const double rate = 1.2e6;
    const std::vector<std::pair<const char*, std::function<void(V&)>>> cases = {
        {"clean", [](V&) {}},
        {"carrier -8 kHz", [&](V& x) { impair::shift(x, -8000, rate); }},
        {"carrier +10 kHz", [&](V& x) { impair::shift(x, 10000, rate); }},
        {"clock +80 ppm", [](V& x) { x = impair::clock(x, 80); }},
        {"combined: -8 kHz, -80 ppm, echo", [&](V& x) { impair::shift(x, -8000, rate); x = impair::clock(x, -80); impair::echo(x, 37, -6, 2.0); }},
    };
    for (double snr : {25.0, 18.0}) {
        SynthConfig c; c.snrDb = snr;
        auto syn = makePacketSynth(c, rate);
        V base((size_t)(rate * (packetGenCycleSec(0) + 1)));
        for (size_t i = 0; i < base.size(); i += 65536) syn->generate(base.data() + i, std::min<size_t>(65536, base.size() - i));
        for (const auto& [name, f] : cases) {
            V x = base; f(x);
            const PacketTelemetry t = run(x, rate, "A61QQ");
            const PacketStation* wx = nullptr;
            for (const auto& s : t.stations) if (s.call == "A61WX") wx = &s;
            const AprsMessage* m = nullptr;
            for (const auto& q : t.messages) if (q.from == "A61TST" && q.to == "A61QQ-9") m = &q;
            printf("SNR %.0f dB, %-34s weather %s, %zu messages, message %s\n", snr, name, wx && wx->hasWx ? aprsWxLabel(wx->wx).c_str() : "none", t.messages.size(),
                   !m ? "missing" : m->acked ? "acked" : "not acked");
            CHECK(wx && wx->hasWx && nearV(wx->wx.tempC, 36.67, 0.05) && nearV(wx->wx.windDirDeg, 220, 0) && nearV(wx->wx.windKmh, 6.44, 0.05) &&
                  nearV(wx->wx.gustKmh, 8.05, 0.05) && nearV(wx->wx.humidityPct, 45, 0) && nearV(wx->wx.pressureHpa, 1009.8, 0.05) && wx->wxHist.size() == 1,
                  "%s: weather station", name);
            CHECK(m && m->text == "Hello from the desert" && m->msgNo == "42" && m->acked && m->toMe && m->kind == AprsMsgKind::Message, "%s: message", name);
            CHECK(t.myCall == "A61QQ", "myCall published");
        }
    }
}
#endif

int main() {
    kinds();
    messages();
    weather();
#ifndef APRSBOARD_LOGIC_ONLY
    roundTrip();
#endif
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
