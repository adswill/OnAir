// APRS / Packet test signal: see packet_gen.h.
#include "dect2/packet_gen.h"
#include "dect2/gen_util.h"
#include "dect2/packet_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kWork = 96000.0;              // the rate the cycle is built at
constexpr double kLeadSec = 0.6;               // quiet before the first frame (the receiver measures its noise floor then)
constexpr double kDev1200 = 2800.0;            // peak deviation of the AFSK tones, Hz
constexpr double kDev9600 = 3000.0;
constexpr double kChanBw = 25000.0;

ax25::Address addr(const std::string& s, bool h = false) {
    ax25::Address a;
    const size_t d = s.find('-');
    a.call = s.substr(0, d);
    a.ssid = d == std::string::npos ? 0 : atoi(s.c_str() + d + 1);
    a.h = h;
    return a;
}
ax25::Frame ui(const std::string& from, const std::string& to, const std::vector<std::pair<std::string, bool>>& path, const std::string& info) {
    ax25::Frame f;
    f.from = addr(from); f.to = addr(to);
    for (const auto& p : path) f.path.push_back(addr(p.first, p.second));
    f.info = info;
    return f;
}

std::string latStr(double lat) {
    char b[16];
    const double a = std::fabs(lat);
    const int d = (int)a;
    snprintf(b, sizeof b, "%02d%05.2f%c", d, (a - d) * 60.0, lat < 0 ? 'S' : 'N');
    return b;
}
std::string lonStr(double lon) {
    char b[16];
    const double a = std::fabs(lon);
    const int d = (int)a;
    snprintf(b, sizeof b, "%03d%05.2f%c", d, (a - d) * 60.0, lon < 0 ? 'W' : 'E');
    return b;
}
void b91(std::string& s, long v, int n) {
    std::string t(n, ' ');
    for (int i = n - 1; i >= 0; i--) { t[(size_t)i] = (char)(33 + v % 91); v /= 91; }
    s += t;
}
// compressed position with course and speed
std::string compressed(char table, char code, double lat, double lon, int course, double knots) {
    std::string s(1, table);
    b91(s, (long)((90.0 - lat) * 380926.0 + 0.5), 4);
    b91(s, (long)((180.0 + lon) * 190463.0 + 0.5), 4);
    s += code;
    s += (char)(33 + course / 4);
    s += (char)(33 + (int)std::lround(std::log(knots + 1.0) / std::log(1.08)));
    s += 'A';
    return s;
}
// Mic-E: the destination address and the information field
void micE(double lat, double lon, int knots, int course, char table, char code, const std::string& comment, std::string& dest, std::string& info) {
    const double a = std::fabs(lat);
    const int deg = (int)a;
    const int mins = (int)((a - deg) * 60.0), hund = (int)std::lround(((a - deg) * 60.0 - mins) * 100.0);
    const int dg[6] = {deg / 10, deg % 10, mins / 10, mins % 10, hund / 10, hund % 10};
    const double lo = std::fabs(lon);
    const int d = (int)lo;
    const int m = (int)((lo - d) * 60.0), h = (int)std::lround(((lo - d) * 60.0 - m) * 100.0);
    const bool offs = d < 10 || d >= 100;                  // longitude offset flag in the fifth letter
    const int dc = d < 10 ? d + 118 : d < 100 ? d + 28 : d < 110 ? d - 100 + 108 : d - 110 + 38;
    dest.clear();
    for (int i = 0; i < 6; i++) {
        const bool high = i == 0 || i == 1 || (i == 3 && lat >= 0) || (i == 4 && offs) || (i == 5 && lon < 0);
        dest += (char)((high ? 'P' : '0') + dg[i]);        // the first two letters also carry the message code "En Route"
    }
    info = "`";
    info += (char)(dc + 28);
    info += (char)((m < 10 ? m + 60 : m) + 28);
    info += (char)(h + 28);
    info += (char)(knots / 10 + 28);
    info += (char)((knots % 10) * 10 + course / 100 + 28);
    info += (char)(course % 100 + 28);
    info += code;
    info += table;
    info += comment;
}

