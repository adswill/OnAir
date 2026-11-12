// FSK200/1000 (Enigma designations F06 and F06a): 200 Bd two-tone FSK with 1000 Hz shift, used for Russian diplomatic and intelligence
// messages. Synchronous 288-bit blocks behind a 32-bit sync word; F06 carries figure groups, F06a files. Ported from Rivet (FSK2001000.java,
// F06a.java, CRC.java):
//   Rivet Copyright (C) 2011 Ian Wraith, GNU General Public License v3 or later, https://github.com/priyom/Rivet
//   This program comes with ABSOLUTELY NO WARRANTY.
// From Rivet: the sync word and its inverse (the shift sense), the block layout (the 11-bit block number, the 16 data bytes spread over
// nibbles of bytes 6 - 23, the CRC-16 over them: polynomial 0x1021, start 0xFFFF, reflected, final XOR 0xFFFF), the divider block, the
// header block every 16 blocks (block count, message count and where the messages start), the three kinds of traffic (F06 figure groups,
// plain ASCII blocks, F06a files with their name, size, file CRC and contents) and when to switch between them. Rivet's F06 and F06a are
// separate modes that hand over to each other; here they are one mode. New here: the tone-pair search over the whole channel, the matched
// tone filters, the symbol clock, a sync word with up to three wrong bits accepted when the CRC then holds, and the correction of one wrong
// bit of a block by the CRC (its distance is 4 for this length: one error is corrected, two are still detected). Bytes 24 - 35 of a block are
// not used by Rivet and are not used here.
#include "x40_rivet.h"

namespace dect2::x40 {
namespace {
using namespace rivet;

constexpr double kShift = 1000, kBaud = 200;
constexpr int kBlockBits = 288;
constexpr uint32_t kSync = 0x7D12B0E6u;   // 01111101000100101011000011100110

// CRC-16 as Rivet's CRC(16, 0x1021, 0xffff, 0xffff, true, true)
uint16_t crc16x25(const uint8_t* d, int n) {
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) { c ^= d[i]; for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); }
    return (uint16_t)(c ^ 0xFFFF);
}

struct Block { int index = 0; uint8_t data[16] = {}; uint16_t crc = 0, calc = 0; bool divider = false; };

// frame: the 36 bytes of a block, first bit of the sync = top bit of frame[0]
Block unpack(const uint8_t* f) {
    Block b;
    int zeros = 0; for (int a = 5; a < 36; a++) zeros += f[a] == 0;
    b.divider = zeros >= 30;
    b.index = (f[4] << 3) | ((f[5] & 224) >> 5);
    uint8_t* d = b.data;
    d[0] = (uint8_t)((f[6] & 0xf0) | ((f[8] >> 4) & 0x0f));
    d[1] = (uint8_t)((f[10] & 0xf0) | ((f[12] >> 4) & 0x0f));
    d[2] = (uint8_t)((f[14] & 0xf0) | ((f[16] >> 4) & 0x0f));
    d[3] = (uint8_t)((f[18] & 0xf0) | ((f[20] >> 4) & 0x0f));
    d[4] = (uint8_t)((f[22] & 0xf0) | (f[6] & 0x0f));
    d[5] = (uint8_t)(((f[8] << 4) & 0xf0) | (f[10] & 0x0f));
    d[6] = (uint8_t)(((f[12] << 4) & 0xf0) | (f[14] & 0x0f));
    d[7] = (uint8_t)(((f[16] << 4) & 0xf0) | (f[18] & 0x0f));
    d[8] = (uint8_t)(((f[20] << 4) & 0xf0) | (f[22] & 0x0f));
    d[9] = (uint8_t)((f[7] & 0xf0) | ((f[9] >> 4) & 0x0f));
    d[10] = (uint8_t)((f[11] & 0xf0) | ((f[13] >> 4) & 0x0f));
    d[11] = (uint8_t)((f[15] & 0xf0) | ((f[17] >> 4) & 0x0f));
    d[12] = (uint8_t)((f[19] & 0xf0) | ((f[21] >> 4) & 0x0f));
    d[13] = (uint8_t)((f[23] & 0xf0) | (f[7] & 0x0f));
    d[14] = (uint8_t)(((f[9] << 4) & 0xf0) | (f[11] & 0x0f));
    d[15] = (uint8_t)(((f[13] << 4) & 0xf0) | (f[15] & 0x0f));
    b.crc = (uint16_t)(((f[17] & 15) << 12) | ((f[19] & 15) << 8) | ((f[21] & 15) << 4) | (f[23] & 15));
    b.calc = crc16x25(b.data, 16);
    return b;
}

