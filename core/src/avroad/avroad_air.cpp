// FANET (the LoRa receiver of the LoRa (raw) mode, set to the FANET channel) and the OGN tracker protocol (the FLARM air format, with its own sync word and an LDPC code).
#include "avroad_int.h"
#include "dect2/avroad/avroad_proto.h"
#include "dect2/loraraw/loraraw_rx.h"
#include "dect2/loraraw/loraraw_tel.h"
#include <cstring>
#include <map>

namespace dect2::avroad {

#include "ogn_ldpc.inc"

// ---- FANET
namespace {
int32_t s24(const uint8_t* p) { int32_t v = p[0] | p[1] << 8 | p[2] << 16; if (v & 0x800000) v -= 1 << 24; return v; }
}

FanetFrame fanetParse(const uint8_t* p, size_t n) {
    FanetFrame f;
    if (n < 4) return f;
    size_t i = 0;
    const bool ext = p[0] & 0x80;
    f.forward = p[0] & 0x40; f.type = p[0] & 0x3F;
    f.manufacturer = p[1]; f.device = p[2] | p[3] << 8;
    i = 4;
    if (ext) {
        if (n < 5) return f;
        const uint8_t e = p[4]; i = 5;
        f.unicast = e & 0x20; f.hasSignature = e & 0x10;
        if (f.unicast) { if (n < i + 3) return f; f.destManufacturer = p[i]; f.destDevice = p[i + 1] | p[i + 2] << 8; i += 3; }
        if (f.hasSignature) { if (n < i + 4) return f; i += 4; }
    }
    const uint8_t* q = p + i; const size_t m = n - i;
    f.valid = true;
    auto text = [&](const uint8_t* a, size_t l) { std::string s; for (size_t k = 0; k < l; k++) s += a[k] >= 32 && a[k] < 127 ? (char)a[k] : '.'; return s; };
    switch (f.type) {
    case 0: f.text = "acknowledge"; break;
    case 1: {
        if (m < 11) { f.valid = false; break; }
        f.hasPos = true; f.lat = s24(q) / 93206.0; f.lon = s24(q + 3) / 46603.0;
        const uint16_t t = q[6] | q[7] << 8;
        f.online = t >> 15; f.aircraftType = t >> 12 & 7; f.altM = (t & 0x7FF) * (t & 0x800 ? 4 : 1);
        f.speedKmh = (q[8] & 0x7F) * 0.5f * (q[8] & 0x80 ? 5 : 1);
        int c = q[9] & 0x7F; if (c & 0x40) c -= 0x80;
        f.climbMs = c * 0.1f * (q[9] & 0x80 ? 5 : 1);
        f.headingDeg = q[10] * 360.0f / 256;
        static const char* kAc[8] = {"other", "paraglider", "hang glider", "balloon", "glider", "powered aircraft", "helicopter", "UAV"};
        f.text = fmt("%s, %d m, %.1f km/h, climb %.1f m/s, heading %.0f", kAc[f.aircraftType], f.altM, f.speedKmh, f.climbMs, f.headingDeg);
        break;
    }
    case 2: f.text = "name: " + text(q, m); break;
    case 3: f.text = m ? "message: " + text(q + 1, m - 1) : std::string("message"); break;
    case 4: {
        if (m < 1) { f.valid = false; break; }
        const uint8_t h = q[0]; size_t o = 1;
        if (h & 1) o++;
        std::string s = "service";
        if (h & 0x80) s += ", internet gateway";
        if (m >= o + 6 && (h & 0x7E)) { f.hasPos = true; f.lat = s24(q + o) / 93206.0; f.lon = s24(q + o + 3) / 46603.0; o += 6; }
        if ((h & 0x40) && m > o) { s += fmt(", %.1f C", (int8_t)q[o] * 0.5); o++; }
        if ((h & 0x20) && m >= o + 3) {
            auto sp = [&](uint8_t v) { return (v & 0x7F) * 0.2f * (v & 0x80 ? 5 : 1); };
            s += fmt(", wind %.0f deg %.1f km/h gust %.1f", q[o] * 360.0 / 256, sp(q[o + 1]), sp(q[o + 2])); o += 3;
        }
        if ((h & 0x10) && m > o) { s += fmt(", humidity %.0f%%", q[o] * 0.4); o++; }
        if ((h & 0x08) && m >= o + 2) { s += fmt(", %.1f hPa", (q[o] | q[o + 1] << 8) * 0.1 + 430); o += 2; }
        f.text = s;
        break;
    }
    case 7: {
        if (m < 7) { f.valid = false; break; }
        f.hasPos = true; f.lat = s24(q) / 93206.0; f.lon = s24(q + 3) / 46603.0;
        static const char* kG[16] = {"other", "walking", "vehicle", "bike", "boot", "", "", "", "need a ride", "landed well", "", "", "need technical support", "need medical help", "distress call", "distress call automatically"};
        f.text = std::string("on the ground: ") + kG[q[6] >> 4];
        break;
    }
    case 9: f.text = "thermal"; break;
    default: f.text = fmt("type %d, %zu bytes", f.type, m); break;
    }
    return f;
}

// ---- OGN
namespace {
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
void tea(uint32_t* d, bool enc) {
    constexpr int kLoops = 8; constexpr uint32_t kDelta = 0x9e3779b9;
    uint32_t a = d[0], b = d[1];
    if (enc) { uint32_t s = 0; for (int i = 0; i < kLoops; i++) { s += kDelta; a += (b << 4) ^ (b + s) ^ (b >> 5); b += (a << 4) ^ (a + s) ^ (a >> 5); } }
    else { uint32_t s = kDelta * kLoops; for (int i = 0; i < kLoops; i++) { b -= (a << 4) ^ (a + s) ^ (a >> 5); a -= (b << 4) ^ (b + s) ^ (b >> 5); s -= kDelta; } }
    d[0] = a; d[1] = b;
}
uint32_t ur(uint32_t v, int mant) {        // the floating formats: two range bits over mant bits, step 1, 2, 4, 8
    const uint32_t r = v >> mant; v &= (1u << mant) - 1;
    const uint32_t b = 1u << mant;
    if (r == 0) return v;
    if (r == 1) return b + 1 + (v << 1);
    if (r == 2) return 3 * b + 2 + (v << 2);
    return 7 * b + 4 + (v << 3);
}
int sr(uint32_t v, int mant) { const int m = (int)ur(v & ((1u << (mant + 2)) - 1), mant); return v >> (mant + 2) & 1 ? -m : m; }
int chkByte(int row, int byte) { return kParityCheck_n208k160[row][byte >> 2] >> ((byte & 3) * 8) & 0xFF; }
}

void ognWhiten(uint8_t* p) {
    uint32_t d[4]; for (int i = 0; i < 4; i++) d[i] = rd32(p + 4 + 4 * i);
    tea(d, true); tea(d + 2, true);
    for (int i = 0; i < 4; i++) wr32(p + 4 + 4 * i, d[i]);
}
int ognParityErrors(const uint8_t* p) {
    int bad = 0;
    for (int r = 0; r < 48; r++) { int c = 0; for (int b = 0; b < 26; b++) c += __builtin_popcount((unsigned)(p[b] & chkByte(r, b))); bad += c & 1; }
    return bad;
}
void ognParity(uint8_t* p) {
    memset(p + 20, 0, 6);
    for (int r = 0; r < 48; r++) {
        int c = 0;
        for (int b = 0; b < 20; b++) c += __builtin_popcount((unsigned)(p[b] & (kParityGen_n208k160[r][b >> 2] >> ((b & 3) * 8) & 0xFF)));
        if (c & 1) p[20 + r / 8] |= (uint8_t)(1 << (r % 8));
    }
}
bool ognRepair(uint8_t* p, const float* soft, int iters) {
    // Gallager-style bit flipping on the 208 bits, weakest evidence first
    static const auto mapping = [] {
        std::vector<std::vector<int>> v(48);
        for (int r = 0; r < 48; r++) for (int b = 0; b < 26; b++) for (int k = 0; k < 8; k++) if (chkByte(r, b) >> k & 1) v[r].push_back(b * 8 + (7 - k));
        return v;
    }();
    uint8_t b[208];
    for (int i = 0; i < 208; i++) b[i] = p[i / 8] >> (7 - i % 8) & 1;
    for (int it = 0; it < iters; it++) {
        int fail[208] = {}, total = 0;
        for (int r = 0; r < 48; r++) { int par = 0; for (int k : mapping[r]) par ^= b[k]; if (par) { total++; for (int k : mapping[r]) fail[k]++; } }
        if (!total) { memset(p, 0, 26); for (int i = 0; i < 208; i++) p[i / 8] |= (uint8_t)(b[i] << (7 - i % 8)); return true; }
        int best = -1; float bestM = 1e30f;
        for (int i = 0; i < 208; i++) if (fail[i] >= 2) { const float m = std::fabs(soft[i]) * 0.5f / (float)fail[i]; const float q = m + ((soft[i] > 0) == (b[i] != 0) ? 0 : -1); if (q < bestM) { bestM = q; best = i; } }
        if (best < 0) for (int i = 0; i < 208; i++) if (fail[i] && std::fabs(soft[i]) < bestM) { bestM = std::fabs(soft[i]); best = i; }
        if (best < 0) return false;
        b[best] ^= 1;
    }
    return false;
}
OgnPacket ognParse(const uint8_t* in) {
    OgnPacket o;
    uint32_t w[5]; for (int i = 0; i < 5; i++) w[i] = rd32(in + 4 * i);
    const uint32_t h = w[0];
    o.address = h & 0xFFFFFF; o.addrType = h >> 24 & 3; o.other = h >> 26 & 1; o.relay = h >> 28 & 3; o.encrypted = h >> 30 & 1; o.emergency = h >> 31 & 1;
    o.addrParityOk = !(__builtin_popcount(h & 0x0FFFFFFF) & 1);
    if (o.encrypted) return o;
    tea(w + 1, false); tea(w + 3, false);
    int32_t lat = (int32_t)(w[1] << 8) >> 8, lon = (int32_t)(w[2] << 8) >> 8;
    o.lat = (lat * 8 + 4) * 0.0001 / 60; o.lon = (lon * 16 + 8) * 0.0001 / 60;
    o.second = w[1] >> 24 & 0x3F; o.fixQuality = w[1] >> 30;
    o.altM = (int)ur(w[3] & 0x3FFF, 12); o.speedMs = (float)ur(w[3] >> 14 & 0x3FF, 8) * 0.1f; o.turnDegS = (float)sr(w[3] >> 24 & 0xFF, 5) * 0.1f;
    o.headingDeg = (float)(((w[4] & 0x3FF) * 3600 + 512) >> 10) * 0.1f; o.climbMs = (float)sr(w[4] >> 10 & 0x1FF, 6) * 0.1f; o.stealth = w[4] >> 19 & 1; o.acftType = w[4] >> 20 & 0xF;
    return o;
}

namespace {

// ---- FANET through the LoRa receiver
class FanetDec : public Decoder {
public:
    bool configure(double fs) override { fs_ = fs; lr_.configure(fs); apply(); return lr_.ready(); }
    void reset() override { lr_.reset(); }
    void setCenterHz(double hz) override { centre_ = hz; apply(); }
    void feed(const cf32* x, size_t n) override {
        lr_.feed(x, n);
        LoraRawTelemetry t;
        if (!lr_.telemetry(t, seq_)) return;
        seq_ = t.seq;
        if (t.state >= 1) out->energy();
        for (auto it = t.log.rbegin(); it != t.log.rend(); ++it) {
            if (it->sec <= lastSec_) continue;
            lastSec_ = it->sec;
            if (!it->crcOk) { out->bad(); continue; }
            const FanetFrame f = fanetParse(it->payload.data(), it->payload.size());
            if (!f.valid) { out->bad(); continue; }
            frames_++;
            out->good(it->snrDb, it->cfoHz);
            const double ll[2] = {f.lat, f.lon};
            out->row(f.type == 1 ? "Tracking" : f.type == 7 ? "Ground" : f.type == 2 ? "Name" : f.type == 3 ? "Message" : f.type == 4 ? "Service" : "Other",
                     fmt("%02X:%04X", f.manufacturer, f.device), f.text, it->levelDb, f.hasPos ? ll : nullptr);
        }
    }
    void fill(AvRoadTelemetry& t) const override {
        t.headline = frames_ ? fmt("%llu frames", (unsigned long long)frames_) : std::string("listening for FANET");
        t.facts.push_back({"channel", wide() ? "920.8 MHz, 500 kHz (Americas, Asia-Pacific)" : "868.2 MHz, 250 kHz (Europe)"});
    }
private:
    bool wide() const { return centre_ > 900e6; }
    void apply() { LoraRawSettings s; s.autoSf = false; s.sf = 7; s.autoBw = false; s.bwKhz = wide() ? 500 : 250; s.sync = 0xF1; lr_.setSettings(s); }
    LoraRawReceiver lr_;
    double fs_ = 0, centre_ = 0, lastSec_ = -1;
    uint64_t seq_ = 0, frames_ = 0;
};

// ---- OGN: 868.2 and 868.4 MHz, 100 kchip/s Manchester 2-GFSK, sync 0x0AF3656C (chips AA66 55A5 9699 965A)
constexpr int kSps = 4, kChips = 416, kSyncChips = 64, kRing = 1024;
constexpr uint64_t kSync = 0xAA6655A59699965Aull;
constexpr int kMaxSyncErr = 7;
constexpr double kCh[2] = {-100e3, 100e3};      // from 868.3 MHz
constexpr double kNominalCentre = 868.3e6;

class OgnDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs;
        for (auto& c : ch_) { c.fs = fs; if (!c.chain.configure(fs, 100e3 * kSps, 85e3, 140e3)) return false; c.reset(); }
        place();
        return true;
    }
    void reset() override { for (auto& c : ch_) c.reset(); }
    void setCenterHz(double hz) override { centre_ = hz; if (fs_ > 0) place(); }
    void feed(const cf32* x, size_t n) override { for (int i = 0; i < 2; i++) if (ch_[i].on) process(ch_[i], i, x, n); }
    void fill(AvRoadTelemetry& t) const override {
        t.headline = packets_ ? fmt("%zu trackers, %llu packets", seen_.size(), (unsigned long long)packets_) : std::string("listening on 868.2 and 868.4 MHz");
        t.facts.push_back({"channels", fmt("868.2 MHz %s, 868.4 MHz %s", ch_[0].on ? "on" : "off", ch_[1].on ? "on" : "off")});
        t.facts.push_back({"repaired", fmt("%llu", (unsigned long long)repaired_)});
        t.facts.push_back({"encrypted positions", fmt("%llu counted", (unsigned long long)encrypted_)});
    }
private:
    struct Chan {
        double fs = 0, hz = 0, phase = 0; bool on = true; Chain chain; std::vector<cf32> mix, y; cf32 prev{1, 0};
        float buf[kSps] = {}; int64_t n = 0; float integ = 0;
        struct Ph { uint64_t reg = 0; float ema = 0; int left = 0; bool inv = false; float soft[kChips]; } ph[kSps];
        void reset() { chain.reset(); phase = 0; prev = cf32(1, 0); n = 0; integ = 0; memset(buf, 0, sizeof buf); for (auto& p : ph) { p.reg = 0; p.ema = 0; p.left = 0; } }
    };
    void place() {
        const double c = centre_ > 0 ? centre_ : kNominalCentre;
        for (int i = 0; i < 2; i++) { ch_[i].hz = kNominalCentre + kCh[i] - c; ch_[i].on = std::fabs(ch_[i].hz) + 90e3 < 0.49 * fs_; }
    }
    void process(Chan& c, int idx, const cf32* x, size_t n) {
        c.mix.resize(n);
        double p = c.phase; const double d = -2 * kPi * c.hz / c.fs;
        for (size_t i = 0; i < n; i++) { c.mix[i] = x[i] * cf32((float)std::cos(p), (float)std::sin(p)); p += d; }
        c.phase = std::remainder(p, 2 * kPi);
        c.chain.process(c.mix.data(), n, c.y);
        for (const cf32& v : c.y) {
            const float f = std::arg(v * std::conj(c.prev)) / (float)(2 * kPi * 50e3 / (100e3 * kSps));   // +-1 at the nominal deviation
            c.prev = v;
            const int k = (int)(c.n % kSps);
            c.integ += f - c.buf[k]; c.buf[k] = f;
            c.n++;
            chip(c, idx, c.ph[c.n % kSps], c.integ / kSps);
        }
    }
    void chip(Chan& c, int idx, Chan::Ph& p, float s) {
        if (p.left > 0) {
            p.soft[kChips - p.left] = s;
            if (--p.left == 0) { finish(c, idx, p); p.reg = 0; }
            return;
        }
        p.ema += (s - p.ema) * (1.f / 32);       // the mean of balanced Manchester chips: the carrier offset
        p.reg = p.reg << 1 | (s > p.ema ? 1u : 0u);
        const int e = __builtin_popcountll(p.reg ^ kSync), ei = 64 - e;
        if (e <= kMaxSyncErr || ei <= kMaxSyncErr) { p.left = kChips; p.inv = ei < e; syncs_++; out->energy(); }
    }
    void finish(Chan& c, int idx, Chan::Ph& p) {
        (void)c;
        uint8_t b[26] = {}; float soft[208];
        for (int i = 0; i < 208; i++) {
            float v = p.soft[2 * i + 1] - p.soft[2 * i];      // bit 1 is chips 01
            if (p.inv) v = -v;
            soft[i] = v;
            if (v > 0) b[i / 8] |= (uint8_t)(1 << (7 - i % 8));
        }
        bool fixed = false;
        if (ognParityErrors(b)) { if (!ognRepair(b, soft)) { out->bad(); return; } fixed = true; }
        const double t = out->now();
        std::string key(reinterpret_cast<const char*>(b), 26);
        if (t - lastT_ < 0.004 && key == lastKey_) return;
        lastT_ = t; lastKey_ = key;
        const OgnPacket o = ognParse(b);
        if (!o.addrParityOk) { out->bad(); return; }
        packets_++; if (fixed) repaired_++;
        seen_[(uint32_t)o.addrType << 24 | o.address]++;
        out->good();
        static const char* kType[4] = {"random", "ICAO", "FLARM", "OGN"};
        const std::string id = fmt("%06X %s", o.address, kType[o.addrType]);
        const char* ch = idx ? "868.4" : "868.2";
        if (o.encrypted) { encrypted_++; out->row("Encrypted", id, fmt("%s MHz, position encrypted, not read", ch), 0); return; }
        if (o.other) { out->row("Status", id, fmt("%s MHz, status or information packet", ch), 0); return; }
        static const char* kAc[16] = {"unknown", "glider", "tow plane", "helicopter", "skydiver", "drop plane", "hang glider", "paraglider", "powered aircraft", "jet", "UFO", "balloon", "airship", "UAV", "ground support", "static"};
        const double ll[2] = {o.lat, o.lon};
        out->row("Position", id, fmt("%s, %d m, %.1f m/s, heading %.0f, climb %.1f m/s, relay %d, %s MHz", kAc[o.acftType], o.altM, o.speedMs, o.headingDeg, o.climbMs, o.relay, ch), 0, ll);
    }
    double fs_ = 0, centre_ = 0;
    Chan ch_[2];
    uint64_t packets_ = 0, repaired_ = 0, encrypted_ = 0, syncs_ = 0;
    std::map<uint32_t, uint32_t> seen_;
    double lastT_ = -1;
    std::string lastKey_;
};

} // namespace

std::unique_ptr<Decoder> makeFanet() { return std::make_unique<FanetDec>(); }
std::unique_ptr<Decoder> makeOgn() { return std::make_unique<OgnDec>(); }

} // namespace dect2::avroad
