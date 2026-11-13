// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Road: 802.11p / ITS-G5 safety messages (J2735 BSM, ETSI CAM) with the Wi-Fi OFDM receiver at 10 MHz; EN 12253 / EN 12795 DSRC tolling (the HDLC framing only, never any content); and GBAS (see below).
#include "avroad_int.h"
#include "dect2/avroad/avroad_proto.h"
#include "dect2/wifi/wifi_ofdm.h"
#include <cstring>
#include <deque>

namespace dect2::avroad {

namespace {
struct Bits {
    const uint8_t* p; size_t n; size_t pos = 0;
    bool have(size_t k) const { return pos + k <= n * 8; }
    uint64_t get(int k) { uint64_t v = 0; for (int i = 0; i < k; i++) { v = v << 1 | ((p[pos / 8] >> (7 - pos % 8)) & 1u); pos++; } return v; }
};
}

bool bsmParse(const uint8_t* p, size_t n, Bsm& o) {
    if (n < 40 || p[0] != 0x00 || p[1] != 0x14) return false;
    size_t len = p[2], at = 3;
    if (len & 0x80) { if (!(len & 0x40) && n >= 4) { len = (len & 0x3F) << 8 | p[3]; at = 4; } else return false; }
    if (len < 30 || at + len > n) return false;
    Bits b{p + at, len};
    b.get(3);        // extension, partII and regional present
    if (!b.have(7 + 32 + 16 + 31 + 32 + 16 + 32 + 3 + 13 + 16)) return false;
    o.msgCnt = (int)b.get(7); o.id = (uint32_t)b.get(32); b.get(16);
    o.lat = (int)b.get(31) - 900000000; o.lon = (int)((int64_t)b.get(32) - 1799999999); o.elev = (int)b.get(16) - 4096;
    b.get(32); b.get(3);        // accuracy, transmission state
    o.speed = (int)b.get(13); o.heading = (int)b.get(15);      // the published example fixes this at 15 bits
    return true;
}

bool camParse(const uint8_t* p, size_t n, Cam& o) {
    if (n < 28) return false;
    Bits b{p, n};
    o.protocol = (int)b.get(8); const int msg = (int)b.get(8); o.station = (uint32_t)b.get(32);
    if (msg != 2 || o.protocol < 1 || o.protocol > 3) return false;
    b.get(16);       // generationDeltaTime
    b.get(3);        // CamParameters: extension, two optional containers
    b.get(1);        // BasicContainer: extension
    o.stationType = (int)b.get(8);
    o.lat = (int)((int64_t)b.get(31) - 900000000); o.lon = (int)((int64_t)b.get(32) - 1800000000);
    return true;
}

std::string itsFrameText(const uint8_t* m, size_t n, std::string& kind, std::string& id, double* ll, bool& hasPos) {
    hasPos = false;
    if (n < 28 || (m[0] & 0x0C) != 0x08) return "";           // a data frame
    size_t h = 24 + (m[0] & 0x80 ? 2 : 0);                     // QoS data has two more bytes
    if (n < h + 8 || m[h] != 0xAA || m[h + 1] != 0xAA) return "";
    const uint16_t eth = (uint16_t)(m[h + 6] << 8 | m[h + 7]);
    const uint8_t* q = m + h + 8; const size_t l = n - h - 8;
    if (eth == 0x88DC) {
        for (size_t i = 0; i + 40 <= l; i++) {
            Bsm b;
            if (q[i] == 0x00 && q[i + 1] == 0x14 && bsmParse(q + i, l - i, b)) {
                kind = "BSM"; id = fmt("%08X", b.id);
                ll[0] = b.lat * 1e-7; ll[1] = b.lon * 1e-7; hasPos = b.lat != 900000001 && b.lon != 1800000001;
                return fmt("message %d, elevation %.1f m, speed %.1f m/s, heading %.1f", b.msgCnt, b.elev * 0.1, b.speed * 0.02, b.heading * 0.0125);
            }
        }
        kind = "WSMP"; id = "-";
        return fmt("%zu bytes, no BSM found", l);
    }
    if (eth == 0x8947) {
        for (size_t i = 0; i + 34 <= l; i++) {
            Cam c;
            if (q[i] == 0x07 && q[i + 1] == 0xD1 && camParse(q + i + 4, l - i - 4, c)) {
                kind = "CAM"; id = fmt("%u", c.station);
                ll[0] = c.lat * 1e-7; ll[1] = c.lon * 1e-7; hasPos = c.lat != 900000001;
                static const char* kSt[16] = {"unknown", "pedestrian", "cyclist", "moped", "motorcycle", "car", "bus", "light truck", "heavy truck", "trailer", "special vehicle", "tram", "", "", "", "roadside unit"};
                return fmt("station type %s", c.stationType < 16 ? kSt[c.stationType] : "?");
            }
            if (q[i] == 0x07 && q[i + 1] == 0xD2) { kind = "DENM"; id = "-"; return "hazard warning (counted, not decoded)"; }
        }
        kind = "GeoNetworking"; id = "-";
        return fmt("%zu bytes, no CAM found", l);
    }
    return "";
}

uint16_t dsrcFcs(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = c & 1 ? (uint16_t)(c >> 1 ^ 0x8408) : (uint16_t)(c >> 1); }
    return (uint16_t)~c;
}
bool dsrcFrame(const uint8_t* p, size_t n, DsrcFrame& o) {
    if (n < 4) return false;
    if (dsrcFcs(p, n - 2) != (uint16_t)(p[n - 2] | p[n - 1] << 8)) return false;
    o.bytes = n - 2;
    size_t i = 0;
    while (i < n - 2 && !(p[i] & 1)) i++;        // the link identifier ends with a byte whose lowest bit is set
    o.lidBytes = (int)i + 1;
    o.broadcast = n >= 6 && p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF;
    o.control = i + 1 < n - 2 ? p[i + 1] : 0;
    return true;
}

