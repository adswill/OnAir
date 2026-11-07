// IEEE 802.15.4 sub-GHz BPSK PHY (868.3 MHz 20 kbit/s, 915 MHz channels 40 kbit/s) through the Zigbee receiver. The test signal is built here
// (differential encoding, 15-chip PN spreading, raised-cosine pulses) with the impairments of real radios: carrier offsets of +-40 ppm,
// chip clock error, a random timing fraction per frame, noise, other sample rates, noise alone, a wrong FCS. Synthetic only: no real
// 868 / 915 MHz Zigbee transmission was available, so this proves the receiver against the standard's PHY as written, not against a radio.
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zigbee/zigbee_tel.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::zigbee;
#include "../common/test_util.h"

namespace {
const int kSeq0[15] = {1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0};

// the chips (+-1) of a PPDU: 32 zero bits, SFD 0xA7, PHR, PSDU; octets LSB first; differential encoding
std::vector<int> chipsOf(const std::vector<uint8_t>& psdu, bool badFcs) {
    std::vector<int> bits(32, 0);
    auto octet = [&](uint8_t b) { for (int i = 0; i < 8; i++) bits.push_back((b >> i) & 1); };
    octet(0xA7); octet((uint8_t)psdu.size());
    for (size_t i = 0; i < psdu.size(); i++) octet(i + 1 == psdu.size() && badFcs ? (uint8_t)(psdu[i] ^ 0x40) : psdu[i]);
    std::vector<int> chips; int r = 0;
    for (int e : bits) {
        r ^= e;   // R_n = E_n xor R_{n-1}
        for (int k = 0; k < 15; k++) chips.push_back(((kSeq0[k] ^ r) ? 1 : -1));
    }
    return chips;
}

double rc(double u) {   // raised cosine, roll-off 1, in chip units
    const double d = 1 - 4 * u * u;
    if (std::fabs(d) < 1e-9) return dect2::math::kPi / 4;
    const double s = std::fabs(u) < 1e-9 ? 1.0 : std::sin(dect2::math::kPi * u) / (dect2::math::kPi * u);
    return s * std::cos(dect2::math::kPi * u) / d;
}

struct Cfg {
    double chipRate = 300e3, rate = 2.4e6, centerMhz = 868.3, snrDb = 30, cfoHz = 0, ppm = 0, amp = 0.3;
    double dial = -1e6;
    int frames = 6; bool badFcs = false; bool noiseOnly = false;
    double gapSec = 0.03;
};

std::vector<uint8_t> macFrame(uint8_t seq) {
    MacBuild b; b.type = Data; b.destPan = 0x1A62; b.destShort = 0; b.srcShort = 0x1234; b.seq = seq;
    b.payload.assign(20, 0);
    for (size_t i = 0; i < 20; i++) b.payload[i] = (uint8_t)(i * 37 + seq);
    return buildMac(b);
}

ZigbeeTelemetry run(const Cfg& c) {
    std::mt19937 g(5);
    std::normal_distribution<double> nd(0, 1);
    std::uniform_real_distribution<double> ud(0, 1);
    const double dt = 1.0 / c.rate, Tc = 1.0 / (c.chipRate * (1 + c.ppm * 1e-6));
    const double sigma = std::sqrt(c.amp * c.amp / std::pow(10, c.snrDb / 10) * (c.rate / c.chipRate) / 2);
    // the sample stream: frames with gaps
    std::vector<std::complex<float>> x;
    const size_t gap = (size_t)(c.gapSec * c.rate);
    auto silence = [&](size_t n) { for (size_t i = 0; i < n; i++) x.emplace_back(0, 0); };
    silence(gap);
    for (int f = 0; f < c.frames && !c.noiseOnly; f++) {
        const std::vector<int> ch = chipsOf(macFrame((uint8_t)f), c.badFcs);
        const double frac = ud(g);
        const size_t n = (size_t)((ch.size() + 8) * Tc * c.rate);
        const size_t at0 = x.size();
        for (size_t m = 0; m < n; m++) {
            const double t = (double)m * dt - (4 + frac) * Tc;   // the first chip's centre lies 4 + frac chips in
            const int k0 = (int)std::floor(t / Tc);
            double v = 0;
            for (int k = k0 - 4; k <= k0 + 5; k++) if (k >= 0 && k < (int)ch.size()) v += ch[(size_t)k] * rc(t / Tc - (double)k);
            const double ph = 2 * dect2::math::kPi * c.cfoHz * (double)(at0 + m) * dt;
            x.emplace_back((float)(c.amp * v * std::cos(ph)), (float)(c.amp * v * std::sin(ph)));
        }
        silence(gap);
    }
    if (c.noiseOnly) silence(gap * 20);
    silence((size_t)(0.4 * c.rate));   // the receiver reports four times a second: the last frame needs one more report
    // mix the signal to the tuning offset, add noise
    ZigbeeReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(c.dial);
    r.setCenterHz(c.centerMhz * 1e6 - c.dial);
    std::vector<cf32> buf;
    size_t i = 0;
    while (i < x.size()) {
        buf.clear();
        for (size_t k = 0; k < 16384 && i < x.size(); k++, i++) {
            const double ph = 2 * dect2::math::kPi * c.dial * (double)i * dt;   // the radio sits |dial| above the channel: the signal is at -dial from the centre
            const std::complex<double> s(x[i].real(), x[i].imag());
            const std::complex<double> y = s * std::complex<double>(std::cos(ph), std::sin(ph)) ;
            buf.emplace_back((float)(y.real() + sigma * nd(g)), (float)(y.imag() + sigma * nd(g)));
        }
        r.feed(buf.data(), buf.size());
    }
    ZigbeeTelemetry t;
    r.telemetry(t, 0);
    return t;
}

void expectAll(const char* name, const Cfg& c, int minGood) {
    const ZigbeeTelemetry t = run(c);
    printf("  %-34s %llu good, %llu bad, channel %d, CFO %.0f Hz, SNR %.1f dB\n", name, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.channel, t.cfoHz, t.snrDb);
    if (getenv("ZB_DEBUG")) for (const auto& f : t.log) printf("      %.4f s %s\n", f.sec, f.info.c_str());
    CHECK((int)t.blocksOk >= minGood, "%s: %llu good of %d", name, (unsigned long long)t.blocksOk, c.frames);
    bool dev = false;
    for (const auto& d : t.devices) if (d.shortAddr == "0x1234") dev = true;
    if (minGood > 0) CHECK(dev, "%s: the source device 0x1234 was not seen", name);
}
}