std::vector<PacketGenFrame> frames1200() {
    std::vector<PacketGenFrame> v;
    auto add = [&](ax25::Frame f) { PacketGenFrame g; g.frame = std::move(f); g.baud = 1200; v.push_back(std::move(g)); };
    add(ui("A61QQ-9", "APRS", {{"WIDE1-1", true}, {"WIDE2-1", false}}, "!2512.34N/05518.12E>123/045/A=000328Dubai mobile"));
    add(ui("A61QQ-7", "APDW17", {{"WIDE1-1", false}}, "=" + compressed('/', '>', 25.2086, 55.2714, 88, 36.0) + "Compressed mobile"));
    std::string dest, info;
    micE(25.1993, 55.3045, 12, 247, '/', '>', "]Mic-E from a moving car", dest, info);
    add(ui("A61MIC-8", dest, {{"WIDE2-1", false}}, info));
    add(ui("A61TST", "APRS", {}, ";SANDSTORM*092345z" + latStr(25.15) + "/" + lonStr(55.35) + "!Sand storm warning"));
    add(ui("A61TST", "APRS", {{"A61RPT-1", true}}, ")HOTSPOT!" + latStr(25.1111) + "/" + lonStr(55.2111) + "-Wi-Fi hotspot"));
    add(ui("A61TST", "APRS", {}, ":A61QQ-9  :Hello from the desert{42"));
    add(ui("A61QQ-9", "APRS", {{"WIDE1-1", true}}, ":A61TST   :ack42"));
    add(ui("A61WX", "APRS", {}, "@092345z" + latStr(25.1667) + "/" + lonStr(55.2833) + "_220/004g005t098r000p000P000h45b10098"));
    add(ui("A61TST", "APRS", {{"WIDE2-2", false}}, ">On air from Dubai, 144.800 MHz"));
    return v;
}

std::vector<PacketGenFrame> frames9600() {
    std::vector<PacketGenFrame> v;
    auto add = [&](ax25::Frame f) { PacketGenFrame g; g.frame = std::move(f); g.baud = 9600; v.push_back(std::move(g)); };
    add(ui("A61HS-1", "APRS", {{"WIDE1-1", false}}, "!2509.50N/05516.40E#9600 baud digipeater"));
    add(ui("A61HS-1", "APRS", {}, ":A61TST   :Fast hello{7"));
    add(ui("A61HS-2", "APRS", {}, ">9600 baud G3RUH test"));
    add(ui("A61HS-3", "APRS", {}, "!2530.00N/05520.25E-Fixed station with a long comment so that the frame is longer than the others: weather permitting"));
    return v;
}

struct Cycle {
    std::vector<PacketGenFrame> frames;
    std::vector<cf32> bb;          // the complex baseband at 96 kHz, unit amplitude while a frame is sent
};

// FM modulator state while building the cycle
struct Mod {
    std::vector<cf32>* out;
    double theta = 0;
    void silence(double sec) { out->insert(out->end(), (size_t)std::lround(sec * kWork), cf32(0.f, 0.f)); }
    void push(double devHz) {
        theta += 2 * kPi * devHz / kWork;
        if (theta > kPi) theta -= 2 * kPi;
        out->push_back(cf32((float)std::cos(theta), (float)std::sin(theta)));
    }
};

void send1200(Mod& m, const std::vector<uint8_t>& levels) {
    double ph = 0;
    for (uint8_t lv : levels)
        for (int i = 0; i < 80; i++) {
            ph += 2 * kPi * (lv ? 1200.0 : 2200.0) / kWork;
            if (ph > 2 * kPi) ph -= 2 * kPi;
            m.push(kDev1200 * std::sin(ph));
        }
}
void send9600(Mod& m, const std::vector<uint8_t>& levels) {
    // levels -> +-1 at 10 samples per bit, shaped by a Gaussian filter (BT 0.5)
    const int sps = 10;
    const double sigma = std::sqrt(std::log(2.0)) / (2 * kPi * 0.5 * 9600.0) * kWork;
    const int half = (int)std::ceil(3 * sigma);
    std::vector<double> g((size_t)(2 * half + 1));
    double sum = 0;
    for (int i = -half; i <= half; i++) { g[(size_t)(i + half)] = std::exp(-0.5 * i * i / (sigma * sigma)); sum += g[(size_t)(i + half)]; }
    for (double& x : g) x /= sum;
    const long n = (long)levels.size() * sps;
    auto nrz = [&](long i) { i = std::max(0L, std::min(n - 1, i)); return levels[(size_t)(i / sps)] ? 1.0 : -1.0; };
    for (long i = 0; i < n; i++) {
        double acc = 0;
        for (int k = -half; k <= half; k++) acc += g[(size_t)(k + half)] * nrz(i + k);
        m.push(kDev9600 * acc);
    }
}

