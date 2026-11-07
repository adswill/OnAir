// Thread through the 802.15.4 receiver in Thread mode: the O-QPSK test signal (zigbee_gen.h) carrying a Thread beacon, link-layer encrypted data,
// MLE Discovery frames and fragments, under real-world impairments: carrier offsets of +-100 kHz (+-40 ppm at 2.4 GHz), chip clock error, noise,
// another sample rate, the Zigbee-mode receiver on the same samples (it must not call this Thread), noise alone.
// Synthetic only: no real Thread network was recorded.
#include "dect2/thread/thread_proto.h"
#include "dect2/zigbee/zigbee_gen.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zigbee/zigbee_tel.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<ZigbeeGenFrame> schedule() {
    std::vector<ZigbeeGenFrame> v;
    auto add = [&](const std::vector<uint8_t>& psdu) { ZigbeeGenFrame f; f.psdu = psdu; f.gapSec = 0.0008; v.push_back(f); };
    {   // beacon
        std::vector<uint8_t> pl = {0xFF, 0xCF, 0x00, 0x00, 0x03, 0x20};
        const char* nm = "HomeThread";
        for (int i = 0; i < 16; i++) pl.push_back(i < 10 ? (uint8_t)nm[i] : 0);
        for (int i = 0; i < 8; i++) pl.push_back((uint8_t)(0x11 * (i + 1)));
        zigbee::MacBuild b; b.type = zigbee::Beacon; b.destMode = 0; b.srcShort = 0x5000; b.srcPan = 0x7A31; b.payload = pl;
        add(zigbee::buildMac(b));
    }
    for (int k = 0; k < 6; k++) {   // encrypted data between two nodes of the PAN
        std::vector<uint8_t> pl = {0x0D, (uint8_t)k, 0, 0, 0, 0x01};
        for (int i = 0; i < 24; i++) pl.push_back((uint8_t)(i * 13 + k));
        zigbee::MacBuild b; b.type = zigbee::Data; b.destPan = 0x7A31; b.destShort = 0x5000; b.srcShort = (uint16_t)(0x5400 + k % 2); b.secured = true; b.seq = (uint8_t)k; b.payload = pl;
        add(zigbee::buildMac(b));
    }
    {   // MLE Discovery Request in the clear
        zigbee::MacBuild b; b.type = zigbee::Data; b.destPan = 0xFFFF; b.destShort = 0xFFFF; b.srcShort = 0x5600; b.srcMode = 3; b.srcExt = 0x0123456789ABCDEFULL;
        b.payload = {0x7F, 0x3B, 0x02, 0xF0, 0x4D, 0x4C, 0x4D, 0x4C, 0x12, 0x34, 0xFF, 0x10, 0x0B, 0x02, 0x20, 0x00};
        add(zigbee::buildMac(b));
    }
    return v;
}

static ZigbeeTelemetry run(bool thread, double rate, double cfo, double ppm, double snr, double secs, bool noiseOnly = false) {
    ZigbeeGenConfig c;
    c.rate = rate; c.snrDb = snr; c.cfoHz = cfo; c.clockPpm = ppm;
    c.dialOffsetHz = -1e6;
    if (!noiseOnly) c.frames = schedule(); else { ZigbeeGenFrame f; f.psdu = {0x02, 0x00, 0x00, 0x00, 0x00}; c.frames.push_back(f); c.amp = 0.0001; }
    ZigbeeGenerator g(c);
    ZigbeeReceiver r;
    r.configure(rate);
    r.setThreadMode(thread);
    r.setSignalOffset(c.dialOffsetHz);
    r.setCenterHz(zigbee::channelMhz(15) * 1e6 - c.dialOffsetHz);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(secs * rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    ZigbeeTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static void check(const char* name, const ZigbeeTelemetry& t, bool thread) {
    printf("  %-30s %llu good, %llu bad, %zu PANs, %zu devices, %llu encrypted, channel %d\n", name, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.pans.size(), t.devices.size(), (unsigned long long)t.encryptedFrames, t.channel);
    CHECK(t.blocksOk >= 20 && t.channel == 15, "%s: %llu frames, channel %d", name, (unsigned long long)t.blocksOk, t.channel);
    const ZbPan* p = nullptr;
    for (const auto& x : t.pans) if (x.pan == 0x7A31) p = &x;
    CHECK(p != nullptr, "%s: PAN 7A31 not seen", name);
    if (p && thread) CHECK(p->networkName == "HomeThread" && p->extPan == "11:22:33:44:55:66:77:88" && p->joinOpen && p->threadVersion == 2, "%s: network \"%s\" ext %s", name, p->networkName.c_str(), p->extPan.c_str());
    if (p && !thread) CHECK(p->networkName.empty(), "%s: Zigbee mode named a Thread network", name);
    bool enc = false, mle = false;
    for (const auto& f : t.log) {
        if (f.info.find("link-layer AES-CCM encrypted") != std::string::npos) enc = true;
        if (f.info.find("MLE Discovery Request") != std::string::npos) mle = true;
    }
    if (thread) CHECK(enc && mle && t.encryptedFrames >= 6, "%s: encrypted frames shown %d, MLE shown %d, %llu counted encrypted", name, enc, mle, (unsigned long long)t.encryptedFrames);
    else CHECK(!mle, "%s: Zigbee mode decoded MLE", name);
}

int main() {
    check("Thread 4 Msps clean", run(true, 4e6, 0, 0, 30, 0.5), true);
    check("Thread +100 kHz", run(true, 4e6, 100e3, 0, 25, 0.5), true);
    check("Thread -100 kHz, +40 ppm", run(true, 4e6, -100e3, 40, 25, 0.5), true);
    check("Thread 10 dB SNR", run(true, 4e6, 30e3, -20, 10, 1.0), true);
    check("Thread 5 Msps", run(true, 5e6, 50e3, 0, 25, 0.5), true);
    check("Zigbee mode on the same", run(false, 4e6, 0, 0, 30, 0.5), false);
    {   // noise alone: nothing
        const ZigbeeTelemetry t = run(true, 4e6, 0, 0, 5, 0.5, true);
        CHECK(t.pans.empty() && t.devices.empty(), "noise: %zu PANs", t.pans.size());
    }
    return testSummary("thread_rx");
}
