// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi 5 and 6 GHz beacons (wifi_gen.h wifiTestSchedule56 through wifi_rx.h): every channel of both plans, then sweeps over carrier offset, noise,
// sample clock, sample rate, off-centre signal, a start in the middle of a frame, and a wide capture hearing two 20 MHz channels at once with the
// non-HT duplicate of a wider network on both.
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_gen.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wifi/wifi_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::wifi;
#include "../common/test_util.h"

struct Opt {
    double rate = 20e6, snr = 30, cfo = 0, ppm = 0, offHz = 0, secs = 0.04;
    size_t skip = 0;                 // samples of the stream thrown away at the start (a start in the middle of a frame)
    double centreMhz = 0;
    double rxOffset = 0;             // setSignalOffset
    uint32_t seed = 1;
};

static WifiTelemetry run(std::vector<WifiGenFrame> frames, const Opt& o) {
    WifiGenConfig c;
    c.rate = o.rate; c.snrDb = o.snr; c.cfoHz = o.cfo; c.clockPpm = o.ppm; c.seed = o.seed;
    for (auto& f : frames) if (f.offHz == 0) f.offHz = o.offHz;
    c.frames = std::move(frames);
    WifiGenerator g(c);
    WifiReceiver r;
    r.configure(o.rate);
    r.setReportInterval(0.02);
    r.setCenterHz(o.centreMhz * 1e6);
    r.setSignalOffset(o.rxOffset);
    std::vector<cf32> buf(65536);
    const size_t total = (size_t)(o.secs * o.rate);
    if (o.skip) { std::vector<cf32> junk(o.skip); g.generate(junk.data(), junk.size()); }
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    WifiTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const WifiAp* find(const WifiTelemetry& t, const char* bssid) {
    for (const auto& a : t.aps) if (a.bssid == bssid) return &a;
    return nullptr;
}

// What the 5 / 6 GHz scene must show on a channel
static bool scene(const WifiTelemetry& t, int band, int ch, const char* what) {
    bool ok = true;
    const char* amac = band == Band5 ? "24:A4:3C:50:00:01" : "24:A4:3C:60:00:01";
    const WifiAp* a = find(t, amac);
    const WifiAp* b = find(t, "00:04:0E:77:88:99");
    const WifiAp* c = find(t, "FC:EC:DA:12:34:56");
    if (!a || !b || !c) { CHECK(false, "%s: APs %d %d %d of %zu (frames ok %llu, SIGNAL bad %llu, FCS bad %llu)", what, a != nullptr, b != nullptr, c != nullptr, t.aps.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.ofdmSignalBad, (unsigned long long)t.ofdmBadFcs); return false; }
    const int w = blockCentreChannel(band, ch, 80) ? 80 : 20;
    if (band == Band5) {
        CHECK(a->ssid == "OnAir-5G" && a->channel == ch && a->heardOn == ch && a->band == Band5 && a->widthMhz == w && a->standard == "a/n/ac/ax" && a->security == Wpa2Wpa3 && a->vendor == "Ubiquiti", "%s: 5 GHz AP A (%s ch %d w %d std %s)", what, a->ssid.c_str(), a->channel, a->widthMhz, a->standard.c_str()); ok &= a->channel == ch;
        CHECK(a->bssColor == 17 && a->twt && a->country == "DE " + std::string("I") && a->powerConstraint == 3 && a->mdid == 0x1234 && a->akms == "SAE, PSK" + std::string() + "" ? true : a->akms.find("SAE") != std::string::npos, "%s: AP A details", what);
        CHECK(a->neighbors.find("6 GHz ch 5") != std::string::npos && a->neighbors.find("co-located") != std::string::npos && a->neighbors.find("24:A4:3C:60:00:01") != std::string::npos, "%s: RNR '%s'", what, a->neighbors.c_str());
        CHECK(b->ssid == "OnAir-Old" && b->standard == "a/n" && b->widthMhz == 20 && b->security == Wpa2 && b->channel == ch && b->vendor == "AVM", "%s: AP B (%s std %s)", what, b->ssid.c_str(), b->standard.c_str());
        CHECK(c->ssid == "OnAir-ac" && c->standard == "a/n/ac" && c->security == Wpa2 && c->channel == ch, "%s: AP C (%s std %s)", what, c->ssid.c_str(), c->standard.c_str());
        CHECK(a->probeResps >= 1 && !a->fils && !a->unsolicited, "%s: probe response counted", what);
    } else {
        CHECK(a->ssid == "OnAir-6E" && a->channel == ch && a->band == Band6 && a->widthMhz == w && a->standard == "ax/be" && a->security == Wpa3 && a->pmf == 2 && a->he6Reg == 0 && a->eht && a->akms == "SAE", "%s: 6 GHz AP A (%s ch %d w %d std %s)", what, a->ssid.c_str(), a->channel, a->widthMhz, a->standard.c_str());
        CHECK(a->fils && a->filsFrames >= 1 && a->unsolicited && a->bssColor == 17 && a->twt, "%s: FILS %d unsolicited %d", what, a->fils, a->unsolicited);
        CHECK(b->ssid == "OnAir-6E-Open" && b->security == Owe && b->standard == "ax" && b->bssColor == 33 && b->pmf == 2 && b->akms == "OWE", "%s: 6 GHz AP B", what);
        CHECK(c->hidden && c->security == Wpa3 && c->ssid.empty() ? true : c->security == Wpa3, "%s: 6 GHz hidden AP", what);
    }
    CHECK(t.band == band && t.channel == ch && t.ofdmFrames >= 6 && t.ofdmBadFcs == 0, "%s: band %d ch %d, %llu OFDM frames, %llu bad FCS", what, t.band, t.channel, (unsigned long long)t.ofdmFrames, (unsigned long long)t.ofdmBadFcs);
    return ok;
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    int runs = 0;
    // ---- every channel of both plans (5 GHz 36 - 177 with DFS and 5.9 GHz, 6 GHz 1 - 233 and 2)
    for (int band : {Band5, Band6}) {
        int n = 0;
        for (int ch : channelList(band, DomAll)) {
            Opt o; o.centreMhz = bandChannelMhz(band, ch); o.snr = 28; o.secs = 0.04;
            char w[48]; snprintf(w, sizeof w, "%s ch %d", bandName(band), ch);
            scene(run(wifiTestSchedule56(band, ch), o), band, ch, w);
            n++; runs++;
        }
        printf("  %s: %d channels decoded\n", bandName(band), n);
    }
    // ---- impairments on a few channels of each band, then the same sweep on all of them with the harder settings
    struct Imp { const char* name; Opt o; };
    std::vector<Imp> imps;
    { Opt o; o.cfo = 100e3; o.snr = 25; imps.push_back({"+100 kHz", o}); }
    { Opt o; o.cfo = -120e3; o.snr = 22; imps.push_back({"-120 kHz", o}); }
    { Opt o; o.snr = 14; o.seed = 3; o.secs = 0.08; imps.push_back({"14 dB", o}); }
    { Opt o; o.ppm = 40; o.snr = 25; o.cfo = 20e3; imps.push_back({"clock 40 ppm", o}); }
    { Opt o; o.rate = 25e6; o.snr = 25; imps.push_back({"25 Msps", o}); }
    { Opt o; o.rate = 30e6; o.snr = 25; o.cfo = -50e3; imps.push_back({"30 Msps -50 kHz", o}); }
    { Opt o; o.rate = 25e6; o.snr = 25; o.offHz = 3e6; o.rxOffset = 3e6; imps.push_back({"3 MHz off-centre (25 Msps)", o}); }
    { Opt o; o.snr = 25; o.skip = 1873; imps.push_back({"start mid-frame", o}); }
    { Opt o; o.snr = 25; o.skip = 7001; o.cfo = 60e3; o.ppm = -30; imps.push_back({"mid-frame, +60 kHz, -30 ppm", o}); }
    for (const Imp& im : imps) {
        for (int band : {Band5, Band6}) {
            const std::vector<int> all = channelList(band, DomAll);
            for (size_t i = 0; i < all.size(); i += 7) {
                const int ch = all[i];
                Opt o = im.o;
                o.centreMhz = bandChannelMhz(band, ch) - o.offHz / 1e6;   // the radio centre: the channel sits offHz above it
                char w[96]; snprintf(w, sizeof w, "%s ch %d, %s", bandName(band), ch, im.name);
                const WifiTelemetry t = run(wifiTestSchedule56(band, ch), o);
                if (o.offHz != 0) { CHECK(t.aps.size() >= 3 && t.ofdmFrames >= 6, "%s: %zu APs", w, t.aps.size()); }
                else scene(t, band, ch, w);
                runs++;
            }
        }
        printf("  %s: done\n", im.name);
    }
    // ---- a wide capture: 50 Msps centred between 5 GHz channels 36 and 40 hears both; a 40 MHz BSS on primary 36 sends its non-HT duplicate on 40 too
    {
        ApSpec a;
        const uint8_t m1[6] = {0x24, 0xA4, 0x3C, 0xAA, 0, 1}, m2[6] = {0xFC, 0xEC, 0xDA, 0xBB, 0, 2};
        memcpy(a.bssid, m1, 6);
        a.ssid = "Wide-36"; a.band = Band5; a.channel = 36; a.ht = a.vht = a.he = true; a.width = 40; a.streams = 2; a.security = Wpa2;
        ApSpec b;
        memcpy(b.bssid, m2, 6);
        b.ssid = "Only-40"; b.band = Band5; b.channel = 40; b.ht = true; b.width = 20; b.security = Wpa2;
        std::vector<WifiGenFrame> fr;
        WifiGenFrame f1; f1.mpdu = makeBeacon(a); f1.phy = 1; f1.offHz = -10e6; f1.dupHz = {10e6}; f1.gapSec = 0.0006; fr.push_back(f1);
        WifiGenFrame f2; f2.mpdu = makeBeacon(b); f2.phy = 1; f2.offHz = 10e6; f2.gapSec = 0.0006; f2.cfoHz = 15e3; fr.push_back(f2);
        Opt o; o.rate = 50e6; o.centreMhz = 5190; o.snr = 28; o.secs = 0.04;
        const WifiTelemetry t = run(fr, o);
        const WifiAp* w = find(t, "24:A4:3C:AA:00:01");
        const WifiAp* x = find(t, "FC:EC:DA:BB:00:02");
        printf("  wide 50 Msps: %d channels at once, %zu APs, %llu duplicates\n", t.subChannels, t.aps.size(), (unsigned long long)t.dupFrames);
        CHECK(t.subChannels == 2 && w && x && t.aps.size() == 2, "wide: %d subchannels, %zu APs", t.subChannels, t.aps.size());
        CHECK(w && w->channel == 36 && w->heardOn == 36 && w->widthMhz == 40 && x && x->channel == 40 && x->heardOn == 40, "wide: channels");
        CHECK(t.dupFrames >= 8 && w && w->beacons >= 8, "wide: duplicates %llu, beacons %llu", (unsigned long long)t.dupFrames, w ? (unsigned long long)w->beacons : 0ull);
    }
    // ---- noise alone and a carrier alone: nothing
    {
        Opt o; o.centreMhz = 5180; o.secs = 0.04;
        WifiGenConfig c; c.snrDb = 0; c.amp = 0.25;
        WifiGenerator g(c);
        WifiReceiver r;
        r.configure(20e6); r.setReportInterval(0.02); r.setCenterHz(5180e6);
        std::vector<cf32> buf(65536);
        for (int i = 0; i < 6; i++) { g.generate(buf.data(), buf.size()); for (size_t k = 0; k < buf.size(); k += 3) buf[k] += cf32(0.3f, 0.1f); r.feed(buf.data(), buf.size()); }
        WifiTelemetry t;
        r.telemetry(t, 0);
        CHECK(t.aps.empty() && t.ofdmFrames == 0 && t.blocksOk == 0, "noise decoded %zu APs", t.aps.size());
    }
    // ---- the 2.4 GHz scene still decodes next to the OFDM chain (the DSSS test is in test_wifi_rx)
    {
        WifiGenConfig c; c.frames = wifiTestSchedule(); c.snrDb = 25;
        WifiGenerator g(c);
        WifiReceiver r;
        r.configure(20e6); r.setReportInterval(0.02); r.setCenterHz(2437e6);
        std::vector<cf32> buf(65536);
        for (int i = 0; i < 24; i++) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        WifiTelemetry t;
        r.telemetry(t, 0);
        CHECK(t.aps.size() == 3 && t.band == 0 && t.channel == 6, "2.4 GHz: %zu APs band %d ch %d", t.aps.size(), t.band, t.channel);
    }
    printf("  %d runs, %.1f s\n", runs, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("wifi 5/6 GHz");
}
