// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) monitor: the test signal through the receiver (halow_gen.h, halow_rx.h): the whole schedule, every width alone near
// the threshold, carrier offsets of +-30 kHz (+-33 ppm at 915 MHz), sample clock error, other sample rates, a signal off the centre, a wrong
// FCS, back-to-back PPDUs, noise alone, a continuous tone, and the results file.
#include "dect2/dsp/gen_util.h"
#include "dect2/halow/halow_gen.h"
#include "dect2/halow/halow_proto.h"
#include "dect2/halow/halow_rx.h"
#include "dect2/halow/halow_tel.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::halow;
#include "../common/test_util.h"

static HalowTelemetry run(HalowGenConfig c, double secs, double centerHz = 0) {
    HalowGenerator g(c);
    HalowReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(c.dialOffsetHz);
    r.setCenterHz(centerHz);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)((secs + 0.3) * c.rate);   // the receiver reports every 0.25 s of signal
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    HalowTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static std::vector<uint8_t> beaconFor(const char* ssid, int w, int sec = Wpa2Psk, int ccf = 3) {
    BssSpec b;
    const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x12, 0x34, (uint8_t)(0x50 + w)};
    std::copy(mac, mac + 6, b.bssid);
    b.ssid = ssid; b.security = sec; b.bwInd = w; b.ccf = ccf; b.primary = ccf; b.opClass = 6; b.opWidthMhz = 1 << w;
    return makeS1gBeacon(b);
}

static HalowGenFrame one(int w, int mcs, double gap = 0.003, double cfo = 0) {
    HalowGenFrame f;
    f.psdu = beaconFor("Test-Net", w);
    f.width = w; f.mcs = mcs; f.gapSec = gap; f.cfoHz = cfo;
    return f;
}

// PPDUs the schedule sends in this time
static double sentIn(const HalowGenConfig& c, double secs) {
    double per = 0;
    for (const auto& f : c.frames) per += (double)ppduSamples(f.width, f.mcs, (int)f.psdu.size()) / kRate + f.gapSec + 130 / kRate;
    return secs / per * (double)c.frames.size();
}

static const HlNet* findNet(const HalowTelemetry& t, const char* ssid) { for (const auto& n : t.nets) if (n.ssid == ssid) return &n; return nullptr; }

