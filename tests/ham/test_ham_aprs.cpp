// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham APRS / packet: paths, APRS fields (back through the repo's APRS parser), the message session (two stations), the beacon, and the audio
// of 1200, 9600 and 300 baud through receivers (pktdemod::Afsk1200, pktdemod::FskSlicer, AfskRx) with noise, clock error, tone twist and
// tuning error; prints the 50 % frame SNR of each speed. Nothing is transmitted: the rig is a recording fake.
#include "dect2/ham/ham_aprs.h"
#include "dect2/packet/packet_aprs.h"
#include "dect2/packet/packet_demod.h"
#include "../common/test_util.h"
#include <cmath>
#include <cstring>
using namespace dect2;
using namespace dect2::ham;

struct Rec : HamRigIo {
    std::vector<float> a;
    int ptt1 = 0, ptt0 = 0;
    bool ptt(bool on, std::string&) override { (on ? ptt1 : ptt0)++; return true; }
    bool beginAudio(int, std::string&) override { a.clear(); return true; }
    int write(const float* m, int n) override { a.insert(a.end(), m, m + n); return n; }
    int pending() override { return 0; }
    void flushAudio() override {}
    void endAudio() override {}
};
static HamTxCheck gate(double dial) {
    HamTxCheck g; g.licenceConfirmed = true; g.armed = true; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = dial; return g;
}
struct Gauss { uint64_t s; explicit Gauss(uint64_t seed) : s(seed) {} double u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return ((s >> 11) + 0.5) / 9007199254740992.0; } double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); } };
static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

// the audio with a clock error (linear interpolation), then white noise: snrDb of a peak-1 sine in `bwHz` (the noise is white over 24 kHz)
static std::vector<float> channel(const std::vector<float>& a, double snrDb, double ppm, double bwHz, uint64_t seed, double gain = 1) {
    std::vector<float> o;
    const double r = 1 + ppm * 1e-6;
    for (double t = 0; t + 1 < (double)a.size(); t += r) { const size_t i = (size_t)t; const double f = t - (double)i; o.push_back((float)(gain * (a[i] * (1 - f) + a[i + 1] * f))); }
    o.insert(o.begin(), 4800, 0.f);
    o.insert(o.end(), 4800, 0.f);
    const double sigma = std::sqrt(0.5 * gain * gain / std::pow(10.0, snrDb / 10) * 24000.0 / bwHz);
    Gauss g(seed);
    if (snrDb < 99) for (auto& v : o) v += (float)(sigma * g.g());
    return o;
}

static std::vector<ax25::Frame> sample(int i) {
    std::string e;
    ax25::Frame f;
    AprsPos p; p.lat = 54.6872; p.lon = 25.2797; p.symCode = '-'; p.comment = "OnAir test " + std::to_string(i);
    ax25UiFrame("LY1ABC-7", kAprsToCall, "WIDE1-1,WIDE2-1", i % 2 ? aprsMessage("LY2XYZ", "Hello number " + std::to_string(i), std::to_string(i)) : aprsPosition(p), f, e);
    return {f};
}

enum Speed { S1200, S9600, S300 };
// frames decoded of `n` sent at snrDb
static int decodeRun(Speed sp, double snrDb, double ppm, int n, double tuneHz, double twistDb, std::vector<std::string>* got = nullptr) {
    int ok = 0;
    for (int i = 0; i < n; i++) {
        const auto fr = sample(i);
        PacketAudioCfg c;
        c.baud = sp == S1200 ? PacketBaud::B1200 : sp == S9600 ? PacketBaud::B9600 : PacketBaud::B300;
        c.emphasisDb = twistDb;
        c.centreHz = 1700 + tuneHz;
        std::vector<float> a = packetAudio(fr, c);
        const std::string want = fr[0].tnc2();
        bool hit = false;
        auto cb = [&](const std::vector<uint8_t>& d, bool good) {
            if (!good) return;
            ax25::Frame f;
            if (ax25::parseFrame(d.data(), d.size(), f) && f.tnc2() == want) hit = true;
            if (got) got->push_back(f.tnc2());
        };
        const std::vector<float> x = channel(a, snrDb, ppm, sp == S9600 ? 9600 : sp == S1200 ? 3000 : 500, 1000 + (uint64_t)i * 7 + (uint64_t)(snrDb * 10));
        if (sp == S1200) {
            pktdemod::Afsk1200 d; d.init(40, false, 1.0f, 0.70f, 0.80f); d.hdlc.setCallback(cb);
            for (size_t k = 0; k < x.size(); k += 960) d.process(&x[k], std::min<size_t>(960, x.size() - k));
        } else if (sp == S9600) {
            pktdemod::FskSlicer d; d.rate = 48000; d.init(0.70f, 0.80f); d.hdlc.setCallback(cb);
            d.process(x.data(), x.size());
        } else {
            AfskRx d(48000, 1600, 1800, 300, cb);
            d.feed(x.data(), x.size());
        }
        ok += hit;
    }
    return ok;
}

