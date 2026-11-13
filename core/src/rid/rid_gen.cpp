// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID test signal (rid_gen.h): Open Drone ID in BLE advertising and Wi-Fi beacons, through the existing test generators.
#include "dect2/rid/rid_gen.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/rid/rid_rx.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/dsp/fir_dot.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;

rid::Message msg(int type) { rid::Message m; m.type = type; m.version = 2; return m; }
}

std::vector<rid::Message> ridTestDrone1() {
    std::vector<rid::Message> v;
    rid::Message b = msg(rid::BasicId); b.idType = 1; b.uaType = 2; b.id = "1581F5FKD229400B"; v.push_back(b);
    rid::Message s = msg(rid::BasicId); s.idType = 4; s.uaType = 2; s.id = "SESSION-7F3A"; v.push_back(s);
    rid::Message l = msg(rid::Location);
    l.status = 2; l.heightType = 0; l.posValid = true; l.lat = 54.6872; l.lon = 25.2797;
    l.altBaro = 230.5; l.altGeo = 245; l.height = 120; l.speed = 8.5; l.vspeed = 1.5; l.vspeedValid = true; l.direction = 90;
    l.horizAcc = 10; l.vertAcc = 4; l.baroAcc = 3; l.speedAcc = 3; l.timestamp = 12345; l.tsAcc = 2;
    v.push_back(l);
    rid::Message y = msg(rid::System);
    y.opLocType = 1; y.classType = 1; y.opValid = true; y.opLat = 54.6860; y.opLon = 25.2770; y.opAlt = 126;
    y.areaCount = 1; y.areaRadius = 0; y.category = 1; y.uaClass = 2; y.sysTime = 245000000;
    v.push_back(y);
    rid::Message o = msg(rid::OperatorId); o.opIdType = 0; o.text = "LTUabcd1234efgh"; v.push_back(o);
    rid::Message t = msg(rid::SelfId); t.descType = 0; t.text = "Survey flight"; v.push_back(t);
    rid::Message a = msg(rid::Auth); a.authType = 1; a.page = 0; a.lastPage = 0; a.authLength = 17; a.authTime = 245000000;
    for (int i = 0; i < 17; i++) a.authData.push_back((uint8_t)(0xA0 + i));
    v.push_back(a);
    return v;
}

std::vector<rid::Message> ridTestDrone2() {
    std::vector<rid::Message> v;
    rid::Message b = msg(rid::BasicId); b.idType = 2; b.uaType = 1; b.id = "FIN87astrdge12k8"; v.push_back(b);
    rid::Message l = msg(rid::Location);
    l.status = 2; l.heightType = 1; l.posValid = true; l.lat = 54.7001; l.lon = 25.3002;
    l.altBaro = 410; l.altGeo = 420; l.height = 300; l.speed = 70; l.vspeedValid = false; l.direction = 275;
    l.horizAcc = 11; l.vertAcc = 5; l.speedAcc = 4; l.timestamp = 600;
    v.push_back(l);
    rid::Message y = msg(rid::System);
    y.opLocType = 0; y.classType = 1; y.opValid = true; y.opLat = 54.6950; y.opLon = 25.2900; y.opAlt = 110; y.category = 2; y.uaClass = 0; y.sysTime = 245000100;
    v.push_back(y);
    rid::Message o = msg(rid::OperatorId); o.text = "FINop-998877"; v.push_back(o);
    return v;
}

std::vector<BleGenPacket> ridBleSchedule() {
    std::vector<BleGenPacket> v;
    const uint8_t addr[6] = {0x9A, 0x78, 0x56, 0x34, 0x12, 0xC6};   // C6:12:34:56:78:9A (sent least significant byte first)
    int counter = 0;
    for (const auto& m : ridTestDrone1()) {
        BleGenPacket p;
        const std::vector<uint8_t> ad = rid::bleServiceData(counter++, m);   // AdvA + 31 bytes: a full legacy packet, no room for flags
        p.pdu = ble::makePdu(ble::AdvNonconnInd, true, ble::advPayload(addr, ad));
        p.channel = 38;
        p.gapSec = 0.002;
        v.push_back(std::move(p));
    }
    // a beacon that is not a drone
    const uint8_t other[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0xD0};
    BleGenPacket p;
    p.pdu = ble::makePdu(ble::AdvNonconnInd, true, ble::advPayload(other, ble::AdBuilder().flags(0x06).name("Thermo").b));
    p.channel = 38;
    v.push_back(std::move(p));
    return v;
}

std::vector<WifiGenFrame> ridWifiSchedule() {
    std::vector<WifiGenFrame> v;
    auto beacon = [](const uint8_t bssid[6], const char* ssid, const std::vector<uint8_t>& extra) {
        wifi::ApSpec s;
        std::copy(bssid, bssid + 6, s.bssid);
        s.ssid = ssid; s.channel = 6; s.security = wifi::Open; s.gRates = false;
        std::vector<uint8_t> f = wifi::makeBeacon(s);
        f.resize(f.size() - 4);   // the FCS goes after the element
        f.insert(f.end(), extra.begin(), extra.end());
        wifi::appendFcs(f);
        return f;
    };
    const uint8_t d2[6] = {0x60, 0x60, 0x1F, 0xAA, 0xBB, 0xCC}, ap[6] = {0x00, 0x04, 0x0E, 0x11, 0x22, 0x33};
    for (int k = 0; k < 3; k++) {
        WifiGenFrame f;
        f.mpdu = beacon(d2, "RID-DJI-2", rid::beaconElement(k, ridTestDrone2()));
        f.gapSec = 0.003;
        v.push_back(std::move(f));
    }
    WifiGenFrame f;
    f.mpdu = beacon(ap, "Home", {});
    f.gapSec = 0.003;
    v.push_back(std::move(f));
    return v;
}