int main() {
    {   // the whole schedule, clean: every network, every width, security, channels, the compressed SSID resolved by a probe response
        HalowGenConfig c;
        c.snrDb = 30;
        c.frames = halowTestSchedule();
        const HalowTelemetry t = run(c, 1.0, 863.5e6);
        printf("  schedule: %llu good, %llu bad, %llu undecoded, %zu networks, SIG %llu ok %llu bad, SNR %.1f dB\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.undecoded, t.nets.size(),
               (unsigned long long)t.sigOk, (unsigned long long)t.sigBad, t.snrDb);
        CHECK(t.blocksBad == 0 && t.sigBad == 0, "%llu bad FCS, %llu bad SIG", (unsigned long long)t.blocksBad, (unsigned long long)t.sigBad);
        const double sent = sentIn(c, 1.0);
        CHECK((double)(t.blocksOk + t.undecoded) > 0.9 * sent, "%llu frames of about %.0f", (unsigned long long)(t.blocksOk + t.undecoded), sent);
        CHECK(t.nets.size() == 4, "%zu networks", t.nets.size());
        const HlNet* home = findNet(t, "HaLow-Home");
        const HlNet* farm = findNet(t, "Farm-Sensors");
        const HlNet* cam = findNet(t, "OpenCam");
        const HlNet* meter = findNet(t, "Meter-Net");
        CHECK(home && home->security == "WPA3-SAE" && home->seenWidthMhz == 1 && home->opWidthMhz == 1 && home->freqMhz == 864.5 && home->ccf == 3, "home network");
        CHECK(farm && farm->security == "WPA2-PSK" && farm->seenWidthMhz == 2 && farm->freqMhz == 866.0 && farm->intervalTu == 200, "farm network");
        CHECK(cam && cam->security == "Open" && cam->seenWidthMhz == 4 && cam->opWidthMhz == 4 && cam->freqMhz == 904.0, "camera network");
        CHECK(meter && meter->hasSsid && meter->comp != 0 && meter->beacons > 0, "the compressed SSID was not resolved");
        CHECK(home && home->vendor == "Espressif" && cam && cam->vendor == "MikroTik", "vendors");
        CHECK(t.perWidth[0] > 0 && t.perWidth[1] > 0 && t.perWidth[2] > 0, "widths %llu %llu %llu", (unsigned long long)t.perWidth[0], (unsigned long long)t.perWidth[1], (unsigned long long)t.perWidth[2]);
        CHECK(t.undecoded > 0, "the MCS 3 PPDU was not counted");
        CHECK(t.chanHint == 1 && t.centerMhz == 863.5, "channel hint %d, %.1f MHz", t.chanHint, t.centerMhz);
        CHECK(!t.log.empty() && t.log.size() <= kHlLogMax, "frame log %zu", t.log.size());
        CHECK(t.levelDb > -30 && t.levelDb < 0, "level %.1f dBFS", t.levelDb);
        CHECK(home && home->rssi > -30 && home->rssi < 0, "network level %.1f", home ? home->rssi : 0.f);
    }
    {   // each width alone, near the threshold, MCS 0 and 1 MHz MCS 10
        struct Case { int w, mcs; double snr; const char* name; };
        const Case cases[] = {{W1, 0, 10, "1 MHz MCS 0"}, {W1, 10, 9, "1 MHz MCS 10"}, {W2, 0, 11, "2 MHz"}, {W4, 0, 14, "4 MHz"}};
        for (const Case& k : cases) {
            HalowGenConfig c;
            c.snrDb = k.snr;
            c.frames = {one(k.w, k.mcs)};
            const HalowTelemetry t = run(c, 1.0);
            const double sent = sentIn(c, 1.0);
            printf("  %s at %.0f dB (1 MHz reference): %llu of %.0f, %llu bad FCS, SIG %llu ok / %llu bad\n", k.name, k.snr, (unsigned long long)t.blocksOk, sent, (unsigned long long)t.blocksBad, (unsigned long long)t.sigOk, (unsigned long long)t.sigBad);
            CHECK((double)t.blocksOk > 0.85 * sent, "%s: %llu of %.0f", k.name, (unsigned long long)t.blocksOk, sent);
            CHECK(t.nets.size() == 1 && t.nets[0].ssid == "Test-Net" && t.nets[0].seenWidthMhz == (1 << k.w), "%s: network", k.name);
        }
    }
    {   // carrier offsets
        for (int w = 0; w < 3; w++) for (double cfo : {-30e3, -12e3, 20e3, 30e3}) {
            HalowGenConfig c;
            c.snrDb = 28;
            c.cfoHz = cfo;
            c.frames = {one(w, 0)};
            const HalowTelemetry t = run(c, 0.6);
            const double sent = sentIn(c, 0.6);
            CHECK((double)t.blocksOk > 0.9 * sent && t.blocksBad == 0, "width %d, offset %+.0f kHz: %llu of %.0f (%llu bad)", 1 << w, cfo / 1e3, (unsigned long long)t.blocksOk, sent, (unsigned long long)t.blocksBad);
            CHECK(t.blocksOk == 0 || std::fabs(t.cfoHz - cfo) < 1500, "width %d: offset %.0f read as %.0f Hz", 1 << w, cfo, t.cfoHz);
        }
    }
    {   // the sample clock: +-40 ppm over a long PPDU (a 400 byte frame at 1 MHz MCS 10 is 23 ms)
        for (double ppm : {-40.0, 40.0}) for (int w = 0; w < 3; w++) {
            HalowGenConfig c;
            c.snrDb = 28;
            c.clockPpm = ppm;
            c.frames = {one(w, w == W1 ? 10 : 0)};
            { const uint8_t bs[6] = {1, 2, 3, 4, 5, 6}, st[6] = {7, 8, 9, 10, 11, 12}; c.frames[0].psdu = makeDataFrame(bs, st, 300); }
            const HalowTelemetry t = run(c, 0.6);
            const double sent = sentIn(c, 0.6);
            CHECK((double)(t.blocksOk + t.blocksBad) > 0.9 * sent, "width %d, %+.0f ppm: %llu good %llu bad of %.0f", 1 << w, ppm, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, sent);
            CHECK(t.blocksBad == 0 && t.blocksOk > 0, "width %d, %+.0f ppm: %llu bad FCS", 1 << w, ppm, (unsigned long long)t.blocksBad);
        }
    }
    {   // other sample rates: 2.4 Msps (an RTL-SDR: 1 and 2 MHz only), 5, 8 and 10 Msps
        for (double rate : {2.4e6, 2.0e6, 5e6, 8e6, 10e6}) {
            HalowGenConfig c;
            c.rate = rate;
            c.snrDb = 28;
            c.frames = {one(W1, 0), one(W2, 0)};
            if (rate >= 4e6) c.frames.push_back(one(W4, 0));
            const HalowTelemetry t = run(c, 0.5);
            const double sent = sentIn(c, 0.5);
            printf("  %.1f Msps: %llu of %.0f\n", rate / 1e6, (unsigned long long)t.blocksOk, sent);
            CHECK((double)t.blocksOk > 0.8 * sent, "%.1f Msps: %llu of %.0f", rate / 1e6, (unsigned long long)t.blocksOk, sent);
        }
    }
    {   // the signal off the centre, as the engine tunes it
        HalowGenConfig c;
        c.snrDb = 28;
        c.dialOffsetHz = -400e3;
        c.frames = {one(W1, 0), one(W2, 0)};
        const HalowTelemetry t = run(c, 0.5, 864.5e6 - c.dialOffsetHz);
        CHECK((double)t.blocksOk > 0.9 * sentIn(c, 0.5), "off centre: %llu", (unsigned long long)t.blocksOk);
    }
    {   // a wrong FCS is counted and never makes a network
        HalowGenConfig c;
        c.snrDb = 30;
        c.frames = {one(W2, 0)};
        c.frames[0].badFcs = true;
        const HalowTelemetry t = run(c, 0.5);
        CHECK(t.blocksOk == 0 && t.blocksBad > 20 && t.nets.empty(), "%llu good %llu bad %zu networks", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.nets.size());
    }
    {   // PPDUs back to back (a gap of 100 us)
        HalowGenConfig c;
        c.snrDb = 30;
        c.frames = {one(W2, 0, 0.0001), one(W1, 0, 0.0001, 5e3), one(W4, 0, 0.0001, -5e3)};
        const HalowTelemetry t = run(c, 0.5);
        CHECK((double)t.blocksOk > 0.85 * sentIn(c, 0.5), "back to back: %llu of %.0f", (unsigned long long)t.blocksOk, sentIn(c, 0.5));
    }
    {   // noise alone, and a strong continuous tone: nothing decoded, nothing listed
        HalowGenConfig c;
        c.snrDb = 0;
        const HalowTelemetry t = run(c, 1.5);
        CHECK(t.blocksOk == 0 && t.nets.empty() && t.sigOk == 0, "noise alone gave %llu frames", (unsigned long long)t.blocksOk);
        HalowReceiver r;
        r.configure(4e6);
        std::vector<cf32> buf(16384);
        double ph = 0;
        for (int k = 0; k < 80; k++) {
            for (auto& v : buf) { ph += 2 * 3.14159265358979 * 100e3 / 4e6; v = cf32((float)std::cos(ph) * 0.3f, (float)std::sin(ph) * 0.3f); }
            genutil::NoiseSource ns(3 + k);
            ns.add(buf.data(), buf.size(), 0.01f);
            r.feed(buf.data(), buf.size());
        }
        HalowTelemetry tt;
        r.telemetry(tt, 0);
        CHECK(tt.blocksOk == 0 && tt.nets.empty() && tt.sigOk == 0, "a tone gave %llu frames", (unsigned long long)tt.blocksOk);
        CHECK(tt.preambles < 5, "a tone gave %llu detections", (unsigned long long)tt.preambles);
    }
    {   // too slow a radio, and the results file
        HalowReceiver r;
        r.configure(1e6);
        CHECK(!r.ready(), "1 Msps is not enough");
        HalowGenConfig c;
        c.snrDb = 30;
        c.frames = halowTestSchedule();
        HalowGenerator g(c);
        HalowReceiver a;
        a.configure(c.rate);
        std::vector<cf32> buf(16384);
        for (int k = 0; k < 90; k++) { g.generate(buf.data(), buf.size()); a.feed(buf.data(), buf.size()); }
        const std::string blob = a.saveResults();
        HalowReceiver b;
        b.configure(c.rate);
        CHECK(b.loadResults(blob), "load");
        HalowTelemetry ta, tb;
        a.telemetry(ta, 0); b.telemetry(tb, 0);
        CHECK(!ta.nets.empty() && ta.nets.size() == tb.nets.size() && ta.log.size() == tb.log.size() && tb.nets[0].bssid == ta.nets[0].bssid, "saved %zu networks, loaded %zu", ta.nets.size(), tb.nets.size());
        CHECK(!b.loadResults("garbage"), "garbage accepted");
        a.clearResults();
        a.telemetry(ta, ta.seq);
        CHECK(ta.nets.empty() && ta.log.empty(), "clear");
    }
    printf(fails ? "halow rx: %d FAILED\n" : "halow rx: all passed\n", fails);
    return fails ? 1 : 0;
}
