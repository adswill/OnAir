// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP32 radio loopback: OnAir's driver (esp32_radio) talks over an in-memory serial pair to the firmware's own dispatcher (onair_core.h) running on a fake
// hardware layer. Proves the protocol and the logic on both sides; it does not prove the radio: there was no ESP32 or nRF24 module to try (docs/ESP32-NRF24-DESIGN.md).
#include "dect2/esp32/esp32_radio.h"
#include "../../firmware/esp32/onair_esp32/onair_core.h"
#include "dect2/nrf24/nrf24_proto.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
using namespace dect2;
using namespace dect2::esp32;
#include "../common/test_util.h"

namespace w = onair::wire;
using Bytes = std::vector<uint8_t>;

struct FakeHal : onair::Hal {
    std::mutex mu;
    std::deque<std::pair<uint8_t, Bytes>> rxq;
    Bytes nrfCfg, wifiCfg, bleCfg;
    std::vector<Bytes> txs;
    int started = 0, stops = 0;
    uint8_t mask = w::kBitNrf24 | w::kBitBle | w::kBitWifi;
    std::function<void(const uint8_t*, size_t)> out;
    uint32_t millis() override { return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void serialWrite(const uint8_t* p, size_t n) override { out(p, n); }
    uint8_t chip() override { return w::kChipSimulator; }
    uint8_t radios() override { return mask; }
    const char* name() override { return "Simulated ESP32"; }
    uint8_t configure(uint8_t r, const uint8_t* p, size_t n) override {
        std::lock_guard<std::mutex> l(mu);
        (r == w::kRadioNrf24 ? nrfCfg : r == w::kRadioWifi ? wifiCfg : bleCfg).assign(p, p + n);
        return w::kOk;
    }
    uint8_t start(uint8_t) override { std::lock_guard<std::mutex> l(mu); started++; return w::kOk; }
    void stopAll() override { std::lock_guard<std::mutex> l(mu); stops++; }
    uint8_t transmit(uint8_t r, const uint8_t* p, size_t n, uint8_t& res) override {
        std::lock_guard<std::mutex> l(mu);
        Bytes b(p, p + n); b.insert(b.begin(), r); txs.push_back(b); res = 0; return w::kOk;
    }
    bool pollRx(uint8_t& r, uint8_t* pl, size_t& len, size_t) override {
        std::lock_guard<std::mutex> l(mu);
        if (rxq.empty()) return false;
        r = rxq.front().first; len = rxq.front().second.size(); memcpy(pl, rxq.front().second.data(), len); rxq.pop_front();
        return true;
    }
    void inject(uint8_t r, const Bytes& b) { std::lock_guard<std::mutex> l(mu); rxq.push_back({r, b}); }
};

// host end of the pair; fault injection on the way back: garbage before frames, one bad frame, one byte at a time
struct Pair : SerialPort {
    FakeHal hal;
    onair::Core core{hal};
    std::mutex cmu, qmu;
    std::deque<uint8_t> q;
    std::function<void(const uint8_t*, size_t)> tap;   // set before the device thread writes (under qmu)
    std::condition_variable qcv;
    std::atomic<bool> stop{false}, garbage{false}, trickle{false}, corruptNext{false};
    std::thread dev;
    bool closed = false;
    Pair() {
        hal.out = [this](const uint8_t* p, size_t n) {
            std::lock_guard<std::mutex> l(qmu);
            if (tap) { tap(p, n); return; }
            if (garbage) { const uint8_t g[] = {0x00, 0xA5, 0xFF, 0xA5, 0x03, 0x00, 0x42}; q.insert(q.end(), g, g + sizeof g); }
            Bytes b(p, p + n);
            if (corruptNext.exchange(false) && b.size() > 8) b[7] ^= 0x40;   // one frame with a damaged payload byte
            q.insert(q.end(), b.begin(), b.end());
            qcv.notify_all();
        };
        dev = std::thread([this] { while (!stop) { { std::lock_guard<std::mutex> l(cmu); core.poll(); } std::this_thread::sleep_for(std::chrono::milliseconds(1)); } });
    }
    ~Pair() override { stop = true; if (dev.joinable()) dev.join(); }
    bool write(const uint8_t* p, size_t n) override { std::lock_guard<std::mutex> l(cmu); for (size_t i = 0; i < n; i++) core.onByte(p[i]); return true; }
    int read(uint8_t* p, size_t n, int ms) override {
        std::unique_lock<std::mutex> l(qmu);
        if (closed) return -1;
        qcv.wait_for(l, std::chrono::milliseconds(ms), [&] { return !q.empty() || closed; });
        if (closed) return -1;
        size_t k = 0, lim = trickle ? 1 : n;
        while (!q.empty() && k < n && k < lim) { p[k++] = q.front(); q.pop_front(); }
        return (int)k;
    }
    void close() override { std::lock_guard<std::mutex> l(qmu); closed = true; qcv.notify_all(); }
    std::string name() const override { return "loopback"; }
};

static void waitFor(const std::function<bool()>& f, int ms = 2000) {
    for (int t = 0; t < ms && !f(); t++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
static void p32(Bytes& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
static Bytes wifiPkt(int rssi, int ch, int kind, const uint8_t* mac, const uint8_t* bssid, int reason, int auth, const char* ssid) {
    Bytes v; p32(v, 1000); v.push_back((uint8_t)rssi); v.push_back((uint8_t)ch); v.push_back((uint8_t)kind);
    v.insert(v.end(), mac, mac + 6); v.insert(v.end(), bssid, bssid + 6); v.push_back(reason & 0xFF); v.push_back(reason >> 8); v.push_back((uint8_t)auth);
    v.push_back((uint8_t)strlen(ssid)); v.insert(v.end(), ssid, ssid + strlen(ssid));
    return v;
}

int main() {
    // ---- the framing alone
    {
        uint8_t f[w::kMaxFrame]; const uint8_t pl[] = {1, 2, 3};
        const size_t n = w::encode(0x42, 7, pl, 3, f);
        w::Parser p; bool got = false;
        for (size_t i = 0; i < n; i++) got = p.feed(f[i]);
        CHECK(got && p.type() == 0x42 && p.seq() == 7 && p.length() == 3 && p.payload()[2] == 3, "frame round trip");
        Bytes noisy = {0x11, 0xA5, 0x09, 0x09, 0xA5};   // an A5 with an impossible length, then a good frame
        noisy.insert(noisy.end(), f, f + n);
        w::Parser q; int frames = 0;
        for (uint8_t b : noisy) if (q.feed(b)) frames++;
        CHECK(frames == 1, "resync after garbage: %d frames", frames);
        f[6] ^= 1;
        w::Parser r; frames = 0;
        for (size_t i = 0; i < n; i++) if (r.feed(f[i])) frames++;
        CHECK(frames == 0 && r.badFrames() == 1, "a damaged frame is dropped and counted");
        CHECK(w::crc16((const uint8_t*)"123456789", 9) == 0x29B1, "CRC-16/CCITT-FALSE check value");
    }
    // ---- handshake, version, transmit gate
    Pair* P = new Pair;
    Radio R;
    std::string err;
    P->trickle = true;   // the host sees the device's bytes one at a time
    CHECK(R.connect(std::unique_ptr<SerialPort>(P), err), "connect: %s", err.c_str());
    P->trickle = false;
    Snapshot s = R.snapshot();
    CHECK(s.info.connected && s.info.protoVersion == 1 && s.info.name == "Simulated ESP32" && s.info.radios == (w::kBitNrf24 | w::kBitBle | w::kBitWifi) && s.info.chip == w::kChipSimulator,
          "hello answer: name %s radios %d", s.info.name.c_str(), s.info.radios);
    CHECK(!R.sendNrf24({1, 2, 3}, false, err) && err.find("Arm") != std::string::npos, "transmit refused until armed: %s", err.c_str());
    CHECK(P->hal.txs.empty(), "nothing reached the transmitter");

    // ---- every nRF24 parameter reaches the (fake) chip; out-of-range values are refused
    Nrf24Params np;
    np.channel = 76; np.rateKbps = 250; np.addrWidth = 4; const uint8_t ad[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01}; memcpy(np.address, ad, 5);
    np.crcBits = 8; np.autoAck = false; np.retryDelay = 5; np.retryCount = 9; np.payloadLen = 12; np.power = 1; np.promiscuous = false;
    CHECK(R.configure(np, err), "configure: %s", err.c_str());
    {
        const Bytes want = {76, 0, 4, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 1, 0, 5, 9, 12, 1, 0};
        std::lock_guard<std::mutex> l(P->hal.mu);
        CHECK(P->hal.nrfCfg == want, "all nRF24 parameters arrive (%zu bytes)", P->hal.nrfCfg.size());
    }
    Nrf24Params bad = np; bad.channel = 126;
    CHECK(!R.configure(bad, err) && err.find("bad parameters") != std::string::npos, "channel 126 refused: %s", err.c_str());
    bad = np; bad.addrWidth = 2;
    CHECK(!R.configure(bad, err), "address width 2 refused");
    bad = np; bad.payloadLen = 33;
    CHECK(!R.configure(bad, err), "payload 33 refused");
    for (int rate : {250, 1000, 2000}) for (int crc : {0, 8, 16}) for (int aw = 3; aw <= 5; aw++) {
        Nrf24Params q = np; q.rateKbps = rate; q.crcBits = crc; q.addrWidth = aw; q.payloadLen = 0; q.channel = 125;
        CHECK(R.configure(q, err), "combo %d/%d/%d: %s", rate, crc, aw, err.c_str());
    }
    CHECK(R.start(w::kRadioNrf24, err), "start nrf24: %s", err.c_str());
    CHECK(P->hal.started == 1, "the chip was started");

    // ---- receive: a decoded nRF24 packet and a raw (promiscuous) one
    {
        Bytes v; p32(v, 5000); v.push_back(0); v.push_back(2); v.push_back(40); v.push_back(5); v.push_back(2); v.push_back(1); v.push_back(16); v.push_back(5);
        const uint8_t a[5] = {1, 2, 3, 4, 5}; v.insert(v.end(), a, a + 5); const char* t = "hello"; v.insert(v.end(), t, t + 5);
        P->hal.inject(w::kRadioNrf24, v);
        waitFor([&] { return !R.snapshot().nrf.frames.empty(); });
        s = R.snapshot();
        CHECK(s.nrf.frames.size() == 1 && s.nrf.frames[0].address == "0102030405" && s.nrf.frames[0].text == "hello" && s.nrf.frames[0].rateKbps == 2000 && s.nrf.frames[0].pid == 2 &&
              s.nrf.frames[0].noAck && s.nrf.frames[0].crcBits == 16, "decoded nRF24 frame: %zu", s.nrf.frames.size());
        CHECK(s.nrf.devices.size() == 1 && s.nrf.devices[0].count == 1, "address list");
        P->hal.inject(w::kRadioNrf24, v);
        waitFor([&] { return R.snapshot().nrf.frames.size() ? R.snapshot().nrf.frames[0].repeats == 2 : false; });
        CHECK(R.snapshot().nrf.frames[0].repeats == 2, "a repeat is counted, not listed twice");
    }
    for (int addrFirst : {0xAA, 0x55}) {   // raw bytes after the chip's match: the sniffer finds the frame in them
        nrf24::Frame f; f.address = {(uint8_t)addrFirst, 0x11, 0x22, 0x33}; f.pid = 1; f.crcBits = 16; f.payload = {'O', 'n', 'A', 'i', 'r'};
        const auto bits = nrf24::encode(f);   // preamble first
        Bytes raw(32, 0);
        uint32_t lf = 0x1234567; for (auto& x : raw) { lf = lf * 1103515245u + 12345u; x = (uint8_t)(lf >> 16); }   // the bytes after a frame are noise, not zeros
        for (size_t i = 8; i < bits.size() && (i - 8) / 8 < 32; i++) raw[(i - 8) / 8] &= (uint8_t)~(1 << (7 - ((i - 8) % 8)));
        for (size_t i = 8; i < bits.size() && (i - 8) / 8 < 32; i++) raw[(i - 8) / 8] |= (uint8_t)(bits[i] << (7 - ((i - 8) % 8)));
        Bytes v; p32(v, 6000); v.push_back(1); v.push_back(1); v.push_back(40); v.push_back(0); v.push_back(0); v.push_back(0); v.push_back(0); v.push_back(32);
        v.insert(v.end(), raw.begin(), raw.end());
        const size_t before = R.snapshot().nrf.devices.size();
        P->hal.inject(w::kRadioNrf24, v);
        waitFor([&] { return R.snapshot().nrf.devices.size() > before; });
        s = R.snapshot();
        CHECK(s.nrf.devices.size() == before + 1 && s.nrf.devices[0].addrLen == 4 && s.nrf.frames[0].text == "OnAir", "promiscuous raw bytes decoded by the host (first byte %02X)", addrFirst);
    }

    // ---- BLE
    {
        Bytes v; p32(v, 7000); v.push_back((uint8_t)-60); v.push_back(37); v.push_back(0);
        const uint8_t a[6] = {0xC0, 0x11, 0x22, 0x33, 0x44, 0x55}; v.insert(v.end(), a, a + 6);
        const Bytes d = {2, 1, 6, 6, 9, 'T', 'a', 'g', '0', '1'}; v.push_back((uint8_t)d.size()); v.insert(v.end(), d.begin(), d.end());
        P->hal.inject(w::kRadioBle, v);
        waitFor([&] { return !R.snapshot().ble.empty(); });
        s = R.snapshot();
        CHECK(s.ble.size() == 1 && s.ble[0].addr == "C0:11:22:33:44:55" && s.ble[0].name == "Tag01" && s.ble[0].rssi == -60, "BLE advertisement");
    }

    // ---- WiFi: networks, clients, deauth counted, no transmit
    {
        BleParams bp; CHECK(R.configure(bp, err), "ble config: %s", err.c_str());
        WifiParams wp; wp.scan = false; wp.channel = 0; wp.hopMs = 300;
        CHECK(R.configure(wp, err), "wifi config: %s", err.c_str());
        { std::lock_guard<std::mutex> l(P->hal.mu); CHECK(P->hal.wifiCfg == Bytes({0, 0, 0x2C, 0x01}), "wifi params arrive"); }
        const uint8_t ap1[6] = {0x14, 0xCC, 0x20, 1, 2, 3}, ap2[6] = {0x00, 0x03, 0x93, 9, 9, 9}, cl[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE}, cl2[6] = {0x00, 0x03, 0x93, 5, 5, 5};
        P->hal.inject(w::kRadioWifi, wifiPkt(-50, 6, 0, ap1, ap1, 0, 3, "HomeNet"));
        P->hal.inject(w::kRadioWifi, wifiPkt(-70, 6, 0, ap2, ap2, 0, 0, "CafeFree"));
        P->hal.inject(w::kRadioWifi, wifiPkt(-45, 6, 0, ap1, ap1, 0, 3, "HomeNet"));
        P->hal.inject(w::kRadioWifi, wifiPkt(-80, 11, 0, cl2, cl2, 0, 5, "Far5"));
        P->hal.inject(w::kRadioWifi, wifiPkt(-60, 6, 1, cl, cl, 0, 255, "PhoneHotspot"));
        P->hal.inject(w::kRadioWifi, wifiPkt(-55, 6, 5, cl2, ap1, 0, 255, ""));
        P->hal.inject(w::kRadioWifi, wifiPkt(-40, 6, 3, ap1, ap1, 7, 255, ""));
        waitFor([&] { return R.snapshot().wifi.deauths == 1; });
        s = R.snapshot();
        const WifiSnapshot& W = s.wifi;
        CHECK(W.aps.size() == 3 && W.aps[0].ssid == "HomeNet" && W.aps[0].vendor == "TP-Link" && W.aps[0].auth == 3 && W.aps[0].count == 2 && W.aps[0].history.size() == 2 && W.aps[0].rssi == -45,
              "access points: %zu, first %s", W.aps.size(), W.aps.empty() ? "" : W.aps[0].ssid.c_str());
        CHECK(authName(W.aps[0].auth) == "WPA2" && authName(0) == "open" && authName(5) == "WPA3", "security names");
        CHECK(W.chanAps[6] == 2 && W.chanAps[11] == 1 && W.chanBestDb[6] == -40, "channel congestion: ch6 %d networks, ch11 %d", W.chanAps[6], W.chanAps[11]);
        CHECK(W.clients.size() == 2 && W.beacons == 4 && W.probes == 1 && W.dataFrames == 1 && W.deauths == 1, "counters: beacons %llu probes %llu data %llu deauth %llu clients %zu",
              (unsigned long long)W.beacons, (unsigned long long)W.probes, (unsigned long long)W.dataFrames, (unsigned long long)W.deauths, W.clients.size());
        bool okc = false, rnd = false;
        for (const auto& c : W.clients) { if (c.mac == "00:03:93:05:05:05" && c.bssid == "14:CC:20:01:02:03" && c.vendor == "Apple") okc = true; if (c.randomised && !c.probed.empty() && c.probed[0] == "PhoneHotspot") rnd = true; }
        CHECK(okc && rnd && W.aps[0].clients == 1, "client list: associated Apple client, randomised phone probing a name");
    }
    // the firmware refuses WiFi transmit even when armed (core directly)
    {
        Pair Q; Bytes got;
        { std::lock_guard<std::mutex> l(Q.qmu); Q.tap = [&](const uint8_t* p, size_t n) { got.insert(got.end(), p, p + n); }; }
        auto send = [&](uint8_t type, const Bytes& pl) { uint8_t f[w::kMaxFrame]; const size_t n = w::encode(type, 1, pl.data(), pl.size(), f); Q.write(f, n); };
        send(w::kHello, {1}); send(w::kTxEnable, {1}); { std::lock_guard<std::mutex> l(Q.qmu); got.clear(); }
        send(w::kTx, {w::kRadioWifi, 0, 0});
        w::Parser p; uint8_t status = 99;
        for (uint8_t b : got) if (p.feed(b) && p.type() == w::kAck) status = p.payload()[1];
        CHECK(status == w::kUnsupported && Q.hal.txs.empty(), "WiFi transmit refused by the firmware even when armed (status %d)", status);
        got.clear(); send(w::kHello, {0});
        w::Parser p2; status = 99;
        for (uint8_t b : got) if (p2.feed(b) && p2.type() == w::kAck) status = p2.payload()[1];
        CHECK(status == w::kBadVersion, "protocol version 0 is refused");
        Q.stop = true;
    }

    // ---- transmit: Arm, then the bytes arrive exactly; BLE advertising; a bad length is refused on the host
    CHECK(R.setTxEnable(true, err), "arm: %s", err.c_str());
    CHECK(R.sendNrf24({0xDE, 0xAD, 0xBE, 0xEF}, true, err), "send: %s", err.c_str());
    waitFor([&] { return R.lastTxResult() == 0; });
    {
        std::lock_guard<std::mutex> l(P->hal.mu);
        CHECK(P->hal.txs.size() == 1 && P->hal.txs[0] == Bytes({w::kRadioNrf24, 1, 4, 0xDE, 0xAD, 0xBE, 0xEF}), "TX bytes reach the chip with the no-ack flag");
    }
    CHECK(R.lastTxResult() == 0, "TX_DONE result seen");
    CHECK(!R.sendNrf24(Bytes(33, 0), false, err), "33 bytes refused: %s", err.c_str());
    const uint8_t baddr[6] = {1, 2, 3, 4, 5, 6};
    CHECK(R.sendBle(3, baddr, {2, 1, 6}, err), "ble advertise: %s", err.c_str());
    CHECK(R.setTxEnable(false, err) && !R.sendNrf24({1}, false, err), "disarm closes the gate");

    // ---- damaged and noisy line
    P->garbage = true; P->corruptNext = true;
    P->hal.inject(w::kRadioBle, Bytes());   // an empty (invalid) packet is ignored by the host, and consumes the damaged frame
    {
        Bytes v; p32(v, 8000); v.push_back((uint8_t)-70); v.push_back(38); v.push_back(0); for (int i = 0; i < 6; i++) v.push_back((uint8_t)(0x20 + i)); v.push_back(0);
        P->hal.inject(w::kRadioBle, v);
        waitFor([&] { return R.snapshot().ble.size() >= 2; });
        CHECK(R.snapshot().ble.size() == 2, "a frame after garbage and a damaged frame is still received (%zu)", R.snapshot().ble.size());
    }
    P->garbage = false;
    std::string e2;
    CHECK(R.stop(e2), "stop");
    R.disconnect();
    CHECK(!R.connected(), "disconnected");
    return testSummary("esp32 loopback");
}