const Cycle& cycleFor(int mode) {
    static Cycle cache[3];
    static bool built[3] = {false, false, false};
    const int k = mode == 1 ? 1 : mode == 2 ? 2 : 0;
    if (built[k]) return cache[k];
    Cycle& c = cache[k];
    if (k != 2) { auto f = frames1200(); c.frames.insert(c.frames.end(), f.begin(), f.end()); }
    if (k != 1) { auto f = frames9600(); c.frames.insert(c.frames.end(), f.begin(), f.end()); }
    Mod m;
    m.out = &c.bb;
    m.silence(kLeadSec);
    uint32_t scr = 0;
    for (const PacketGenFrame& g : c.frames) {
        const std::vector<uint8_t> bytes = ax25::buildFrame(g.frame);
        if (g.baud == 1200) {
            const std::vector<uint8_t> bits = ax25::hdlcBits(bytes, 24, 4);
            send1200(m, ax25::nrziEncode(bits));
            m.silence(0.25);
        } else {
            const std::vector<uint8_t> bits = ax25::hdlcBits(bytes, 60, 6);
            send9600(m, ax25::scramble(ax25::nrziEncode(bits), scr));
            m.silence(0.15);
        }
    }
    built[k] = true;
    return c;
}

class PacketSynth : public ModeSynth {
public:
    PacketSynth(const SynthConfig& cfg, double rate) : rate_(rate), cyc_(cycleFor(cfg.modeOpt[0])), noise_(7) {
        // noise per real component for the carrier-to-noise ratio in 25 kHz; the common scale keeps the peaks below 0.9 (the source rounds to 8 bits)
        const double pn = rate / kChanBw / std::pow(10.0, cfg.snrDb / 10.0);
        sigma_ = cfg.snrDb < -100 ? 0.f : (float)std::sqrt(pn / 2);
        scale_ = sigma_ > 0.15f ? 0.15f / sigma_ : 1.f;
        if (sigma_ == 0.f) scale_ = 0.4f;
        const double w = 2 * kPi * (cfg.cfoHz - packetTuning().tuneOffsetHz) / rate;
        step_ = cf32((float)std::cos(w), (float)std::sin(w));
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const std::vector<cf32>& bb = cyc_.bb;
        const double adv = kWork / rate_;
        const size_t len = bb.size();
        for (size_t i = 0; i < n; i++) {
            const size_t i0 = (size_t)pos_;
            const float fr = (float)(pos_ - (double)i0);
            const cf32 a = bb[i0 % len], b = bb[(i0 + 1) % len];
            out[i] = (a + (b - a) * fr) * rot_ * scale_;
            rot_ *= step_;
            pos_ += adv;
            if (pos_ >= (double)len) pos_ -= (double)len;
        }
        rot_ /= std::abs(rot_);
        if (sigma_ > 0.f) noise_.add(out, n, sigma_ * scale_);
    }
private:
    double rate_;
    const Cycle& cyc_;
    genutil::NoiseSource noise_;
    float sigma_ = 0, scale_ = 1;
    cf32 step_, rot_ = cf32(1.f, 0.f);
    double pos_ = 0;
};

} // namespace

std::vector<PacketGenFrame> packetGenCycle(int mode) { return cycleFor(mode).frames; }
double packetGenCycleSec(int mode) { return (double)cycleFor(mode).bb.size() / kWork; }

std::unique_ptr<ModeSynth> makePacketSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < packetTuning().minSampleRate - 1) return nullptr;
    return std::make_unique<PacketSynth>(cfg, sampleRate);
}

} // namespace dect2