struct RidGenerator::Impl {
    RidGenConfig c;
    std::unique_ptr<BleGenerator> ble;
    std::unique_ptr<WifiGenerator> wifi;
    std::complex<double> lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    std::vector<float> taps, taps2;   // taps2: the doubled layout of firDot
    std::vector<cf32> hist, a, b, x;   // hist: the last taps of the double-rate signal; x: history + block, kept between calls
};

RidGenerator::RidGenerator(const RidGenConfig& c) : p_(std::make_unique<Impl>()) {
    Impl& m = *p_;
    m.c = c;
    const double r2 = 2 * c.rate;
    const RidPlan plan = ridPlan(c.centerHz);
    if (c.ble) {
        BleGenConfig g;
        g.rate = r2; g.snrDb = c.snrDb; g.cfoHz = c.cfoHz; g.dialOffsetHz = plan.bleOffsetHz; g.seed = c.seed;
        g.pkts = ridBleSchedule();
        m.ble = std::make_unique<BleGenerator>(g);
    }
    if (c.wifi) {
        WifiGenConfig g;
        g.rate = r2; g.snrDb = c.snrDb; g.cfoHz = c.cfoHz; g.seed = c.seed + 7;
        g.frames = ridWifiSchedule();
        m.wifi = std::make_unique<WifiGenerator>(g);
        m.loStep = std::polar(1.0, 2 * kPi * plan.wifiOffsetHz / r2);
    }
    // the radio's channel filter: windowed sinc, cut-off at 0.45 of the output rate
    const int L = 63;
    const double fc = 0.45 * c.rate / r2;
    double sum = 0;
    m.taps.resize(L);
    for (int i = 0; i < L; i++) {
        const double k = i - (L - 1) / 2.0;
        const double s = k == 0 ? 2 * fc : std::sin(2 * kPi * fc * k) / (kPi * k);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (L - 1)) + 0.08 * std::cos(4 * kPi * i / (L - 1));
        m.taps[(size_t)i] = (float)(s * w); sum += s * w;
    }
    for (float& t : m.taps) t = (float)(t / sum);
    m.taps2 = firDoubleTaps(m.taps.data(), L);
    m.hist.assign((size_t)L - 1, cf32(0, 0));
}
RidGenerator::~RidGenerator() = default;

void RidGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const size_t n2 = 2 * n, L = m.taps.size();
    m.a.assign(n2, cf32(0, 0));
    if (m.ble) m.ble->generate(m.a.data(), n2);
    if (m.wifi) {
        m.b.resize(n2);
        m.wifi->generate(m.b.data(), n2);
        // the oscillator written out by hand (a std::complex<double> product may go through the NaN-checking library call)
        double lr = m.lo.real(), li = m.lo.imag();
        const double sr = m.loStep.real(), si = m.loStep.imag();
        for (size_t i = 0; i < n2; i++) {
            const cf32 v = m.b[i];
            const float fr = (float)lr, fi = (float)li;
            m.a[i] += cf32(v.real() * fr - v.imag() * fi, v.real() * fi + v.imag() * fr);
            const double nr = lr * sr - li * si;
            li = lr * si + li * sr;
            lr = nr;
            if (++m.loCount >= 4096) { m.loCount = 0; const double g = 1.0 / std::sqrt(lr * lr + li * li); lr *= g; li *= g; }
        }
        m.lo = std::complex<double>(lr, li);
    }
    // filter and keep every second sample (the history and the block in one buffer that is reused, not allocated per call)
    m.x.resize(L - 1 + n2);
    std::copy(m.hist.begin(), m.hist.end(), m.x.begin());
    std::copy(m.a.begin(), m.a.end(), m.x.begin() + (ptrdiff_t)(L - 1));
    for (size_t k = 0; k < n; k++) out[k] = firDot(m.taps2.data(), m.x.data() + 2 * k, (int)L);
    m.hist.assign(m.x.end() - (ptrdiff_t)(L - 1), m.x.end());
}

namespace {
class RidSynth : public ModeSynth {
public:
    RidSynth(const SynthConfig& s, double rate) : rate_(rate) {
        RidGenConfig c;
        c.rate = rate; c.snrDb = s.snrDb; c.cfoHz = s.cfoHz;
        c.centerHz = ridTuning().defMhz * 1e6;
        c.ble = s.modeOpt[0] != 2; c.wifi = s.modeOpt[0] != 1;
        gen_ = std::make_unique<RidGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<RidGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeRidSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<RidSynth>(cfg, sampleRate);
}

} // namespace dect2
