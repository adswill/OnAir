// CROWD36: 34-tone MFSK, 40 Bd, tones 40 Hz apart from 1410 to 2730 Hz in the upper sideband, used for Russian diplomatic and intelligence
// messages. Ported from Rivet (CROWD36.java):
//   Rivet Copyright (C) 2011 Ian Wraith, GNU General Public License v3 or later, https://github.com/priyom/Rivet
//   This program comes with ABSOLUTELY NO WARRANTY.
// From Rivet: the tone table, the two character tables (letters and figures, with the figure and letter shift tones and the line end) and the
// sync rule: two tones alternating, the higher one is the sync high tone, tone 24 (Rivet's default; stations are known to use others, and
// Rivet lets the user change it). New here: the tone search, the tone filters and the symbol clock of x40_rivet.h; the shift tones are
// not printed (Rivet prints "<fs>" and "<ls>"), and a tone outside the table shows as '?'. The output is the raw characters:
// the traffic is believed to be enciphered, nothing is deciphered.
#include "x40_rivet.h"

namespace dect2::x40 {
namespace {
using namespace rivet;

constexpr double kBase = 1410, kSpacing = 40, kBaud = 40;
constexpr int kK = 34, kGuard = 4, kSyncHigh = 24;

// Rivet's C36A and F36A tables (index = tone number); "fs" / "ls": figure and letter shift, "cr": a new line
const char* const kLet[kK] = {"Q", "X", "W", "V", "E", "K", " ", "B", "R", "J", "<*10>", "G", "T", "F", "fs", "M", "Y", "C", "cr", "Z", "U", "L", "<*22>", "D", "I", "H", "ls", "S", "O", "N", "<*30>", "A", "P", "<*33>"};
const char* const kFig[kK] = {"1", "\xD0\xA9", "2", "=", "3", "(", " ", "?", "4", "\xD0\xAE", "<*10>", "8", "5", "\xD0\xAD", "fs", ".", "6", ":", "cr", "+", "7", ")", "<*22>", " ", "8", "\xD1\x89", "ls", "", "9", ",", "<*30>", "-", "0", "<*33>"};

class Crowd36Dec : public Continuous {
public:
    void fill(X40Telemetry& t) const override {
        const Fit& f = lastFit();
        if (!locked()) { t.headline = chars_ ? fmt("CROWD36: %d characters so far, searching", chars_) : std::string("searching for CROWD36 tones"); return; }
        t.headline = synced_ ? fmt("CROWD36: %d characters", chars_) : fmt("CROWD36 tones found (offset %+.0f Hz), waiting for the sync tones", offsetHz());
        t.facts.push_back({"tone offset", fmt("%+.1f Hz", offsetHz())});
        if (syncHi_ >= 0) t.facts.push_back({"sync tones", fmt("%d and %d (high tone taken as tone %d)", syncHi_, syncLo_, kSyncHigh)});
        t.facts.push_back({"signal to noise (3 kHz)", fmt("%.1f dB", 10 * std::log10(std::max(f.snr3k, 1e-6)))});
        t.facts.push_back({"characters", fmt("%d", chars_)});
    }
protected:
    Search search() const override {
        Search s;
        for (int i = 0; i < kK; i++) s.tones.push_back(kBase + i * kSpacing);
        s.halfW = 0.4 * kSpacing; s.guardStep = kSpacing; s.guards = 2; s.minZ = 9;
        s.dfLo = -4500; s.dfHi = 4500; s.track = 0.3 * kSpacing; s.periodic = true; s.spacing = kSpacing; s.replaySec = 16;
        return s;
    }
    void setup(double rate) override {
        std::vector<double> f;
        for (int b = 0; b < kK + 2 * kGuard; b++) f.push_back(kBase + (b - kGuard) * kSpacing);
        bank_.configure(rate, f, (int)std::lround(rate / kBaud));
        clk_.configure(rate / kBaud, 32, 12);
    }
    void restart() override { bank_.reset(); clk_.reset(); q_.clear(); open_ = false; line_.clear(); adds_.clear(); lineChars_ = 0; fig_ = false; corr_ = 0; synced_ = false; inSync_ = 0; for (int& v : h_) v = -99; }
    void lost() override { flush(); }
    void sample(cf32 y) override {
        bank_.push(y);
        if (!clk_.push(bank_.maxE())) return;
        int b1 = 0, b2 = -1;
        for (int b = 1; b < bank_.size(); b++) if (bank_.e(b) > bank_.e(b1)) b1 = b;
        for (int b = 0; b < bank_.size(); b++) if (b != b1 && (b2 < 0 || bank_.e(b) > bank_.e(b2))) b2 = b;
        const double e1 = bank_.e(b1), e2 = b2 >= 0 ? bank_.e(b2) : 0;
        q_.push((e1 - e2) / (e1 + e2 + 1e-30));
        if (!(q_.full() && q_.mean() >= kQualityOpen)) { if (open_) flush(); open_ = false; return; }
        open_ = true;
        symbol(b1 - kGuard);
    }
private:
    static constexpr double kQualityOpen = 0.25;
    // n: the tone number as found (before the sync correction)
    void symbol(int n) {
        for (int i = 0; i < 5; i++) h_[i] = h_[i + 1];
        h_[5] = n;
        // the sync: two tones alternating for six symbols, at least three tones apart (Rivet: 100 Hz)
        const bool alt = h_[0] == h_[2] && h_[2] == h_[4] && h_[1] == h_[3] && h_[3] == h_[5] && std::abs(h_[0] - h_[1]) >= 3;
        if (alt) {
            const int hi = std::max(h_[0], h_[1]), lo = std::min(h_[0], h_[1]);
            if (!inSync_) {
                for (int i = 0; i < 5 && !adds_.empty(); i++) { const size_t a = adds_.back(); adds_.pop_back(); line_.resize(line_.size() >= a ? line_.size() - a : 0); if (a) lineChars_--; }   // the sync symbols read before the pattern was clear
                flush();
                corr_ = hi - kSyncHigh; syncHi_ = hi - corr_; syncLo_ = lo - corr_;
                out->row("Sync", "CROWD36", fmt("sync tones %d / %d, offset %+.0f Hz", syncHi_, syncLo_, offsetHz() + corr_ * kSpacing), snrDb());
                synced_ = true; fig_ = false;
            }
            inSync_ = 6;
            return;
        }
        if (inSync_ > 0) inSync_--;
        if (!synced_) return;   // as Rivet: no text before the sync tones (a steady carrier would read as one letter over and over)
        const int k = n - corr_;
        if (k < 0 || k >= kK) { put("?"); return; }
        const std::string c = fig_ ? kFig[k] : kLet[k];
        if (c == "ls") { fig_ = false; put(""); return; }
        if (c == "fs") { fig_ = true; put(""); return; }
        if (c == "cr") { fig_ = false; flush(); return; }
        put(c);
    }
    void put(const std::string& c) {
        adds_.push_back(c.size()); if (adds_.size() > 8) adds_.pop_front();
        if (c.empty()) return;
        line_ += c; lineChars_++;
        if (lineChars_ >= 80) flush();
    }
    void flush() {
        while (!line_.empty() && line_.back() == ' ') line_.pop_back();
        while (!line_.empty() && line_.front() == ' ') line_.erase(line_.begin());
        if (line_.size() >= 3) {
            const float s = snrDb();
            chars_ += lineChars_;
            out->good(s, offsetHz());
            out->row("Text", "CROWD36", line_, s);
        }
        line_.clear(); lineChars_ = 0; adds_.clear();
    }
    float snrDb() const { return (float)(10 * std::log10(std::max(lastFit().snr3k, 1e-6))); }
    ToneBank bank_; SymbolClock clk_; RunMean q_{16};
    bool open_ = false, fig_ = false, synced_ = false;
    int corr_ = 0, inSync_ = 0, chars_ = 0, lineChars_ = 0, syncHi_ = -1, syncLo_ = -1;
    int h_[6] = {-99, -99, -99, -99, -99, -99};
    std::string line_; std::deque<size_t> adds_;
};

} // namespace

std::unique_ptr<Decoder> makeCrowd36() { return std::make_unique<Crowd36Dec>(); }

} // namespace dect2::x40
