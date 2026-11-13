// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW test signal (see espnow_gen.h).
#include "dect2/espnow/espnow_gen.h"
#include "dect2/espnow/espnow_proto.h"
#include "dect2/source/source.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace dect2 {

const uint8_t kEspnowTestKey[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

std::vector<WifiGenFrame> espnowTestSchedule(bool onlyDsss1) {
    std::vector<WifiGenFrame> v;
    auto add = [&](const espnow::Build& b, int phy, double gap, double amp = 1.0, double cfo = 0) {
        if (onlyDsss1 && phy != 0) return;
        WifiGenFrame f;
        f.mpdu = espnow::makeFrame(b);
        f.phy = phy; f.gapSec = gap; f.ampScale = amp; f.cfoHz = cfo;
        v.push_back(std::move(f));
    };
    auto text = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
    espnow::Build sensor;   // broadcast text, 1 Mbit/s
    sensor.body = text("temp=21.5C hum=40% bat=3.91V");
    sensor.seq = 100;
    add(sensor, 0, 0.002);
    espnow::Build button;   // unicast binary struct to a peer, 1 Mbit/s
    const uint8_t bs[6] = {0x84, 0xCC, 0xA8, 0x44, 0x55, 0x66}, bd[6] = {0x24, 0x6F, 0x28, 0x11, 0x22, 0x33};
    std::copy(bs, bs + 6, button.sa); std::copy(bd, bd + 6, button.da);
    button.body = {0x01, 0x00, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0xC3, 0xF5, 0x48, 0x40};
    button.seq = 7;
    add(button, 0, 0.0015, 0.7, 9e3);
    espnow::Build fast;     // 2 Mbit/s, long and short preamble
    const uint8_t fs[6] = {0xA4, 0xCF, 0x12, 0x01, 0x02, 0x03};
    std::copy(fs, fs + 6, fast.sa);
    for (int i = 0; i < 200; i++) fast.body.push_back((uint8_t)(i * 7 + 3));
    fast.seq = 2000;
    add(fast, 2, 0.002, 0.9, -6e3);
    fast.seq++; fast.body.resize(60);
    add(fast, 3, 0.002, 0.9, -6e3);
    espnow::Build ofdm;     // OFDM 6 and 12 Mbit/s
    const uint8_t os[6] = {0xEC, 0x94, 0xCB, 0x0A, 0x0B, 0x0C};
    std::copy(os, os + 6, ofdm.sa);
    ofdm.body = text("hello from an ESP32-C3 at 6 Mbit/s");
    ofdm.seq = 55;
    add(ofdm, 1, 0.002, 0.8);
    ofdm.body = text("and again at 12 Mbit/s"); ofdm.seq++;
    add(ofdm, 4, 0.002, 0.8);
    espnow::Build enc;      // encrypted unicast (CCMP)
    const uint8_t es[6] = {0x30, 0xAE, 0xA4, 0x77, 0x88, 0x99}, ed[6] = {0x84, 0xCC, 0xA8, 0x44, 0x55, 0x66};
    std::copy(es, es + 6, enc.sa); std::copy(ed, ed + 6, enc.da);
    enc.encrypt = true; std::memcpy(enc.key, kEspnowTestKey, 16); enc.pn = 0x1234;
    enc.body = text("secret door code 4711");
    enc.seq = 900;
    add(enc, 0, 0.002, 0.8);
    espnow::Build big;      // ESP-NOW v2: 600 bytes in three elements
    const uint8_t gs[6] = {0x24, 0x6F, 0x28, 0x11, 0x22, 0x33};
    std::copy(gs, gs + 6, big.sa);
    big.version = 2;
    for (int i = 0; i < 600; i++) big.body.push_back((uint8_t)('A' + i % 26));
    big.seq = 101;
    add(big, 0, 0.002);
    if (!onlyDsss1) {       // an access point's beacon: not ESP-NOW
        wifi::ApSpec ap;
        const uint8_t ab[6] = {0x3C, 0x37, 0x86, 0x01, 0x02, 0x03};
        std::copy(ab, ab + 6, ap.bssid);
        ap.ssid = "OnAir-Home"; ap.channel = 1;
        WifiGenFrame f; f.mpdu = wifi::makeBeacon(ap); f.gapSec = 0.002;
        v.push_back(std::move(f));
    }
    return v;
}

namespace {
class EspnowSynth : public ModeSynth {
public:
    EspnowSynth(const SynthConfig& s, double rate) : rate_(rate) {
        WifiGenConfig c;
        c.rate = rate;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.frames = espnowTestSchedule(s.modeOpt[0] == 1);
        gen_ = std::make_unique<WifiGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<WifiGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeEspnowSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<EspnowSynth>(cfg, sampleRate);
}

} // namespace dect2
