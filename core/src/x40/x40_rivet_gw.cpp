// GW FSK: the 100 Bd, 200 Hz shift ship-to-shore data system of Globe Wireless on the HF marine bands (shore free-channel markers, ship
// packets with MMSI numbers, position reports, acknowledgements). Ported from Rivet (GW.java):
//   Rivet Copyright (C) 2011 Ian Wraith, GNU General Public License v3 or later, https://github.com/priyom/Rivet
//   This program comes with ABSOLUTELY NO WARRANTY.
// From Rivet (whose authors worked the format out from recordings; nothing about it is published): the mark / space sense (the low tone
// is 1), a packet ends when the signal does; more than 144 bits is a free-channel marker (the pattern 0011100010100011, then 0xF2 three
// times, the station code six times and 0xFF), 64 - 94 bits a packet (10, a 4-bit type, a count bit, a 7-bit subtype, six bytes), 8 - 11
// bits an ACK; the packet types it names, the character table of the text fields, the two digit codings of MMSI numbers (ship and shore),
// the position report assembled from its fragments, and the shore station codes. Rivet's ship-name lookup (its own database) is not here:
// the MMSI is shown. New here: the burst detector, the tone-pair search inside each burst, the matched tone filters and the bit timing
// found over the whole burst, and both senses of the shift tried (the reading that forms a known packet wins).
#include "x40_rivet.h"

