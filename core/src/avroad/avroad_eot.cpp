// AAR end-of-train telemetry (EOT to head of train, 457.9375 MHz) and head-of-train commands (452.9375 MHz): 1200 bit/s AFSK (1200 / 1800 Hz) on NFM. Receive only.
// Frame: bit sync 101010..., 11 bit frame sync 11100010010, 45 data bits, 18 check bits: BCH(63,45) with the generator and the fixed XOR key published in PyEOT
// (Eric Reuter, GPL-3, DEF CON 26). Which tone is which, and the field positions, follow PyEOT; demo3eot.wav from that project is the real-signal test.
#include "avroad_int.h"
#include "dect2/avroad/avroad_proto.h"
#include <cstring>
#include <deque>
#include <unordered_map>

namespace dect2::avroad {

namespace {
constexpr int kAudioRate = 48000, kSps = 40, kHop = 5, kPhases = 8;
constexpr uint32_t kSync = 0b10101011100010010;       // the end of the bit sync, then the 11 bit frame sync
constexpr int kBodyBits = 63;
constexpr uint32_t kGen = 0b1111001101000001111;       // the 19 bit generator, x^18 first
constexpr uint32_t kKey = 0b101011011101110000;

uint32_t remainder(const uint8_t* b, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        r = r << 1 | b[i];
        if (r >> 18 & 1) r ^= kGen;
    }
    return r & 0x3FFFF;
}
}

bool eotCheck(uint8_t* body, int* fixed) {
    static const auto table = [] {
        std::unordered_map<uint32_t, std::pair<int8_t, int8_t>> t;
        uint8_t b[kBodyBits];
        for (int i = 0; i < kBodyBits; i++) {
            memset(b, 0, kBodyBits); b[i] = 1;
            t[remainder(b, kBodyBits)] = {(int8_t)i, (int8_t)-1};
            for (int j = i + 1; j < kBodyBits; j++) { b[j] = 1; t[remainder(b, kBodyBits)] = {(int8_t)i, (int8_t)j}; b[j] = 0; }
        }
        return t;
    }();
    // On the air: d0 .. d44, then c0 .. c17. The check is computed over the data reversed, then XORed with the key.
    uint8_t w[kBodyBits];
    for (int i = 0; i < 45; i++) w[i] = body[44 - i];
    for (int i = 0; i < 18; i++) w[45 + i] = body[45 + i] ^ (kKey >> (17 - i) & 1);
    const uint32_t s = remainder(w, kBodyBits);
    if (fixed) *fixed = 0;
    if (!s) return true;
    auto it = table.find(s);
    if (it == table.end()) return false;
    const int a = it->second.first, c = it->second.second;
    auto flip = [&](int k) { if (k < 45) body[44 - k] ^= 1; else body[k] ^= 1; };
    flip(a); if (c >= 0) flip(c);
    if (fixed) *fixed = c >= 0 ? 2 : 1;
    return true;
}

EotFrame eotParse(const uint8_t* d) {
    auto lsb = [&](int a, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v |= (uint32_t)d[a + i] << i; return v; };
    EotFrame f;
    f.chaining = lsb(0, 2);
    f.batteryCond = d[3] << 1 | d[2];
    f.messageType = d[4] << 2 | d[5] << 1 | d[6];
    f.unitAddress = lsb(7, 17);
    f.pressurePsi = lsb(24, 7);
    f.batteryPercent = (int)(lsb(31, 7) * 100 / 127);
    f.valve = d[39]; f.confirm = d[40]; f.turbine = d[41]; f.motion = d[42]; f.markerBattery = d[43]; f.markerLight = d[44];
    return f;
}

std::string eotText(const EotFrame& f, bool hot) {
    static const char* batt[4] = {"not monitored", "very low", "low", "OK"};
    if (hot) return fmt("message type %d, unit %u", f.messageType, f.unitAddress);
    const char* arm = f.messageType == 7 ? (f.confirm ? "armed" : "arming") : "normal";
    return fmt("brake pipe %d psi, battery %s %d%%, %s, %s, valve %s, marker light %s", f.pressurePsi, batt[f.batteryCond], f.batteryPercent, f.motion ? "moving" : "stopped", arm,
               f.valve ? "open" : "closed", f.markerLight ? "on" : "off");
}