namespace {

class ItsDec : public Decoder {
public:
    bool configure(double fs) override {
        if (!ch_.configure(fs, 10e6, 4.6e6, 5.0e6)) return false;
        rx_.setAllowQpsk(true);
        rx_.setCallback([this](const uint8_t* m, size_t n, const wifi::ofdm::Info& in) { frame(m, n, in); });
        return true;
    }
    void reset() override { ch_.reset(); rx_.reset(); }
    void feed(const cf32* x, size_t n) override { ch_.process(x, n, y_); rx_.feed(y_.data(), y_.size()); }
    void fill(AvRoadTelemetry& t) const override {
        t.headline = frames_ ? fmt("%llu safety messages", (unsigned long long)msgs_) : std::string("listening on the 10 MHz ITS channel");
        const auto& s = rx_.stats();
        t.facts.push_back({"frames", fmt("%llu with a good checksum, %llu bad", (unsigned long long)s.framesOk, (unsigned long long)s.framesBad)});
        t.facts.push_back({"note", "J2735 layout checked against a published example; CAM against the public ETSI ASN.1"});
    }
private:
    void frame(const uint8_t* m, size_t n, const wifi::ofdm::Info& in) {
        frames_++;
        std::string k, id; double ll[2] = {}; bool pos = false;
        const std::string txt = itsFrameText(m, n, k, id, ll, pos);
        if (txt.empty()) { out->energy(); return; }
        msgs_++;
        out->good(in.snrDb, in.cfoHz * 0.5);
        out->row(k.c_str(), id, txt, in.rssiDb, pos ? ll : nullptr);
    }
    Chain ch_;
    wifi::ofdm::OfdmRx rx_;
    std::vector<cf32> y_;
    uint64_t frames_ = 0, msgs_ = 0;
};

// ---- DSRC downlink: ASK at 500 kbit/s, FM0. HDLC flags, bit stuffing and the FCS are checked; nothing of the content is kept or shown.
class DsrcDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs; spb_ = fs / 500e3;
        if (spb_ < 2) return false;
        reset();
        return true;
    }
    void reset() override { avg_ = 0; last_ = 0; since_ = 0; shift_ = 0; inFrame_ = false; frameBits_.clear(); ones_ = 0; have_ = false; }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            const float e = std::abs(x[i]);
            avg_ += (e - avg_) * (e > 2 * avg_ ? 0.05f : 0.002f);      // quick attack: a beacon starts from silence
            const int lvl = e > avg_ * 1.0f ? 1 : 0;
            since_++;
            if (lvl != last_) {
                last_ = lvl;
                const double t = since_ / spb_;
                since_ = 0;
                edge(t);
            }
        }
    }
    void fill(AvRoadTelemetry& t) const override {
        t.headline = frames_ ? fmt("%llu frames (%llu broadcast)", (unsigned long long)frames_, (unsigned long long)broadcast_) : std::string("listening for roadside beacons");
        t.facts.push_back({"content", "not decoded and not stored: frame shape only"});
    }