int main() {
    // zigbee::channelCodeOf and names
    CHECK(channelCodeOf(868.3) == 100 && channelCodeName(100) == "868 ch 0", "868 code");
    CHECK(channelCodeOf(906.0) == 101 && channelCodeOf(924.0) == 110 && channelCodeName(104) == "915 ch 4", "915 codes");
    CHECK(channelCodeOf(2405) == 11 && channelCodeOf(2480) == 26 && channelCodeOf(915.5) != 0 && channelCodeOf(880) == 0, "codes");
    { Cfg c; expectAll("868.3 MHz clean 2.4 Msps", c, 6); }
    { Cfg c; c.rate = 4e6; expectAll("868.3 MHz clean 4 Msps", c, 6); }
    { Cfg c; c.cfoHz = 30e3; expectAll("868.3 MHz +30 kHz (+-35 ppm)", c, 6); }
    { Cfg c; c.cfoHz = -30e3; expectAll("868.3 MHz -30 kHz", c, 6); }
    { Cfg c; c.ppm = 40; c.cfoHz = 20e3; expectAll("868.3 MHz chip clock +40 ppm", c, 6); }
    { Cfg c; c.ppm = -40; expectAll("868.3 MHz chip clock -40 ppm", c, 6); }
    { Cfg c; c.snrDb = 8; c.frames = 12; expectAll("868.3 MHz 8 dB SNR", c, 8); }
    { Cfg c; c.chipRate = 600e3; c.rate = 4e6; c.centerMhz = 912.0; expectAll("915 MHz ch 3 clean 4 Msps", c, 6); }
    { Cfg c; c.chipRate = 600e3; c.rate = 4e6; c.centerMhz = 912.0; c.cfoHz = 36e3; expectAll("915 MHz ch 3 +36 kHz", c, 6); }
    { Cfg c; c.chipRate = 600e3; c.rate = 2.4e6; c.centerMhz = 906.0; c.cfoHz = -30e3; expectAll("915 MHz ch 1 2.4 Msps -30 kHz", c, 6); }
    { Cfg c; c.chipRate = 600e3; c.rate = 5e6; c.centerMhz = 924.0; c.snrDb = 10; c.frames = 12; expectAll("915 MHz ch 10 5 Msps 10 dB", c, 8); }
    {   // a wrong FCS is never shown as a frame
        Cfg c; c.badFcs = true;
        const ZigbeeTelemetry t = run(c);
        CHECK(t.blocksOk == 0 && t.blocksBad > 0, "bad FCS: %llu good %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {   // noise alone: no frames
        Cfg c; c.noiseOnly = true; c.snrDb = 5; c.amp = 0.3;
        const ZigbeeTelemetry t = run(c);
        CHECK(t.blocksOk == 0, "noise only: %llu frames", (unsigned long long)t.blocksOk);
    }
    return testSummary("zigbee_bpsk");
}