namespace {

// Bits from the FM audio: a non-coherent tone pair over one bit, evaluated at 8 sampling phases
class Eot : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs;
        if (!ch_.configure(fs, kAudioRate, 7500, 12000)) return false;
        reset();
        return true;
    }
    void reset() override {
        ch_.reset(); prev_ = cf32(1, 0); n_ = 0; dc_ = 0; memset(buf_, 0, sizeof buf_); m12_ = m18_ = cf32(0, 0);
        for (auto& p : ph_) { p = Phase{}; }
    }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override {
        ch_.process(x, n, y_);
        for (const cf32& v : y_) {
            const float a = std::arg(v * std::conj(prev_)) * (float)(kAudioRate / (2 * kPi)) / 3000.0f;
            prev_ = v;
            audio(a);
        }
    }
    void fill(AvRoadTelemetry& t) const override {
        t.headline = frames_ ? fmt("%llu frames, last unit %u", (unsigned long long)frames_, lastUnit_) : (hot() ? std::string("listening for head-of-train commands") : std::string("listening for end-of-train telemetry"));
        t.facts.push_back({"link", hot() ? "head of train to end of train, 452.9375 MHz" : "end of train to head of train, 457.9375 MHz"});
        t.facts.push_back({"frames repaired", fmt("%llu", (unsigned long long)repaired_)});
        t.facts.push_back({"syncs", fmt("%llu", (unsigned long long)syncs_)});
    }
    // FM audio at 48 kHz (also what a recording provides)
    void audio(float a) {
        dc_ += (a - dc_) * 0.002f;                          // a carrier offset is a DC term here; 1800 Hz is not orthogonal to it over one bit
        a -= dc_;
        const int i = (int)(n_ % kSps);
        const float old = buf_[i]; buf_[i] = a;
        const double t = (double)(n_ % 48000) / kAudioRate;
        const cf32 r12((float)std::cos(2 * kPi * 1200 * t), (float)-std::sin(2 * kPi * 1200 * t)), r18((float)std::cos(2 * kPi * 1800 * t), (float)-std::sin(2 * kPi * 1800 * t));
        // sliding sums: over the 40 sample window 1200 Hz turns once (the leaving sample used the same rotator) and 1800 Hz one and a half times (the opposite one)
        m12_ += (a - old) * r12; m18_ += (a + old) * r18;
        n_++;
        if (n_ % kHop == 0) {
            const int p = (int)((n_ / kHop) % kPhases);
            bit(ph_[p], std::abs(m12_) > std::abs(m18_) ? 1 : 0);
        }
    }
private:
    struct Phase { uint32_t reg = 0; int left = 0; uint8_t body[kBodyBits] = {}; };
    bool hot() const { return std::fabs(centre_ - 452.9375e6) < 1e6; }
    void bit(Phase& p, int b) {
        if (p.left > 0) {
            p.body[kBodyBits - p.left] = (uint8_t)b;
            if (--p.left == 0) finish(p);
            return;
        }
        p.reg = (p.reg << 1 | (uint32_t)b) & 0x1FFFF;
        if (__builtin_popcount(p.reg ^ kSync) <= 1) { p.left = kBodyBits; syncs_++; out->energy(); }
    }
    void finish(Phase& p) {
        int fixed = 0;
        uint8_t b[kBodyBits];
        memcpy(b, p.body, kBodyBits);
        if (!eotCheck(b, &fixed)) { out->bad(); return; }
        const double t = out->now();
        std::string key(reinterpret_cast<const char*>(b), 45);
        if (t - lastT_ < 0.05 && key == lastKey_) return;      // the same frame through a neighbouring sampling phase
        lastT_ = t; lastKey_ = key;
        frames_++; if (fixed) repaired_++;
        const EotFrame f = eotParse(b);
        lastUnit_ = f.unitAddress;
        out->good();
        out->row(hot() ? "Head of train" : "End of train", fmt("%05u", f.unitAddress), eotText(f, hot()), 0);
    }
    double fs_ = 0, centre_ = 0;
    Chain ch_;
    std::vector<cf32> y_;
    cf32 prev_{1, 0}, m12_{0, 0}, m18_{0, 0};
    float buf_[kSps] = {}, dc_ = 0;
    uint64_t n_ = 0;
    Phase ph_[kPhases];
    uint64_t frames_ = 0, repaired_ = 0, syncs_ = 0;
    uint32_t lastUnit_ = 0;
    double lastT_ = -1;
    std::string lastKey_;
};

} // namespace

std::unique_ptr<Decoder> makeEot() { return std::make_unique<Eot>(); }

} // namespace dect2::avroad