int main() {
    std::string err;
    // ---- addresses and paths ----
    {
        ax25::Address a;
        CHECK(ax25ParseAddress("ly1abc-9", a) && a.call == "LY1ABC" && a.ssid == 9, "address with SSID");
        CHECK(!ax25ParseAddress("LY1ABC-16", a) && !ax25ParseAddress("TOOLONGX", a) && !ax25ParseAddress("", a) && !ax25ParseAddress("LY1/P", a), "bad addresses refused");
        std::vector<ax25::Address> p;
        CHECK(ax25ParsePath("WIDE1-1, WIDE2-1", p, err) && p.size() == 2 && p[1].call == "WIDE2" && p[1].ssid == 1, "path of two");
        CHECK(ax25ParsePath("", p, err) && p.empty(), "direct (no path)");
        CHECK(!ax25ParsePath("A,B,C,D,E,F,G,H,I", p, err) && has(err, "8"), "nine digipeaters refused");
        CHECK(!ax25ParsePath("WIDE1-1,WI*DE", p, err), "bad digipeater refused: %s", err.c_str());
        ax25::Frame f;
        CHECK(ax25UiFrame("LY1ABC-7", "APOAIR", "WIDE1-1", ">hi", f, err) && f.tnc2() == "LY1ABC-7>APOAIR,WIDE1-1:>hi", "UI frame %s", f.tnc2().c_str());
        CHECK(!ax25UiFrame("LY1ABC", "APOAIR", "", std::string(300, 'x'), f, err), "too long info refused");
        // the bytes survive build -> parse
        const auto b = ax25::buildFrame(f);
        ax25::Frame g;
        CHECK(ax25::fcsOk(b.data(), b.size()) && ax25::parseFrame(b.data(), b.size() - 2, g) && g.tnc2() == f.tnc2(), "build / parse round trip");
    }
    // ---- APRS fields, read back by the APRS parser ----
    {
        AprsPos p; p.lat = 54.6872; p.lon = 25.2797; p.symCode = '-'; p.comment = "Vilnius";
        const std::string s = aprsPosition(p);
        CHECK(s == "=5441.23N/02516.78E-Vilnius", "position \"%s\"", s.c_str());
        aprs::Info in;
        CHECK(aprs::parse("APOAIR", s, in) && in.hasPos && std::fabs(in.lat - 54.6872) < 2e-4 && std::fabs(in.lon - 25.2797) < 2e-4 && in.symCode == '-', "position parses back %.4f %.4f", in.lat, in.lon);
        p.lat = -33.8688; p.lon = -151.2093; p.messaging = false; p.altM = 100; p.comment = "";
        const std::string s2 = aprsPosition(p);
        CHECK(s2 == "!3352.13S/15112.56W-/A=000328", "south / west with altitude \"%s\"", s2.c_str());
        CHECK(aprs::parse("APOAIR", s2, in) && in.lat < -33.86 && in.lon < -151.2 && in.hasAlt && std::fabs(in.altM - 100) < 1, "south / west parses back (alt %.0f)", in.altM);
        p.ambiguity = 2;
        CHECK(aprsPosition(p).substr(1, 8) == "3352.  S", "ambiguity 2 \"%s\"", aprsPosition(p).c_str());
        const std::string m = aprsMessage("ly2xyz", "Hi {there}|~", "12");
        CHECK(m == ":LY2XYZ   :Hi there}{12", "message \"%s\"", m.c_str());
        CHECK(aprs::parse("APOAIR", m, in) && in.name == "LY2XYZ" && in.msgNo == "12", "message parses back (%s / %s)", in.name.c_str(), in.msgNo.c_str());
        CHECK(aprsAck("LY1ABC-7", "12") == ":LY1ABC-7 :ack12", "ack");
        CHECK(aprs::parse("APOAIR", aprsAck("LY1ABC-7", "12"), in) && in.isAck, "ack parses back");
        CHECK(aprsBulletin('1', "Net tonight") == ":BLN1     :Net tonight", "bulletin");
        CHECK(aprsStatus("QRV 144.800") == ">QRV 144.800", "status");
        CHECK(aprsMessage("A", std::string(100, 'x'), "").size() == 11 + 67, "message text cut to 67");
        double la, lo;
        CHECK(gridCentre("KO24", la, lo) && std::fabs(la - 54.5) < 1e-9 && std::fabs(lo - 25) < 1e-9, "KO24 centre %.3f %.3f", la, lo);
        CHECK(gridCentre("KO24pq", la, lo) && la > 54.66 && la < 54.71 && lo > 25.25 && lo < 25.34, "KO24PQ centre %.4f %.4f", la, lo);
    }
    // ---- the message session between two stations ----
    {
        AprsMessenger a, b; a.myCall = "LY1ABC-7"; b.myCall = "LY2XYZ";
        const std::string no = a.send("ly2xyz", "Hello Bob", 0, err);
        CHECK(no == "1", "first number %s", no.c_str());
        auto d = a.due(0);
        CHECK(d.size() == 1 && d[0] == ":LY2XYZ   :Hello Bob{1", "first send \"%s\"", d.empty() ? "" : d[0].c_str());
        CHECK(a.due(10).empty(), "nothing before the retry");
        auto r1 = a.due(30);
        CHECK(r1.size() == 1, "retry after 30 s");
        CHECK(a.due(60).empty() && a.due(90).size() == 1, "then 60 s later");
        auto acks = b.onReceive("LY1ABC-7", r1[0], 31);
        CHECK(acks.size() == 1 && acks[0] == ":LY1ABC-7 :ack1", "B acks \"%s\"", acks.empty() ? "" : acks[0].c_str());
        CHECK(b.inbox().size() == 1 && b.inbox()[0].text == "Hello Bob" && b.inbox()[0].from == "LY1ABC-7", "B shows it");
        CHECK(b.onReceive("LY1ABC-7", r1[0], 32).size() == 1 && b.inbox().size() == 1, "a second copy: acked again, shown once");
        CHECK(b.onReceive("LY1ABC-7", ":SOMEONE  :not for me{5", 33).empty() && b.inbox().size() == 1, "others' messages ignored");
        CHECK(a.onReceive("LY2XYZ", acks[0], 34).empty() && a.outbox()[0].acked, "A sees the ack");
        CHECK(a.due(1000).empty(), "no retries after the ack");
        // reply-ack form and give-up
        b.onReceive("LY1ABC-7", ":LY2XYZ   :reply-ack{7}3", 40);
        CHECK(b.inbox().size() == 2 && b.inbox()[1].no == "7", "reply-ack number %s", b.inbox().back().no.c_str());
        AprsMessenger c; c.myCall = "LY1ABC"; c.maxTries = 3;
        c.send("LY3QQQ", "anyone", 0, err);
        int sent = 0; for (double t = 0; t < 3600; t += 5) sent += (int)c.due(t).size();
        CHECK(sent == 3 && c.outbox()[0].gaveUp, "gives up after 3 tries (%d)", sent);
        CHECK(c.send("bad call!", "x", 0, err).empty() && !err.empty(), "bad addressee refused");
        CHECK(c.send("LY3QQQ", "   ", 0, err).empty(), "empty text refused");
    }
    // ---- beacon ----
    {
        AprsBeacon bc; bc.cfg.pos.lat = 54.68; bc.cfg.pos.lon = 25.28; bc.cfg.status = "OnAir"; bc.cfg.statusEvery = 3; bc.cfg.intervalSec = 10; bc.cfg.maxTx = 5;
        bc.start(0);
        std::vector<std::string> v;
        for (double t = 0; t < 2000; t += 1) { auto s = bc.due(t); if (!s.empty()) v.push_back(s); }
        CHECK(v.size() == 5 && !bc.active() && has(bc.stopReason(), "5"), "5 beacons then stop (%zu)", v.size());
        CHECK(v.size() == 5 && v[0][0] == '=' && v[2] == ">OnAir", "every 3rd is the status");
        bc.start(0); bc.due(0);
        CHECK(bc.due(59).empty() && !bc.due(60).empty(), "never closer than 60 s");
    }
    // ---- audio through the receivers ----
    {
        // shapes: 1200 baud tones, 9600 baseband bandwidth
        auto fr = sample(0);
        PacketAudioCfg c;
        auto a = packetAudio(fr, c);
        const size_t bits = ax25::hdlcBits(ax25::buildFrame(fr[0]), 45, 5).size();
        CHECK(std::fabs((double)a.size() - bits * 40.0) < 80, "1200 baud length %zu for %zu bits", a.size(), bits);
        float pk = 0; for (float v : a) pk = std::max(pk, std::fabs(v));
        CHECK(pk > 0.99f && pk < 1.001f, "peak 1");
        std::vector<std::string> got;
        CHECK(decodeRun(S1200, 99, 0, 2, 0, 0, &got) == 2, "1200 clean (%zu frames: %s)", got.size(), got.empty() ? "" : got[0].c_str());
        CHECK(decodeRun(S9600, 99, 0, 2, 0, 0) == 2, "9600 clean");
        CHECK(decodeRun(S300, 99, 0, 2, 0, 0) == 2, "300 clean");
        CHECK(decodeRun(S1200, 99, 300, 4, 0, 0) == 4 && decodeRun(S1200, 99, -300, 4, 0, 0) == 4, "1200: 300 ppm clock error");
        CHECK(decodeRun(S1200, 20, 0, 6, 0, -6) >= 6 && decodeRun(S1200, 20, 0, 6, 0, 6) >= 6, "1200: 6 dB twist both ways");
        CHECK(decodeRun(S9600, 99, 200, 4, 0, 0) == 4, "9600: 200 ppm clock error");
        CHECK(decodeRun(S300, 30, 0, 4, 60, 0) == 4 && decodeRun(S300, 30, 0, 4, -60, 0) == 4, "300: 60 Hz tuning error");
        CHECK(decodeRun(S300, 30, 100, 4, 0, 0) == 4, "300: clock error");
        // the 50 % point and the required margin
        printf("speed  SNR(dB, in)  frames decoded of 20\n");
        const struct { Speed s; const char* name; const char* bw; double need; } sp[3] = {{S1200, "1200", "3 kHz", 12}, {S9600, "9600", "9.6 kHz", 15}, {S300, "300", "500 Hz", 14}};
        for (const auto& q : sp) {
            double fifty = 99;
            std::string row;
            for (double snr = 0; snr <= 22; snr += 2) {
                const int k = decodeRun(q.s, snr, 0, 20, 0, 0);
                row += " " + std::to_string((int)snr) + ":" + std::to_string(k);
                if (k >= 10 && fifty == 99) fifty = snr;
            }
            printf("%5s  (%s)%s  -> 50 %% at %.0f dB\n", q.name, q.bw, row.c_str(), fifty);
            CHECK(decodeRun(q.s, q.need, 0, 20, 0, 0) >= 19, "%s baud: 95 %% of frames at %.0f dB", q.name, q.need);
        }
    }
    // ---- the gate ----
    {
        auto fr = sample(0);
        PacketAudioCfg c;
        Rec r; HamSlotTx tx(r); TxDuty duty;
        HamTxCheck g = gate(144.800e6); g.armed = false;
        CHECK(!aprsSend(tx, duty, g, fr, c, 0.5, 0, err) && r.ptt1 == 0, "disarmed: nothing keyed");
        g = gate(144.800e6); g.licenceConfirmed = false;
        CHECK(aprsSend(tx, duty, g, fr, c, 0.5, 0, err) && r.ptt1 == 1, "no licence: a warning, sent");
        CHECK(aprsSend(tx, duty, gate(156.8e6), fr, c, 0.5, 0, err) && r.ptt1 == 2, "marine channel 16: a warning, sent (the operator decides)");
        CHECK(aprsSend(tx, duty, gate(143.995e6), fr, c, 0.5, 0, err) && r.ptt1 == 3, "FM channel over the 2 m lower edge: a warning, sent");
        CHECK(aprsSend(tx, duty, gate(144.800e6), fr, c, 0.5, 0, err) && r.ptt1 == 4 && r.ptt0 == 4 && !r.a.empty(), "144.800 armed: sent (%s)", err.c_str());
        PacketAudioCfg h; h.baud = PacketBaud::B300;
        CHECK(aprsSend(tx, duty, gate(10.1476e6 - 1700), fr, h, 0.5, 10, err) && r.ptt1 == 5, "HF 300 baud on 30 m: sent (%s)", err.c_str());
        CHECK(aprsSend(tx, duty, gate(10.1495e6), fr, h, 0.5, 20, err), "HF 300 baud over the 30 m edge: a warning, sent");
        TxDuty full; full.add(0, 300);
        CHECK(aprsSend(tx, full, gate(144.8e6), fr, c, 0.5, 300, err), "a full duty counter does not refuse (3.2)");
    }
    return testSummary("ham_aprs");
}