private:
    void edge(double intervalBits) {
        if (intervalBits > 1.25 || intervalBits < 0.3) { have_ = false; inFrame_ = false; return; }
        if (intervalBits > 0.75) { bit(1); have_ = false; return; }           // a full bit between boundaries
        if (have_) { bit(0); have_ = false; } else have_ = true;              // a half bit: two make a zero
    }
    void bit(int b) {
        shift_ = (uint8_t)(shift_ >> 1 | b << 7);
        if (shift_ == 0x7E) {
            if (inFrame_ && frameBits_.size() >= 32) close();
            inFrame_ = true; frameBits_.clear(); ones_ = 0; out->energy();
            return;
        }
        if (!inFrame_) return;
        if (b) { ones_++; frameBits_.push_back(1); if (frameBits_.size() > 8 * 400) inFrame_ = false; }
        else {
            if (ones_ == 5) { ones_ = 0; return; }        // a stuffed zero
            ones_ = 0; frameBits_.push_back(0);
        }
    }
    void close() {
        size_t have = frameBits_.size();
        if (have >= 7) have -= 7;                         // the flag's own first seven bits came in with the data
        const size_t nb = have / 8;
        if (nb < 4 || nb > 300) { out->bad(); return; }
        std::vector<uint8_t> f(nb);
        for (size_t i = 0; i < nb * 8; i++) f[i / 8] |= (uint8_t)(frameBits_[i] << (i % 8));
        DsrcFrame d;
        if (!dsrcFrame(f.data(), nb, d)) { out->bad(); return; }
        frames_++; if (d.broadcast) broadcast_++;
        out->good();
        out->row(d.broadcast ? "Beacon broadcast" : "Private link", "-", fmt("%zu bytes, control %02X (%s link identifier, value not shown)", d.bytes, (unsigned)d.control, d.broadcast ? "broadcast" : "private"), 0);
    }
    double fs_ = 0, spb_ = 8;
    float avg_ = 0; int last_ = 0; double since_ = 0; uint8_t shift_ = 0; bool inFrame_ = false, have_ = false; int ones_ = 0;
    std::vector<uint8_t> frameBits_;
    uint64_t frames_ = 0, broadcast_ = 0;
};

