// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sub-GHz sensors receiver on synthetic OOK / FSK signals: decoding at several sample rates, off-centre and with noise, the device table,
// the unknown signal analyser and a flex decoder made from its suggestion, FSK through a custom decoder, exports, "Remove after", negatives.
#include "dect2/sensors/sensors_rx.h"
#include "dect2/sensors/sensors_tel.h"
#include "dect2/sensors/sensors_gen.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static void feedAll(SensorsReceiver& rx, const std::vector<cf32>& x) {
    const size_t blk = 8192;
    for (size_t o = 0; o < x.size(); o += blk) rx.feed(x.data() + o, std::min(blk, x.size() - o));
    rx.flush();
}

// the burst, resampled to `rate`, shifted by shiftHz, with noise at snrDb (carrier power over noise in the 250 kHz channel; >= 99: none)
static std::vector<cf32> sig(const PulseList& p, double rate, double shiftHz, double snrDb, uint32_t seed = 3) {
    std::vector<cf32> x = sensorsOokSignal(p, rate, 0.5, 20000, 60000);
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double sigma = snrDb >= 99 ? 0 : std::sqrt(0.5 * 0.5 / 2 / std::pow(10.0, snrDb / 10.0) * (rate / 250e3) / 2);
    double ph = 0;
    for (auto& v : x) {
        if (shiftHz != 0) { ph += 2 * math::kPi * shiftHz / rate; v *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
        if (sigma > 0) v += cf32(nd(rng) * (float)sigma, nd(rng) * (float)sigma);
    }
    return x;
}

static const SensorField* fld(const SensorDevice& d, const char* k) {
    for (const auto& f : d.latest) if (f.key == k) return &f;
    return nullptr;
}

static bool nexusOk(const SensorsTelemetry& t, const char* what) {
    for (const auto& d : t.devices) {
        if (d.model != "Nexus-TH") continue;
        const SensorField* tc = fld(d, "temperature_C");
        const SensorField* hu = fld(d, "humidity");
        const bool ok = d.id == "106" && d.channel == "1" && tc && std::fabs(tc->num - 21.4) < 0.05 && hu && hu->num == 47 && d.batteryOk == 1;
        CHECK(ok, "%s: Nexus fields id=%s ch=%s", what, d.id.c_str(), d.channel.c_str());
        return ok;
    }
    CHECK(false, "%s: no Nexus-TH sensor (%zu sensors, %llu packages)", what, t.devices.size(), (unsigned long long)t.packages);
    return false;
}

static SensorsTelemetry run(double rate, double shiftHz, double snrDb, double offsetHz = 0) {
    SensorsReceiver rx;
    rx.setAutoLevel(true);
    rx.configure(rate);
    rx.setSignalOffset(offsetHz);
    feedAll(rx, sig(sensorsNexusPulses(0x6a, 1, true, 21.4, 47, 12), rate, shiftHz, snrDb));
    SensorsTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

int main() {
    const ModeTuning mt = sensorsTuning();
    CHECK(mt.stdMode == 243 && std::string(mt.id) == "sensors" && mt.defMhz == 433.92 && mt.sampleRate == 250e3, "defaults: 433.92 MHz at 250 kHz");

    // the decoder set
    {
        const auto p = SensorsReceiver::protocols();
        unsigned def = 0;
        for (const auto& x : p) def += x.defaultEnabled;
        printf("  %zu decoders, %u on by default\n", p.size(), def);
        CHECK(p.size() > 250 && def > 200, "decoder table");
    }

    // the same message at several input rates, shifted off the centre, and in noise
    nexusOk(run(250e3, 0, 99), "250 ksps");
    nexusOk(run(500e3, 0, 99), "500 ksps");
    nexusOk(run(1e6, 0, 99), "1 Msps");
    nexusOk(run(2e6, 0, 99), "2 Msps (averaged by 8)");
    nexusOk(run(250e3, 25e3, 99), "25 kHz off the centre");
    nexusOk(run(1e6, 80e3, 99, 80e3), "80 kHz off, offset set");
    nexusOk(run(250e3, 0, 24), "24 dB SNR");
    nexusOk(run(250e3, -30e3, 22), "-30 kHz and 22 dB");

    // the device table: a second message of the same sensor updates it, history grows, a second sensor is added
    {
        SensorsReceiver rx;
        rx.configure(250e3);
        std::vector<int64_t> now = {1000};
        rx.setClock([&] { return now[0]; });
        feedAll(rx, sig(sensorsNexusPulses(0x6a, 1, true, 21.4, 47, 12), 250e3, 0, 99));
        now[0] = 1010;
        feedAll(rx, sig(sensorsNexusPulses(0x6a, 1, true, 21.9, 46, 12), 250e3, 0, 99));
        feedAll(rx, sig(sensorsNexusPulses(0x2c, 2, false, -3.6, 81, 12), 250e3, 0, 99));
        SensorsTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.devices.size() == 2, "two sensors, got %zu", t.devices.size());
        const SensorDevice* a = nullptr; const SensorDevice* b = nullptr;
        for (const auto& d : t.devices) { if (d.id == "106") a = &d; if (d.id == "44") b = &d; }
        CHECK(a && b, "both ids");
        if (a && b) {
            CHECK(fld(*a, "temperature_C") && std::fabs(fld(*a, "temperature_C")->num - 21.9) < 0.05, "latest value kept");
            CHECK(a->count >= 2 && a->kind == "weather", "count %llu kind %s", (unsigned long long)a->count, a->kind.c_str());
            size_t pts = 0;
            for (const auto& s : a->history) if (s.key == "temperature_C") pts = s.pts.size();
            CHECK(pts == 2, "history points %zu", pts);
            CHECK(b->batteryOk == 0 && fld(*b, "temperature_C") && std::fabs(fld(*b, "temperature_C")->num + 3.6) < 0.05, "low battery, negative temperature");
        }
        // exports
        const std::string csv = sensorsCsv(t), js = sensorsJson(t), mq = sensorsMqtt(t);
        CHECK(csv.find("model,id,channel,kind") == 0 && csv.find("Nexus-TH,106,1,weather") != std::string::npos && csv.find("temperature_C") != std::string::npos, "csv");
        CHECK(js.find("\"model\":\"Nexus-TH\"") != std::string::npos && js.find("\"temperature_C\":21.9") != std::string::npos, "json");
        CHECK(mq.find("rtl_433/devices/Nexus-TH/1/106/temperature_C 21.9") != std::string::npos, "mqtt: %s", mq.substr(0, 200).c_str());
        // remove after: 60 s, the clock moves on, the next block of samples prunes
        const std::string pinKey = t.devices.empty() ? std::string() : t.devices.front().key;
        rx.setPinned({pinKey});
        rx.setRemoveAfterSec(60);
        now[0] = 1010 + 30;
        feedAll(rx, std::vector<cf32>(40000, cf32(0, 0)));
        rx.telemetry(t, 0);
        CHECK(t.devices.size() == 2, "still there after 30 s");
        now[0] = 1010 + 200;
        feedAll(rx, std::vector<cf32>(40000, cf32(0, 0)));
        SensorsTelemetry t2;
        CHECK(rx.telemetry(t2, 0), "telemetry after prune");
        CHECK(t2.devices.size() == 1 && t2.devices.front().key == pinKey, "removed after 60 s except the pinned one, %zu left", t2.devices.size());
        rx.setPinned({});
        now[0] = 1010 + 400;
        feedAll(rx, std::vector<cf32>(40000, cf32(0, 0)));
        SensorsTelemetry t3;
        CHECK(rx.telemetry(t3, 0) && t3.devices.empty(), "unpinned: removed");
    }

    // unknown signal: a PWM burst no decoder knows -> analyser guess, bits, flex spec; the spec decodes the signal
    {
        const PulseList burst = sensorsUnknownPwmPulses(0x5a3c96e1u, 32, 6);
        SensorsReceiver rx;
        rx.setAutoLevel(true);
        rx.configure(250e3);
        feedAll(rx, sig(burst, 250e3, 0, 99));
        SensorsTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.devices.empty(), "no decoder takes it (got %zu: %s)", t.devices.size(), t.devices.empty() ? "" : t.devices[0].model.c_str());
        CHECK(!t.unknown.empty(), "unknown signal listed");
        if (!t.unknown.empty()) {
            const SensorUnknown& u = t.unknown.front();
            printf("  unknown: %s | %s | %s\n", u.guess.c_str(), u.flexSpec.c_str(), u.hex.substr(0, 40).c_str());
            CHECK(u.modName.find("Pulse width") != std::string::npos, "guess PWM: %s", u.modName.c_str());
            CHECK(u.hex.find("5a3c96e1") != std::string::npos || u.hex.find("a5c3691e") != std::string::npos, "bits recovered (or inverted): %s", u.hex.c_str());
            CHECK(!u.flexSpec.empty() && u.flexSpec.find("m=OOK_PWM") != std::string::npos, "flex spec: %s", u.flexSpec.c_str());
            CHECK(SensorsReceiver::checkFlexSpec(u.flexSpec).empty(), "the suggestion is a valid flex definition");
            CHECK(!u.pulseUs.empty() && u.pulseUs.size() == u.gapUs.size(), "widths kept");
            // save it as a decoder: the same signal is now a sensor
            SensorsReceiver rx2;
            rx2.setAutoLevel(true);
            rx2.configure(250e3);
            CHECK(rx2.setCustomDecoders({u.flexSpec}) == 1, "custom decoder accepted");
            feedAll(rx2, sig(burst, 250e3, 0, 99));
            SensorsTelemetry t2;
            rx2.telemetry(t2, 0);
            bool found = false;
            for (const auto& d : t2.devices) if (d.model == "unknown") { found = true; CHECK(fld(d, "codes") || fld(d, "len"), "flex output has the row"); }
            CHECK(found, "the saved decoder reports the signal (%zu sensors)", t2.devices.size());
            CHECK(t2.customProtocols == 1, "custom count");
        }
    }

    // flex spec checks: valid and invalid definitions (an invalid one must not end the program)
    CHECK(SensorsReceiver::checkFlexSpec("n=x,m=OOK_PPM,s=500,l=1500,r=5000").empty(), "valid ppm spec");
    CHECK(!SensorsReceiver::checkFlexSpec("n=x,m=NOPE,s=500").empty(), "bad modulation refused");
    CHECK(!SensorsReceiver::checkFlexSpec("this is not a spec").empty(), "garbage refused");
    CHECK(!SensorsReceiver::checkFlexSpec("").empty(), "empty refused");

    // FSK: 2-FSK 10 kbit/s +-25 kHz, two copies of a 64-bit word, through a custom FSK_PCM decoder
    {
        const double rate = 250e3, baud = 10e3, dev = 25e3;
        const uint64_t word = 0xa5c3d2e1f0b4961eull;
        std::vector<cf32> x(30000, cf32(0, 0));
        double ph = 0;
        for (int rep = 0; rep < 3; rep++) {
            // preamble 0xaa x2 then the word
            std::vector<int> bits;
            for (int i = 0; i < 16; i++) bits.push_back(i % 2);
            for (int i = 63; i >= 0; i--) bits.push_back((word >> i) & 1);
            for (int b : bits)
                for (int k = 0; k < (int)(rate / baud); k++) {
                    ph += 2 * math::kPi * (b ? dev : -dev) / rate;
                    x.push_back(cf32(0.5f * (float)std::cos(ph), 0.5f * (float)std::sin(ph)));
                }
            x.insert(x.end(), 12000, cf32(0, 0));
        }
        SensorsReceiver rx;
        rx.setAutoLevel(true);
        rx.configure(rate);
        CHECK(rx.setCustomDecoders({"n=fskword,m=FSK_PCM,s=100,l=100,r=1000,bits>=60"}) == 1, "fsk spec");
        feedAll(rx, x);
        SensorsTelemetry t;
        rx.telemetry(t, 0);
        bool found = false;
        for (const auto& d : t.devices) if (d.model == "fskword") found = true;
        CHECK(found, "FSK package decoded by the custom FSK_PCM decoder (%llu FSK packages, %zu sensors)", (unsigned long long)t.fskPackages, t.devices.size());
    }

    // negatives: noise and a carrier give no sensors and no crash
    {
        SensorsReceiver rx;
        rx.setAutoLevel(true);
        rx.configure(250e3);
        std::mt19937 rng(9);
        std::normal_distribution<float> nd(0.f, 0.05f);
        std::vector<cf32> x(500000);
        for (auto& v : x) v = cf32(nd(rng), nd(rng));
        feedAll(rx, x);
        for (auto& v : x) v += cf32(0.4f, 0.1f);
        feedAll(rx, x);
        SensorsTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.devices.empty(), "noise and a carrier decode nothing (%zu)", t.devices.size());
    }
    {   // the first telemetry comes with the first block, not with the first sensor heard (the screen stayed on "starting" until then)
        SensorsReceiver r;
        r.configure(250e3);
        r.reset();
        SensorsTelemetry tel;
        CHECK(!r.telemetry(tel, 0), "nothing before the first block");
        std::vector<cf32> quiet(40000, cf32(0.001f, 0.f));
        r.feed(quiet.data(), quiet.size());
        uint64_t last = 0;
        const bool first = r.telemetry(tel, last);
        CHECK(first && tel.seq > 0 && tel.devices.empty(), "first block reported with no sensor heard (seq %llu)", (unsigned long long)tel.seq);
        last = tel.seq;
        CHECK(!r.telemetry(tel, last), "nothing new until half a second of signal or a change");
        std::vector<cf32> more(140000, cf32(0.001f, 0.f));
        r.feed(more.data(), more.size());
        CHECK(r.telemetry(tel, last) && tel.timeSec > 0.5, "time moves on while nothing is heard (%.2f s)", tel.timeSec);
    }
    return testSummary("sensors rx");
}