class F06Dec : public Continuous {
public:
    void fill(X40Telemetry& t) const override {
        const Fit& f = lastFit();
        if (!locked()) { t.headline = good_ ? fmt("FSK200/1000: %d blocks so far, searching", good_) : std::string("searching for FSK200/1000 tones"); return; }
        static const char* kTx[3] = {"F06 figure groups", "ASCII", "F06a file"};
        t.headline = blocks_ ? fmt("FSK200/1000 %s: %d blocks, %d with a CRC error", kTx[tx_], good_, bad_) : fmt("FSK200/1000 tones found (offset %+.0f Hz), waiting for a block", offsetHz());
        t.facts.push_back({"traffic", kTx[tx_]});
        t.facts.push_back({"tones", fmt("%.0f / %.0f Hz from the dial%s", offsetHz() - kShift / 2, offsetHz() + kShift / 2, inverted_ ? " (reversed)" : "")});
        t.facts.push_back({"signal to noise (3 kHz)", fmt("%.1f dB", 10 * std::log10(std::max(f.snr3k, 1e-6)))});
        t.facts.push_back({"blocks", fmt("%d good (%d corrected), %d CRC errors", good_, fixed_, bad_)});
    }
protected:
    Search search() const override {
        Search s;
        s.tones = {-kShift / 2, kShift / 2}; s.minRatio = 1.25;
        s.halfW = std::min(0.35 * kShift, kBaud); s.guardStep = kShift; s.guards = 0; s.minZ = 9;
        s.dfLo = -3000; s.dfHi = 3000; s.track = 20; s.periodic = false; s.replaySec = 16;
        return s;
    }
    void setup(double rate) override {
        const double T = rate / kBaud;
        bank_.configure(rate, {-kShift / 2, kShift / 2}, (int)std::lround(T));
        clk_.configure(T, 32, 16);
    }
    void restart() override { recent_.clear(); lastIdx_ = -1; bank_.reset(); clk_.reset(); bits_.assign(kBlockBits, 0); soft_.assign(kBlockBits, 0.f); pos_ = 0; nbits_ = 0; hold_ = 0; }
    void lost() override {}
    void sample(cf32 y) override {
        bank_.push(y);
        if (!clk_.push(bank_.maxE())) return;
        const float lo = bank_.e(0), hi = bank_.e(1);
        bits_[(size_t)pos_] = hi > lo ? 1 : 0;   // the low tone is 0 (Rivet, not inverted)
        soft_[(size_t)pos_] = std::fabs(hi - lo) / (hi + lo + 1e-30f);
        pos_ = (pos_ + 1) % kBlockBits; nbits_++;
        if (hold_ > 0) { hold_--; return; }
        if (nbits_ < kBlockBits) return;
        uint32_t w = 0; for (int i = 0; i < 32; i++) w = (w << 1) | bit(i);
        const int dn = ones(w ^ kSync), di = ones(w ^ ~kSync);
        if (dn <= 3) block(false, dn); else if (di <= 3) block(true, di);
    }
private:
    static int ones(uint32_t v) { int c = 0; while (v) { v &= v - 1; c++; } return c; }
    // bit i of the 288-bit window (0 = oldest)
    int bit(int i) const { return bits_[(size_t)((pos_ + i) % kBlockBits)]; }
    float soft(int i) const { return soft_[(size_t)((pos_ + i) % kBlockBits)]; }
    void block(bool inv, int syncErr) {
        uint8_t f[36] = {};
        for (int i = 0; i < kBlockBits; i++) if (bit(i) ^ (inv ? 1 : 0)) f[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        Block b = unpack(f);
        bool fixedOne = false;
        if (!b.divider && b.crc != b.calc) {
            // one wrong bit among the 144 of bytes 6 - 23 (data and CRC): try them, weakest first
            std::vector<int> order; for (int i = 48; i < 192; i++) order.push_back(i);
            std::sort(order.begin(), order.end(), [&](int a, int c) { return soft(a) < soft(c); });
            for (int i : order) {
                f[i >> 3] ^= (uint8_t)(0x80 >> (i & 7));
                const Block t = unpack(f);
                if (t.crc == t.calc && !t.divider) { b = t; fixedOne = true; break; }
                f[i >> 3] ^= (uint8_t)(0x80 >> (i & 7));
            }
        }
        const bool ok = b.divider || b.crc == b.calc;
        if (!ok && syncErr > 1) return;           // a loose sync match is kept only when the CRC confirms it
        hold_ = kBlockBits - 40;
        // the 11-bit block number is outside the CRC: a block exactly one block after the last good one takes the next number
        bool renumbered = false;
        if (ok && !b.divider && lastIdx_ >= 0 && nbits_ - lastBits_ == kBlockBits && b.index != lastIdx_ + 1) { b.index = lastIdx_ + 1; renumbered = true; }
        if (ok && !b.divider) { lastIdx_ = b.index; lastBits_ = nbits_; }
        if (ok && !b.divider) {                   // the same block again (a sender repeats blocks): shown once
            for (int i : recent_) if (i == b.index) return;
            recent_.push_back(b.index); if (recent_.size() > 8) recent_.erase(recent_.begin());
        }
        inverted_ = inv;
        const float s = (float)(10 * std::log10(std::max(lastFit().snr3k, 1e-6)));
        if (b.divider) { out->row("Divider", "", "divider block", s); return; }
        blocks_++;
        if (ok) { good_++; if (fixedOne) fixed_++; out->good(s, offsetHz()); } else { bad_++; out->bad(); }
        const char* crcTxt = ok ? (fixedOne ? "CRC OK (one bit corrected)" : renumbered ? "CRC OK (block number from the one before)" : "CRC OK") : "CRC error";
        const uint8_t* d = b.data;
        const std::string id = fmt("#%d", b.index);
        if (b.index % 16 == 0) {
            const int frameCount = (d[0] << 3) | (d[1] >> 5), messageCount = d[1] & 0x1f;
            const bool hv = ok && messageCount <= 7;   // there cannot be more than 7 messages
            std::string msg = fmt("%d blocks, %d message(s) | %s", frameCount, messageCount, hv ? crcTxt : "CRC error");
            if (hv) {
                for (int i = 0; i < 8; i++) pos8_[i] = 0;
                std::string list;
                for (int i = 0; i < 7; i++) {
                    pos8_[i] = (d[2 + 2 * i] << 3) | (d[3 + 2 * i] >> 5);
                    if (!pos8_[i]) break;
                    list += fmt("%sMessage %d at block #%d", list.empty() ? "" : ", ", i + 1, pos8_[i]);
                }
                if (!list.empty()) msg += " | " + list;
            }
            out->row("Header", id, msg, s);
            return;
        }
        // which traffic this is (Rivet: block 1 tells; an F06 block with impossible figures means F06a)
        if (b.index == 1 && d[0] == 0x34 && d[1] == 0x36) tx_ = 1;
        else if (b.index == 1 && d[0] == 0x1b) tx_ = 0;
        else if (tx_ == 0 && ((b.index == 1 && d[0] == 0 && d[1] == 0) || (b.index > 1 && ok && !figuresValid(d)))) { tx_ = 2; ascii_ = true; }
        if (tx_ == 0) {
            int g[14];
            g[0] = (d[0] << 2) | (d[1] >> 6); g[1] = ((d[1] << 4) & 0x3ff) | (d[2] >> 4); g[2] = ((d[2] << 6) & 0x3ff) | (d[3] >> 2); g[3] = ((d[3] << 8) & 0x3ff) | d[4];
            g[4] = (d[5] << 2) | (d[6] >> 6); g[5] = ((d[6] << 4) & 0x3ff) | (d[7] >> 4); g[6] = ((d[7] << 6) & 0x3ff) | (d[8] >> 2); g[7] = ((d[8] << 8) & 0x3ff) | d[9];
            g[8] = (d[10] << 2) | (d[11] >> 6); g[9] = ((d[11] << 4) & 0x3ff) | (d[12] >> 4); g[10] = ((d[12] << 6) & 0x3ff) | (d[13] >> 2); g[11] = ((d[13] << 8) & 0x3ff) | d[14];
            g[12] = d[15] >> 4; g[13] = d[15] & 0x0f;
            std::string t;
            for (int i = 0; i < 12; i++) t += fmt("%03d", g[i]);
            t += fmt("%d%d | %s", g[12], g[13], crcTxt);
            out->row("Block", id, t, s);
        } else if (tx_ == 1) {
            std::string t; for (int i = 0; i < 16; i++) t += printable(d[i]);
            out->row("Block", id, t + " | " + crcTxt, s);
        } else {
            if (isHeader(b.index)) {
                std::string name;
                for (int i = 4; i < 16; i++) { if (d[i] == 0) continue; name += (d[i] < 32 || d[i] >= 127) ? fmt("[0x%02x]", d[i]) : std::string(1, (char)d[i]); }
                const unsigned size = ((unsigned)d[0] << 24) | ((unsigned)d[1] << 16) | ((unsigned)d[2] << 8) | d[3];
                out->row("File", id, fmt("file named %s, size %u bytes | %s", name.c_str(), size, crcTxt), s);
                return;
            }
            int from = 0;
            if (isHeader(b.index - 1)) { out->row("File", id, fmt("CRC of the file contents 0x%02x%02x%02x%02x | %s", d[0], d[1], d[2], d[3], crcTxt), s); from = 4; }
            std::string t; int nonPrint = 0;
            for (int i = from; i < 16; i++) {
                if (!ascii_) { t += fmt("%02x", d[i]); continue; }
                if (d[i] == 0 && ok) { t += "<EOF>"; break; }
                if (d[i] == 13 && ok) t += "<CR>";
                else if (d[i] == 10 && ok) t += "<LF>";
                else if (d[i] < 32 || d[i] >= 127) { t += fmt("[0x%02x]", d[i]); nonPrint++; }
                else t += (char)d[i];
            }
            if (ascii_ && nonPrint > 10 && ok) ascii_ = false;   // Rivet: not an ASCII file, show the rest as hex
            out->row("Block", id, t + " | " + crcTxt, s);
        }
    }
    static bool figuresValid(const uint8_t* d) {
        const int g[12] = {(d[0] << 2) | (d[1] >> 6), ((d[1] << 4) & 0x3ff) | (d[2] >> 4), ((d[2] << 6) & 0x3ff) | (d[3] >> 2), ((d[3] << 8) & 0x3ff) | d[4],
                           (d[5] << 2) | (d[6] >> 6), ((d[6] << 4) & 0x3ff) | (d[7] >> 4), ((d[7] << 6) & 0x3ff) | (d[8] >> 2), ((d[8] << 8) & 0x3ff) | d[9],
                           (d[10] << 2) | (d[11] >> 6), ((d[11] << 4) & 0x3ff) | (d[12] >> 4), ((d[12] << 6) & 0x3ff) | (d[13] >> 2), ((d[13] << 8) & 0x3ff) | d[14]};
        for (int v : g) if (v > 999) return false;
        return (d[15] >> 4) <= 9 && (d[15] & 15) <= 9;
    }
    static std::string printable(uint8_t c) { return c >= 32 && c < 127 ? std::string(1, (char)c) : std::string("."); }
    bool isHeader(int index) const { for (int i = 0; i < 8; i++) if (pos8_[i] != 0 && pos8_[i] == index) return true; return false; }
    ToneBank bank_; SymbolClock clk_;
    std::vector<uint8_t> bits_; std::vector<float> soft_;
    int pos_ = 0, hold_ = 0; long nbits_ = 0;
    int tx_ = 0, blocks_ = 0, good_ = 0, bad_ = 0, fixed_ = 0; bool ascii_ = true, inverted_ = false;
    int pos8_[8] = {};
    std::vector<int> recent_; int lastIdx_ = -1; long lastBits_ = 0;
};

} // namespace

std::unique_ptr<Decoder> makeF06() { return std::make_unique<F06Dec>(); }

} // namespace dect2::x40