// ---- GBAS VDB: bursts in 62.5 ms slots of a 500 ms frame, D8PSK at 10 500 symbols/s. The burst layout (training sequence, scrambler) is only in ICAO Annex 10 Volume I and RTCA DO-246,
// neither of which is public, so this is a detector: the slot pattern and the symbol rate line. Nothing of the message content is claimed.
class GbasDec : public Decoder {
public:
    bool configure(double fs) override { if (!ch_.configure(fs, 100e3, 13e3, 20e3)) return false; reset(); return true; }
    void reset() override { ch_.reset(); n_ = 0; noise_ = 0; inB_ = false; bursts_.clear(); len_ = 0; line_ = ref_ = cf32(0, 0); memset(ma_, 0, sizeof ma_); maSum_ = 0; maW_ = 0; }
    void feed(const cf32* x, size_t n) override {
        ch_.process(x, n, y_);
        for (const cf32& v : y_) {
            const float p = std::norm(v);
            maSum_ += p - ma_[maW_]; ma_[maW_] = p; maW_ = (maW_ + 1) % kMa;
            const float pa = maSum_ / kMa;                 // 1 ms average, so noise peaks do not join into bursts
            if (n_ < 4000) { if (n_ >= 2000) noise_ += pa; if (n_ == 3999) noise_ /= 2000; n_++; continue; }
            const bool on = pa > noise_ * 4;
            if (on && !inB_) { inB_ = true; startAt_ = n_; acc_ = 0; line_ = ref_ = cf32(0, 0); }
            if (inB_) {
                const double t = (double)n_ / 100e3;
                line_ += p * std::polar(1.f, (float)(-2 * kPi * 10500 * t));
                ref_ += p * std::polar(1.f, (float)(-2 * kPi * 7300 * t));
                acc_ += p; lowCount_ = on ? 0 : lowCount_ + 1;
                if (lowCount_ > 200) { inB_ = false; close(n_ - 200); }
            } else noise_ += (p - noise_) * 2e-5f;
            n_++;
        }
    }
    void fill(AvRoadTelemetry& t) const override {
        t.headline = frames_ ? fmt("%d of 8 slots busy, symbol rate %s", slots_, symOk_ ? "10 500 /s seen" : "not confirmed") : std::string("listening for VDB bursts");
        t.facts.push_back({"content", "not decoded: the burst format is only in ICAO Annex 10 and RTCA DO-246, which are not public"});
        t.facts.push_back({"bursts", fmt("%llu", (unsigned long long)total_)});
    }
private:
    void close(int64_t endAt) {
        const double dur = (double)(endAt - startAt_) / 100e3;
        if (dur < 0.005 || dur > 0.08) return;
        total_++;
        const double t = (double)startAt_ / 100e3;
        const bool sym = std::abs(line_) > 3 * std::abs(ref_);
        if (sym) symOk_ = true;
        bursts_.push_back(t);
        while (!bursts_.empty() && t - bursts_.front() > 2.0) bursts_.pop_front();
        std::vector<int> used(8, 0);
        const double t0 = bursts_.front();
        for (double b : bursts_) used[(int)(std::fmod(b - t0 + 0.0001, 0.5) / 0.0625) % 8] = 1;
        int s = 0; for (int u : used) s += u;
        slots_ = s;
        frames_++;
        out->good(toDb(acc_ / (float)std::max<int64_t>(endAt - startAt_, 1) / std::max(noise_, 1e-12f)));
        if (frames_ % 16 == 1) out->row("Bursts", fmt("%.1f ms", dur * 1e3), fmt("%d of 8 slots busy, symbol rate %s", slots_, symOk_ ? "10 500 /s seen" : "not confirmed"), 0);
    }
    static constexpr int kMa = 100;
    float ma_[kMa] = {}, maSum_ = 0; int maW_ = 0;
    Chain ch_; std::vector<cf32> y_;
    int64_t n_ = 0, startAt_ = 0; float noise_ = 0, acc_ = 0; bool inB_ = false, symOk_ = false; int lowCount_ = 0, slots_ = 0, len_ = 0;
    cf32 line_{0, 0}, ref_{0, 0};
    std::deque<double> bursts_;
    uint64_t total_ = 0, frames_ = 0;
};

} // namespace

std::unique_ptr<Decoder> makeIts() { return std::make_unique<ItsDec>(); }
std::unique_ptr<Decoder> makeDsrc() { return std::make_unique<DsrcDec>(); }
std::unique_ptr<Decoder> makeGbas() { return std::make_unique<GbasDec>(); }

} // namespace dect2::avroad
