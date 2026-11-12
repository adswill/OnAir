// FSK200/500 (Enigma designation F01): 200 Bd two-tone FSK with 500 Hz shift, used for Russian diplomatic and intelligence messages.
// Asynchronous ITA2 characters: a start bit, five data bits (first bit least significant) and a stop of one and a half bits; the low tone is
// the mark. Ported from Rivet (FSK200500.java, FSK.java):
//   Rivet Copyright (C) 2011 Ian Wraith, GNU General Public License v3 or later, https://github.com/priyom/Rivet
//   This program comes with ABSOLUTELY NO WARRANTY.
// From Rivet: the character frame, the mark / space sense, the ITA2 table it prints (always the figures case: the traffic is figure groups;
// the shift codes are dropped) and the line ends (CR, LF, 80 characters). New here: the tone-pair search over the whole channel, the matched
// tone filters, the start-bit edge timing of every character (as the FSK modems of x40_fskmodem.cpp do, so the half stop bit needs no
// special case), both senses of the shift tried at once (the one whose stop bits hold wins), and the framing error count.
#include "x40_rivet.h"

namespace dect2::x40 {
namespace {
using namespace rivet;

constexpr double kShift = 500, kBaud = 200;
constexpr int kRing = 2048;

// One sense of the shift: async characters from mark minus space
class Async {
public:
    explicit Async(bool lowIsMark) : lowMark_(lowIsMark) {}
    struct Char { int code; bool ok; };
    std::vector<Char> got;
    void configure(double T) { T_ = T; reset(); }
    void reset() { d_.assign(kRing, 0.f); n_ = 0; hunt_ = 0; pendT_ = -1; got.clear(); valid_.clear(); }
    double validRatio() const { return valid_.mean(); }
    bool validFull() const { return valid_.full(); }
    void push(float eLow, float eHigh) {
        const float m = lowMark_ ? eLow : eHigh, s = lowMark_ ? eHigh : eLow;
        d_[(size_t)(n_ % kRing)] = (m - s) / (m + s + 1e-30f);
        n_++;
        if (pendT_ >= 0) { if ((double)n_ - 1 >= pendT_ + 6.5 * T_ + 1) read(); return; }
        const long i = n_ - 1;
        if (i < 1 || (double)i < hunt_) return;
        const float a = at(i - 1), b = at(i);
        if (a >= 0 && b < 0) pendT_ = (double)(i - 1) + a / (a - b);   // a start bit: the crossing lies half a bit after its edge
    }
private:
    float at(long i) const { return d_[(size_t)(i % kRing)]; }
    float atT(double t) const { const long i = (long)std::llround(t); return i >= 0 && i < n_ && n_ - i < kRing ? at(i) : 0.f; }
    void read() {
        const double tc = pendT_; pendT_ = -1;
        int v = 0;
        for (int k = 1; k <= 5; k++) if (atT(tc + (k + 0.5) * T_) > 0) v |= 1 << (k - 1);
        const bool stop = atT(tc + 6.5 * T_) > 0;
        valid_.push(stop ? 1.0 : 0.0);
        got.push_back({v, stop});
        hunt_ = stop ? tc + 7.0 * T_ : tc + 1.0 * T_;
    }
    bool lowMark_; double T_ = 40, hunt_ = 0, pendT_ = -1; long n_ = 0;
    std::vector<float> d_; RunMean valid_{24};
};

class F01Dec : public Continuous {
public:
    void fill(X40Telemetry& t) const override {
        const Fit& f = lastFit();
        if (!locked()) { t.headline = chars_ ? fmt("FSK200/500: %d characters so far, searching", chars_) : std::string("searching for FSK200/500 tones"); return; }
        t.headline = win_ >= 0 ? fmt("FSK200/500: %d characters", chars_) : fmt("FSK200/500 tones found (offset %+.0f Hz), reading", offsetHz());
        t.facts.push_back({"tones", fmt("%.0f / %.0f Hz from the dial (%s)", offsetHz() - kShift / 2, offsetHz() + kShift / 2, win_ == 1 ? "high tone is mark: reversed" : "low tone is mark")});
        t.facts.push_back({"signal to noise (3 kHz)", fmt("%.1f dB", 10 * std::log10(std::max(f.snr3k, 1e-6)))});
        t.facts.push_back({"characters", fmt("%d (%d framing errors)", chars_, ferr_)});
    }
protected:
    Search search() const override {
        Search s;
        s.tones = {-kShift / 2, kShift / 2};
        s.halfW = std::min(0.35 * kShift, kBaud); s.guardStep = kShift; s.guards = 0; s.minZ = 9;
        s.dfLo = -3200; s.dfHi = 3200; s.track = 20; s.periodic = false; s.replaySec = 16;
        return s;
    }
    void setup(double rate) override {
        T_ = rate / kBaud;
        bank_.configure(rate, {-kShift / 2, kShift / 2}, (int)std::lround(T_));
        for (auto& a : as_) a.configure(T_);
    }
    void restart() override { bank_.reset(); for (auto& a : as_) a.reset(); q_.clear(); win_ = -1; line_.clear(); lineChars_ = 0; open_ = false; bitN_ = 0; }
    void lost() override { flush(); }
    void sample(cf32 y) override {
        bank_.push(y);
        const float lo = bank_.e(0), hi = bank_.e(1);
        if (++bitN_ >= (long)T_) { bitN_ = 0; q_.push(std::fabs(lo - hi) / (lo + hi + 1e-30f)); }   // decisiveness once a bit time
        for (auto& a : as_) a.push(lo, hi);
        const bool open = q_.full() && q_.mean() >= kQualityOpen;
        if (!open && open_) flush();
        open_ = open;
        if (win_ < 0) {
            for (int i = 0; i < 2 && win_ < 0; i++) {
                const Async& a = as_[i]; const Async& o = as_[1 - i];
                if (a.validFull() && a.validRatio() >= 0.85 && a.validRatio() > o.validRatio() + 0.2) win_ = i;
            }
            if (win_ < 0) { for (auto& a : as_) if (a.got.size() > 64) a.got.erase(a.got.begin(), a.got.begin() + (long)(a.got.size() - 64)); return; }
        }
        as_[1 - win_].got.clear();
        for (const auto& c : as_[win_].got) if (open_) character(c.code, c.ok);
        as_[win_].got.clear();
    }
private:
    static constexpr double kQualityOpen = 0.7;   // mean |mark - space| / (mark + space) over 16 bit times; noise gives 0.5
    void character(int code, bool ok) {
        if (!ok) ferr_++;
        if (!ok && line_.empty()) return;   // the reversals and other noise before a line are not text
        if (code == 0 || code == 27 || code == 31) return;
        if (code == 2 || code == 8) { flush(); return; }
        line_ += ok ? baudotFigure(code) : "?";
        if (++lineChars_ >= 80) flush();
    }
    void flush() {
        while (!line_.empty() && line_.back() == ' ') line_.pop_back();
        size_t b = line_.find_first_not_of(' ');
        if (b == std::string::npos) line_.clear(); else line_.erase(0, b);
        if (lineChars_ >= 4 && !line_.empty()) {
            const float s = (float)(10 * std::log10(std::max(lastFit().snr3k, 1e-6)));
            chars_ += lineChars_;
            out->good(s, offsetHz());
            out->row("Text", "FSK200/500", line_, s);
        }
        line_.clear(); lineChars_ = 0;
    }
    ToneBank bank_; Async as_[2] = {Async(true), Async(false)}; RunMean q_{16};
    double T_ = 40; long bitN_ = 0; int win_ = -1, chars_ = 0, ferr_ = 0, lineChars_ = 0; bool open_ = false;
    std::string line_;
};

} // namespace

std::unique_ptr<Decoder> makeF01() { return std::make_unique<F01Dec>(); }

} // namespace dect2::x40
