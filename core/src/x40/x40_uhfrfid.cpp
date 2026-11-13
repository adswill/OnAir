// UHF RFID, EPC Class-1 Gen-2 / ISO 18000-63 (860 - 960 MHz): the commands of the READER. The reader modulates its carrier with pulse-interval encoding: every
// symbol ends in a low pulse (PW), data-0 is one Tari long, data-1 between 1.5 and 2 Tari. A frame is a delimiter (12.5 us low), data-0, RTcal (a 0 plus a 1),
// for Query also TRcal, then the bits; a symbol shorter than RTcal / 2 is a 0. Query (1000 + DR, M, TRext, Sel, Session, Target, Q, CRC-5), QueryRep, QueryAdjust,
// ACK (RN16), NAK, Select (mask, CRC-16), Req_RN, Read, Write, Kill, Lock, Access are named and their checksums tested; frames that fail their CRC are
// counted as bad and not listed. The tag's reply (FM0 / Miller backscatter, 40 - 640 kHz) is far weaker than the reader and is NOT decoded here. Receive only.
#include "x40_int.h"
#include <array>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 1.0e6;

class UhfDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        const double r = fs / d;
        if (!dec_.configure(fs, d, std::min(300000.0, 0.45 * r))) return false;
        rate_ = dec_.outRate(); reset(); return true;
    }
    void reset() override {
        dec_.reset(); n_ = 0; y_ = 0; hi_ = lo_ = 0; init_ = false; low_ = false; fall_ = 0; riseLast_ = 0; inFrame_ = false; syms_.clear();
        okCmds_ = badCmds_ = 0; tari_ = rtcal_ = trcal_ = 0; lastCmd_.clear(); cw_ = 0;
        counts_ = {};
    }
    void feed(const cf32* x, size_t n) override { dec_.process(x, n, [&](cf32 a) { step(std::abs(a)); }); }
    void fill(X40Telemetry& t) const override {
        if (!init_) { t.headline = "waiting for data"; return; }
        t.headline = okCmds_ ? fmt("%d reader commands, last %s", okCmds_, lastCmd_.c_str()) : (cw_ > 0 ? "reader carrier, no command yet" : "no reader carrier");
        t.facts.push_back({"commands", fmt("%d decoded, %d failed their CRC or length", okCmds_, badCmds_)});
        if (tari_ > 0) t.facts.push_back({"Tari (data-0)", fmt("%.2f us", tari_ * 1e6)});
        if (rtcal_ > 0) t.facts.push_back({"RTcal", fmt("%.2f us (%.2f Tari)", rtcal_ * 1e6, rtcal_ / tari_)});
        if (trcal_ > 0) t.facts.push_back({"TRcal", fmt("%.2f us: tag BLF %.0f kHz (DR 8) or %.0f kHz (DR 64/3)", trcal_ * 1e6, 8 / trcal_ / 1e3, 64.0 / 3 / trcal_ / 1e3)});
        std::string s;
        const char* nm[] = {"Query", "QueryRep", "QueryAdjust", "ACK", "NAK", "Select", "Req_RN", "Read", "Write", "Kill", "Lock", "Access"};
        for (int i = 0; i < 12; i++) if (counts_[i]) s += fmt("%s %d  ", nm[i], counts_[i]);
        if (!s.empty()) t.facts.push_back({"by command", s});
    }