namespace dect2::x40 {
namespace {
using namespace rivet;

constexpr double kShift = 200, kBaud = 100;
constexpr int kFrame = 256;

const char* stationName(int id) {
    switch (id) {
    case 0xcc: return "LFI, Rogaland, Norway & 9MG, Georgetown, Malaysia";
    case 0xb8: return "HLF, Seoul, South Korea";
    case 0x9c: return "VCS, Halifax, Canada";
    case 0xae: return "KEJ, Honolulu, Hawaii";
    case 0x5e: return "CPK, Santa Cruz, Bolivia";
    case 0xbe: return "A9M, Hamala, Bahrain";
    case 0x63: return "9HD, Malta";
    case 0xf0: return "XSV, Tianjin, China";
    case 0xc9: return "ZLA, Awanui, New Zealand";
    case 0xd2: return "ZSC, Capetown, RSA";
    case 0xfa: return "KPH, San Francisco, USA";
    case 0xc6: return "WNU, Slidell Radio, USA";
    case 0xf6: return "KHF, Agana, Guam";
    case 0xce: return "KFS, Palo Alto, USA";
    case 0xee: return "LSD836, Buenos Aires, Argentina";
    case 0xde: return "SAB, Goeteborg, Sweden";
    case 0xe3: return "8PO, Bridgetown, Barbados";
    default: return "Unknown";
    }
}

std::string gwChar(int c) {
    switch (c) {
    case 0x60: return "0"; case 0x20: return "1"; case 0x40: return "2"; case 0x00: return "3"; case 0x70: return "4";
    case 0x30: return "5"; case 0x50: return "6"; case 0x10: return "7"; case 0x68: return "8"; case 0x28: return "9";
    case 0x78: case 0x3e: case 0x4e: case 0xf8: return "<";
    case 0x2e: case 0x7c: return ",";
    case 0x5c: return "."; case 0x74: return "$"; case 0x4c: return "*"; case 0x27: return "A";
    case 0x47: case 0x3f: return "B";
    case 0x37: case 0x07: return "E";
    case 0x17: return "G";
    case 0x7f: case 0x63: return "L";
    case 0x5f: return "N";
    case 0x1f: case 0x43: return "O";
    case 0x13: return "W";
    default: return fmt("[0x%x]", c);
    }
}

std::string convertMmsi(int n, bool alt) {
    switch (n & 7) {
    case 0: return "3"; case 1: return "7"; case 2: return alt ? "9" : "1"; case 3: return "5";
    case 4: return "2"; case 5: return "6"; case 6: return alt ? "8" : "0"; default: return "4";
    }
}
std::string convertShore(int n, bool alt) {
    static const char* a[8] = {"7", "3", "5", "1", "6", "2", "4", "0"};
    static const char* b[8] = {"7", "3", "5", "9", "6", "2", "4", "8"};
    return alt ? b[n & 7] : a[n & 7];
}
// Rivet's displayGW_ShipMMSI / displayGW_ShoreMMSI: per byte the low nibble, then the high one
std::string shipMmsi(const std::vector<int>& m, int digits) {
    std::string s;
    for (int a = 0; a < 6 && a < (int)m.size(); a++) {
        const int hn = (m[(size_t)a] & 240) >> 4, ln = m[(size_t)a] & 15, follow = a < 5 && a + 1 < (int)m.size() ? (m[(size_t)a + 1] & 240) >> 4 : 0;
        s += convertMmsi(ln, follow >= 8); if ((int)s.size() == digits) return s;
        s += convertMmsi(hn, ln >= 8); if ((int)s.size() == digits) return s;
    }
    return s;
}
std::string shoreMmsi(const std::vector<int>& m, int digits) {
    std::string s;
    for (int a = 0; a < 6 && a < (int)m.size(); a++) {
        const int hn = (m[(size_t)a] & 240) >> 4, ln = m[(size_t)a] & 15, follow = a < 5 && a + 1 < (int)m.size() ? (m[(size_t)a + 1] & 240) >> 4 : 0;
        s += convertShore(ln, follow < 8); if ((int)s.size() == digits) return s;
        s += convertShore(hn, ln < 8); if ((int)s.size() == digits) return s;
    }
    return s;
}

class GwDec : public Decoder {
public:
    bool configure(double fs) override {
        if (!front_.configure(fs)) return false;
        rate_ = front_.rate(); binHz_ = rate_ / kFrame;
        win_.resize(kFrame); for (int i = 0; i < kFrame; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFrame));
        reset();
        return true;
    }
    void reset() override { front_.reset(); fr_.clear(); prev_.clear(); burst_.clear(); hist_.clear(); inBurst_ = false; on_ = off_ = 0; posFrag_ = 0; posOn_ = false; posRep_.clear(); lastFrag_.clear(); shore_ = false; }
    void feed(const cf32* x, size_t n) override {
        front_.process(x, n, [&](cf32 a) { fr_.push_back(a); if ((int)fr_.size() >= kFrame) { frame(); fr_.clear(); } });
    }
    void fill(X40Telemetry& t) const override {
        t.headline = packets_ || markers_ ? fmt("GW FSK: %d packets, %d channel markers", packets_, markers_) : std::string("listening for GW FSK bursts");
        if (lastTones_ > 0) t.facts.push_back({"tones (last burst)", fmt("%.0f / %.0f Hz from the dial", lastTones_ - kShift / 2, lastTones_ + kShift / 2)});
        if (!lastStation_.empty()) t.facts.push_back({"last shore station", lastStation_});
        t.facts.push_back({"bursts", fmt("%d analysed, %d not understood", bursts_, unknown_)});
    }
private:
    // ---- burst detection: the strongest 310 Hz wide stretch of a 32 ms spectrum against the noise floor of the band
    void frame() {
        std::vector<cf32> a((size_t)kFrame);
        for (int i = 0; i < kFrame; i++) a[(size_t)i] = fr_[(size_t)i] * win_[(size_t)i];
        fftx(a);
        std::vector<double> p((size_t)kFrame);
        for (int i = 0; i < kFrame; i++) p[(size_t)i] = std::norm(a[(size_t)((i - kFrame / 2 + kFrame) % kFrame)]);
        const int w = std::max(3, (int)std::lround(310.0 / binHz_)), lo = (int)(kFrame / 2 - 0.42 * rate_ / binHz_), hi = (int)(kFrame / 2 + 0.42 * rate_ / binHz_);
        double best = 0, s = 0; int at = lo;
        for (int i = lo; i < lo + w; i++) s += p[(size_t)i];
        best = s;
        for (int i = lo + w; i < hi; i++) { s += p[(size_t)i] - p[(size_t)(i - w)]; if (s > best) { best = s; at = i - w + 1; } }
        // inside a burst its end is judged where the burst started (the strongest stretch of plain noise is well above the mean)
        if (inBurst_) { best = 0; for (int a = std::max(lo, at0_ - 2); a <= std::min(hi - w, at0_ + 2); a++) { double q = 0; for (int i = a; i < a + w; i++) q += p[(size_t)i]; best = std::max(best, q); } }
        std::vector<double> v(p.begin() + lo, p.begin() + hi);
        std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
        const double nb = std::max(v[v.size() / 2] / 0.69, 1e-30);   // median of exponential bins / mean
        const double r = best / (nb * w);
        const bool on = r > 3.0, off = r < 1.6;
        if (!inBurst_) {
            if (on) { if (++on_ >= 2) { inBurst_ = true; off_ = 0; burst_ = prev_; at0_ = at; } } else on_ = 0;
        }
        if (inBurst_) {
            burst_.insert(burst_.end(), fr_.begin(), fr_.end());
            if (off) { if (++off_ >= 3) endBurst(); } else off_ = 0;
            if (burst_.size() > (size_t)(12 * rate_)) endBurst();
        } else {
            prev_.insert(prev_.end(), fr_.begin(), fr_.end());
            if (prev_.size() > (size_t)(3 * kFrame)) prev_.erase(prev_.begin(), prev_.end() - 3 * kFrame);
            if (r > 1.6) out->energy();
        }
    }
    void endBurst() {
        inBurst_ = false; on_ = off_ = 0;
        std::vector<cf32> b; b.swap(burst_);
        if ((double)b.size() / rate_ < 0.07) return;
        analyse(b);
    }
    // ---- one burst: the tone pair, the bits, the packet
    void analyse(const std::vector<cf32>& b) {
        Psd p; p.configure(rate_, 1.0 / 64, 0.064);
        for (const cf32& v : b) p.push(v);
        const Fit f = fitComb(p, {-kShift / 2, kShift / 2}, 70, kShift, 1, 6, -3200, 3200, 2);
        if (!f.found) return;
        bursts_++;
        const double T = rate_ / kBaud;
        ToneBank bank; bank.configure(rate_, {-kShift / 2, kShift / 2}, (int)std::lround(T));
        std::vector<float> d(b.size()), e(b.size());
        double ph = 0;
        for (size_t i = 0; i < b.size(); i++) {
            const cf32 lo((float)std::cos(ph), (float)-std::sin(ph)); ph += 2 * kPi * f.df / rate_; if (ph > kPi) ph -= 2 * kPi;
            bank.push(b[i] * lo);
            const float l = bank.e(0), h = bank.e(1);
            d[i] = (l - h) / (l + h + 1e-30f); e[i] = l + h;
        }
        std::vector<float> es(e.begin(), e.end());
        std::nth_element(es.begin(), es.begin() + (long)(es.size() * 9 / 10), es.end());
        const float thr = 0.5f * es[es.size() * 9 / 10];
        // the bit phase: the largest mean |d| where the tones are on
        int bestPh = 0; double bestM = -1;
        for (int q = 0; q < (int)std::ceil(T); q++) {
            double m = 0; int c = 0;
            for (double t = q; t < (double)b.size(); t += T) { const size_t i = (size_t)t; if (e[i] >= thr) { m += std::fabs(d[i]); c++; } }
            if (c && m / c > bestM) { bestM = m / c; bestPh = q; }
        }
        std::string bits; bool started = false; double dsum = 0;
        for (double t = bestPh; t < (double)b.size(); t += T) {
            const size_t i = (size_t)t;
            if (e[i] < thr) { if (started) break; continue; }
            started = true; bits += d[i] > 0 ? '1' : '0'; dsum += std::fabs(d[i]);
        }
        if (bits.size() < 8 || dsum / bits.size() < 0.55) { unknown_++; out->bad(); return; }
        lastTones_ = f.df;
        const float snr = (float)(10 * std::log10(std::max(f.snr3k, 1e-6)));
        std::string inv = bits; for (char& c : inv) c = c == '1' ? '0' : '1';
        for (int off = 0; off <= 1; off++)
            for (const std::string* s : {&bits, &inv}) {
                if ((int)s->size() <= off + 7) continue;
                if (parse(s->substr((size_t)off), snr, f.df)) { out->good(snr, f.df); return; }
            }
        unknown_++; out->bad();
        out->row("Unknown", fmt("%zu bits", bits.size()), bits.size() > 200 ? bits.substr(0, 200) + "..." : bits, snr);   // Rivet's "display bad packets"
    }
    static std::vector<int> bytesFrom(const std::string& s, size_t start) {
        std::vector<int> v;
        for (size_t i = start; i + 8 <= s.size(); i += 8) { int x = 0; for (int k = 0; k < 8; k++) x = (x << 1) | (s[i + (size_t)k] == '1'); v.push_back(x); }
        return v;
    }
    bool parse(const std::string& s, float snr, double df) {
        const size_t n = s.size();
        if (n > 144) {
            const size_t pos = s.find("0011100010100011");
            if (pos == std::string::npos) return false;
            // Rivet needs the byte before the pattern (pos >= 8); its value is only shown, so a first bit lost at the start of the burst is padded
            const std::string ps = pos < 8 ? std::string(8 - pos, '0') + s : s;
            const std::vector<int> fr = bytesFrom(ps, pos < 8 ? 0 : pos - 8);
            if (fr.size() < 18) return false;
            if (!(fr[8] == 0xf2 && fr[9] == 0xf2 && fr[10] == 0xf2 && fr[11] == fr[12] && fr[12] == fr[13] && fr[13] == fr[14] && fr[14] == fr[15] && fr[15] == fr[16] && fr[17] == 0xff)) return false;
            std::string hex; for (int a = 0; a < 18; a++) hex += fmt("%02x ", fr[(size_t)a]);
            lastStation_ = fmt("0x%02x (%s)", fr[12], stationName(fr[12]));
            out->row("Channel marker", fmt("0x%02x", fr[12]), fmt("free channel marker from station 0x%02x (%s) | %s", fr[12], stationName(fr[12]), hex.c_str()), snr);
            markers_++; shore_ = true; (void)df;
            return true;
        }
        if (n > 63 && n < 95) {
            if (s[0] != '1' || s[1] != '0') return false;
            int type = 0, sub = 0;
            for (int i = 2; i <= 5; i++) type = (type << 1) | (s[(size_t)i] == '1');
            const int count = s[6] == '1';
            for (int i = 7; i <= 13; i++) sub = (sub << 1) | (s[(size_t)i] == '1');
            const std::vector<int> m = bytesFrom(s, 14);
            if (m.size() < 6) return false;
            std::string hex; for (int a = 0; a < 6; a++) hex += fmt("%s0x%x", a ? "," : "", m[(size_t)a]);
            const std::string head = fmt("Type=%d Count=%d Subtype=%d (%s) (%s)", type, count, sub, s.substr(0, 14).c_str(), hex.c_str());
            std::string ascii; for (int a = 0; a < 6; a++) ascii += gwChar(m[(size_t)a]);
            const std::string id = fmt("%d/%d", type, sub);
            if (type == 5 && sub == 102) { posRep_.clear(); lastFrag_.clear(); posOn_ = true; posFrag_ = 0; row(id, head, ascii, snr); return true; }
            if (type == 5 && sub == 86) {
                shore_ = false;
                if (ascii.rfind("$", 0) == 0) { posFrag_ = 0; posRep_.clear(); }
                else if (ascii == lastFrag_) return true;
                posRep_ += ascii; lastFrag_ = ascii; posFrag_++;
                row(id, head, ascii, snr); return true;
            }
            if (type == 5 && (sub == 118 || sub == 54)) { row(id, head, ascii, snr); positionLine(ascii, snr); return true; }
            if (type == 2 && sub == 106) { row(id, head, ascii, snr); return true; }
            if (type == 2 && sub == 101) {
                if (m[0] == 0xe2 && m[1] == 0x72 && m[2] == 0xff && m[3] == 0xff && m[4] == 0xe7 && m[5] == 0xe6) {
                    shore_ = true; const std::string full = shoreMmsi(m, 12);
                    row(id, head, full.substr(0, 9) + " | Command=" + (full.size() >= 12 ? full.substr(9, 3) : std::string("?")), snr);
                } else row(id, head, "MMSI : " + shipMmsi(m, 9), snr);
                return true;
            }
            if (type == 5 && sub == 41) {
                const std::string full = shore_ ? shoreMmsi(m, 12) : shipMmsi(m, 12);
                const std::string mmsi = full.substr(0, std::min<size_t>(9, full.size())), cmd = full.size() >= 12 ? full.substr(9, 3) : std::string("?");
                row(id, head, (shore_ ? "Traffic for : MMSI " : "Traffic for : ") + mmsi + " | Command=" + cmd, snr);
                return true;
            }
            if (type == 5 && sub == 63) { row(id, head, "", snr); return true; }
            return false;
        }
        if (n > 7 && n < 12) {
            const std::string c = s.substr(0, 6);
            if (c == "010101" || c == "101010") { out->row("ACK", "", "GW ACK", snr); packets_++; return true; }
        }
        return false;
    }
    void row(const std::string& id, const std::string& head, const std::string& body, float snr) {
        out->row("Packet", id, body.empty() ? head : head + " | " + body, snr);
        packets_++;
    }
    void positionLine(const std::string& last, float snr) {
        if (!posOn_) return;
        posOn_ = false;
        if (posFrag_ < 10) { out->row("Position", "", fmt("position report incomplete: %d fragments", posFrag_), snr); return; }
        if (last != lastFrag_) posRep_ += last;
        out->row("Position", "", posRep_, snr);
    }
    Front front_; double rate_ = kWork, binHz_ = 1;
    std::vector<float> win_; std::vector<cf32> fr_, prev_, burst_; std::deque<double> hist_;
    bool inBurst_ = false; int on_ = 0, off_ = 0, at0_ = 0;
    int packets_ = 0, markers_ = 0, bursts_ = 0, unknown_ = 0, posFrag_ = 0; bool posOn_ = false, shore_ = false;
    double lastTones_ = 0; std::string posRep_, lastFrag_, lastStation_;
};

} // namespace

std::unique_ptr<Decoder> makeGw() { return std::make_unique<GwDec>(); }

} // namespace dect2::x40