private:
    void step(float e) {
        n_++;
        const double t = n_ / rate_;
        y_ += 0.6f * (e - y_);
        if (!init_) { init_ = true; hi_ = lo_ = y_; }
        const float dh = 1.f / (float)(0.002 * rate_);
        if (y_ > hi_) hi_ = y_; else hi_ -= dh * (hi_ - y_);
        if (y_ < lo_) lo_ = y_; else lo_ += 0.2f * dh * (y_ - lo_);
        const float mid = 0.5f * (hi_ + lo_), span = hi_ - lo_;
        if (span > 0.3f * hi_) cw_ = 1;
        const bool wasLow = low_; const float yPrev = yPrevS_; yPrevS_ = y_;
        if (span > 0.3f * std::max(hi_, 1e-9f)) { if (y_ < mid - 0.1f * span) low_ = true; else if (y_ > mid + 0.1f * span) low_ = false; }
        else low_ = false;
        // the time the envelope crossed the middle, interpolated between two samples (a sample is a quarter to a half of the shortest pulse)
        if ((yPrev >= mid) != (y_ >= mid)) {
            const float d = y_ - yPrev; const double tc = std::fabs(d) > 1e-12f ? t - (1.0 - (mid - yPrev) / d) / rate_ : t;
            if (y_ < mid) lastFall_ = tc; else lastRise_ = tc;
        }
        if (low_ && !wasLow) fall_ = lastFall_ > 0 ? lastFall_ : t;
        if (!low_ && wasLow) rise(lastRise_ > fall_ ? lastRise_ : t);
        if (inFrame_ && t - riseLast_ > frameGap()) endFrame();
    }
    double frameGap() const { return syms_.size() >= 2 ? 3.5 * (syms_[1]) : 400e-6; }
    void rise(double t) {
        const double pw = t - fall_;
        if (!inFrame_) {
            if (pw > 9e-6 && pw < 17e-6 && t - riseLast_ > 40e-6) { inFrame_ = true; syms_.clear(); riseLast_ = t; }   // the delimiter: the first rising edge starts data-0
            else riseLast_ = t;
            return;
        }
        const double len = t - riseLast_; riseLast_ = t;
        if (len > 3.5e-4 || len < 2.5e-6) { inFrame_ = false; syms_.clear(); return; }
        syms_.push_back(len);
        if (syms_.size() > 80 + 2 + 1 + 600) { inFrame_ = false; syms_.clear(); }
    }
    static void crcAdd16(uint16_t& c, int bit) { const bool x = ((c >> 15) & 1) ^ bit; c <<= 1; if (x) c ^= 0x1021; }
    static bool crc16ok(const std::vector<uint8_t>& b, size_t n) { uint16_t c = 0xFFFF; for (size_t i = 0; i < n; i++) crcAdd16(c, b[i]); return c == 0x1D0F; }
    static bool crc5ok(const std::vector<uint8_t>& b, size_t n) { uint8_t c = 0x09; for (size_t i = 0; i < n; i++) { const bool x = ((c >> 4) & 1) ^ b[i]; c = (uint8_t)((c << 1) & 0x1F); if (x) c ^= 0x09; } return c == 0; }
    static uint32_t get(const std::vector<uint8_t>& b, size_t o, size_t n) { uint32_t v = 0; for (size_t i = 0; i < n; i++) v = v << 1 | b[o + i]; return v; }

    void endFrame() {
        inFrame_ = false;
        std::vector<double> s; s.swap(syms_);
        if (s.size() < 4) return;
        const double tari = s[0], rtcal = s[1];
        // RTcal is 2.5 to 3 Tari; data-0 6.25 - 25 us
        if (tari < 5.5e-6 || tari > 28e-6 || rtcal < 2.35 * tari || rtcal > 3.2 * tari) { bad(); return; }
        size_t i0 = 2; double trcal = 0;
        if (s.size() > 2 && s[2] > 1.05 * rtcal) { trcal = s[2]; i0 = 3; if (trcal > 3.2 * rtcal) { bad(); return; } }
        const double piv = rtcal * 0.5;
        std::vector<uint8_t> b;
        for (size_t i = i0; i < s.size(); i++) {
            const bool one = s[i] > piv;
            if (one ? (s[i] < 1.35 * tari || s[i] > 2.2 * tari) : (s[i] < 0.85 * tari || s[i] > 1.15 * tari)) { bad(); return; }   // clock and pulse widths must fit the preamble
            b.push_back(one ? 1 : 0);
        }
        tari_ = tari; rtcal_ = rtcal; if (trcal > 0) trcal_ = trcal;
        classify(b, trcal > 0);
    }
    void bad() { badCmds_++; }
    void emit(int idx, const char* kind, const std::string& id, const std::string& text) {
        counts_[idx]++; okCmds_++; lastCmd_ = kind;
        out->good(0, 0);
        out->row(kind, id, text, toDb(y_ * y_));
    }
    void classify(const std::vector<uint8_t>& b, bool hasTrcal) {
        const size_t n = b.size();
        if (n < 4) { bad(); return; }
        const uint32_t c2 = get(b, 0, 2);
        if (n == 4 && c2 == 0) { emit(1, "QueryRep", fmt("session %u", get(b, 2, 2)), fmt("QueryRep, session S%u", get(b, 2, 2))); return; }
        if (n == 18 && c2 == 1) { emit(3, "ACK", fmt("%04X", get(b, 2, 16)), fmt("ACK of the tag random number RN16 %04X", get(b, 2, 16))); return; }
        if (n == 22 && get(b, 0, 4) == 8 && hasTrcal) {
            if (!crc5ok(b, 22)) { bad(); return; }
            const unsigned dr = b[4], m = get(b, 5, 2), trext = b[7], sel = get(b, 8, 2), sess = get(b, 10, 2), tgt = b[12], q = get(b, 13, 4);
            static const char* mn[4] = {"FM0", "Miller 2", "Miller 4", "Miller 8"};
            static const char* sn[4] = {"all", "all", "~SL", "SL"};
            emit(0, "Query", fmt("Q=%u S%u", q, sess),
                 fmt("Query: %u slots (Q=%u), session S%u, target %c, select %s, tag coding %s%s, DR %s", 1u << q, q, sess, tgt ? 'B' : 'A', sn[sel], mn[m], trext ? " with pilot tone" : "", dr ? "64/3" : "8"));
            return;
        }
        if (n == 9 && get(b, 0, 4) == 9) { const unsigned u = get(b, 6, 3); emit(2, "QueryAdjust", fmt("S%u", get(b, 4, 2)), fmt("QueryAdjust, session S%u, Q %s", get(b, 4, 2), u == 6 ? "+1" : u == 3 ? "-1" : u == 0 ? "unchanged" : "reserved")); return; }
        if (n == 8 && get(b, 0, 8) == 0xC0) { emit(4, "NAK", "", "NAK: the tag goes back to arbitrate"); return; }
        if (n >= 8) {
            const uint32_t cmd = get(b, 0, 8);
            if (cmd == 0xC1 && n == 40 && crc16ok(b, n)) { emit(6, "Req_RN", fmt("%04X", get(b, 8, 16)), fmt("Req_RN for handle or RN16 %04X", get(b, 8, 16))); return; }
            if (cmd == 0xC2 && n >= 58 && crc16ok(b, n)) {
                // Read: MemBank 2, WordPtr EBV (8 bit per 7 data bits), WordCount 8, RN 16, CRC 16
                size_t o = 10, ptr = 0; bool more = true; int guard = 0;
                while (more && o + 8 <= n && guard++ < 3) { more = b[o] != 0; ptr = ptr << 7 | get(b, o + 1, 7); o += 8; }
                if (o + 8 + 16 + 16 == n) { static const char* bk[4] = {"Reserved", "EPC", "TID", "User"}; emit(7, "Read", fmt("%s word %zu", bk[get(b, 8, 2)], ptr), fmt("Read %u words from %s memory at word %zu, handle %04X", get(b, o, 8), bk[get(b, 8, 2)], ptr, get(b, o + 8, 16))); return; }
            }
            if (cmd == 0xC3 && n == 8 + 2 + 8 + 16 + 16 + 16 && crc16ok(b, n)) { { static const char* bk[4] = {"Reserved", "EPC", "TID", "User"}; emit(8, "Write", bk[get(b, 8, 2)], fmt("Write of one word to %s memory", bk[get(b, 8, 2)])); }; return; }
            if (cmd == 0xC4 && n == 8 + 16 + 3 + 16 + 16 && crc16ok(b, n)) { emit(9, "Kill", "", "Kill (password phase)"); return; }
            if (cmd == 0xC5 && n == 8 + 20 + 16 + 16 && crc16ok(b, n)) { emit(10, "Lock", fmt("%05X", get(b, 8, 20)), fmt("Lock, mask and action %05X", get(b, 8, 20))); return; }
            if (cmd == 0xC6 && n == 8 + 16 + 16 + 16 && crc16ok(b, n)) { emit(11, "Access", "", "Access (password phase)"); return; }
        }
        if (n >= 4 + 3 + 3 + 2 + 8 + 8 + 1 + 16 && get(b, 0, 4) == 10 && crc16ok(b, n)) {
            const unsigned tgt = get(b, 4, 3), act = get(b, 7, 3), bank = get(b, 10, 2);
            size_t o = 12, ptr = 0; bool more = true; int guard = 0;
            while (more && o + 8 <= n && guard++ < 3) { more = b[o] != 0; ptr = ptr << 7 | get(b, o + 1, 7); o += 8; }
            if (o + 8 <= n) {
                const unsigned len = get(b, o, 8);
                if (o + 8 + len + 1 + 16 == n) {
                    static const char* bk[4] = {"RFU", "EPC", "TID", "User"};
                    std::string m; for (unsigned i = 0; i < len && i < 96; i++) m += b[o + 8 + i] ? '1' : '0';
                    static const char* tg[8] = {"S0", "S1", "S2", "S3", "SL", "RFU", "RFU", "RFU"};
                    emit(5, "Select", fmt("%s bit %zu, %u bits", bk[bank], ptr, len), fmt("Select %s, action %u, %s memory from bit %zu, mask %s%s", tg[tgt], act, bk[bank], ptr, m.c_str(), b[o + 8 + len] ? ", truncate" : ""));
                    return;
                }
            }
        }
        bad();
    }
    Decimator dec_; double rate_ = 1; uint64_t n_ = 0;
    float y_ = 0, yPrevS_ = 0, hi_ = 0, lo_ = 0; bool init_ = false, low_ = false; double fall_ = 0, riseLast_ = 0, lastFall_ = 0, lastRise_ = 0; bool inFrame_ = false; int cw_ = 0;
    std::vector<double> syms_;
    int okCmds_ = 0, badCmds_ = 0; double tari_ = 0, rtcal_ = 0, trcal_ = 0; std::string lastCmd_; std::array<int, 12> counts_{};
};
}

std::unique_ptr<Decoder> makeUhfRfid() { return std::make_unique<UhfDec>(); }

} // namespace dect2::x40
