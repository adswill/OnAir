// XPA (10 and 20 Bd) and XPA2 (7.8125 Bd): MFSK number-group senders on HF (believed to carry Russian intelligence messages), received in
// the upper or lower sideband. Ported from Rivet (XPA.java, XPA2.java):
//   Rivet Copyright (C) 2011 Ian Wraith, GNU General Public License v3 or later, https://github.com/priyom/Rivet
//   This program comes with ABSOLUTELY NO WARRANTY.
// From Rivet: the tone tables (XPA 20 tones 40 Hz apart from 520 Hz; XPA2 16 tones 15.625 Hz apart from 978 Hz), the meaning of each tone
// (start, sync, space, end, message start, the ten digits and the "repeat the last character" tone), the start-tone pattern and the
// message assembly (spaces, ten groups a line, the block sync 6666622662626, the break 4444444444, the end of a message).
// New here: the tone search over the whole channel (any offset, both sidebands), the matched tone filters and the symbol clock (x40_rivet.h),
// both speeds of XPA at once, and the choice between the readings (speed, sideband, tone numbering off by up to three tones) by which
// one forms the most five-figure groups; a tone outside the table shows as '?' inside the line instead of breaking it.
#include "x40_rivet.h"

namespace dect2::x40 {
namespace {
using namespace rivet;

struct Kind { const char* name; double base, spacing; int K; double bauds[2]; int nBauds; bool two; };
const Kind kXpa{"XPA", 520, 40, 20, {10, 20}, 2, false};
const Kind kXpa2{"XPA2", 978, 15.625, 16, {7.8125, 0}, 1, true};
constexpr int kGuard = 3;            // extra filters on both sides of the table: the numbering may be off by this many tones

std::string xpaChar(int k, const std::string& prev) {
    switch (k) {
    case 0: return "Start Low";
    case 2: return "Sync Low";
    case 4: return " ";
    case 5: return "End Tone";
    case 15: return prev == "Sync Low" ? "Sync High" : "9";
    case 16: return "Message Start";
    case 17: return "R";
    case 19: return "Start High";
    default: break;
    }
    if (k >= 6 && k <= 14) return std::string(1, (char)('0' + k - 6));
    return "UNID";
}
std::string xpa2Char(int k, const std::string& prev) {
    switch (k) {
    case 0: return " ";
    case 2: return "Sync Low";
    case 3: return "End Tone";
    case 4: case 5: return prev == "Sync Low" ? "Sync High" : "R";   // Rivet allows 1041 and 1057 Hz for the sync high
    default: break;
    }
    if (k >= 6 && k <= 15) return std::string(1, (char)('0' + k - 6));
    return "UNID";
}

struct Line { const char* kind; std::string text; };

// One reading of the symbol stream: Rivet's displayMessage()
class Proto {
public:
    Proto(bool two, int chain, bool mirror, int corr) : two_(two), chain_(chain), mirror_(mirror), corr_(corr) {}
    int chain() const { return chain_; }
    bool mirror() const { return mirror_; }
    int corr() const { return corr_; }
    int wellFormed() const { return wf_; }
    std::vector<Line> pending;
    void symbol(int k) {
        h_[0] = h_[1]; h_[1] = h_[2]; h_[2] = h_[3]; h_[3] = k;
        text(k);
        // the start tones (two low, two high) and the sync high come before the traffic: what was read so far is not traffic (XPA2's
        // start tones are the space and the 9)
        const int hi = two_ ? 15 : 19;
        if (h_[0] == 0 && h_[1] == 0 && h_[2] == hi && h_[3] == hi) { line_.clear(); groups_ = 0; if (!inMsg_) { emit("Start", "start tones"); inMsg_ = true; } }
        if (prev_ == "Sync High") { line_.clear(); groups_ = 0; }
    }
    void text(int k) {
        std::string t = two_ ? xpa2Char(k, prev_) : xpaChar(k, prev_);
        if (t == "R" && prev_ == "End Tone") { flush(); emit("End", "message complete"); prev_.clear(); inMsg_ = false; return; }
        if (t == "R") t = prev_;
        if (t == "UNID") t = "?";
        if (t == "Message Start" && prev_ == "Message Start") return;
        if (t == " " && prev_ == " ") return;
        if (t == " " && line_.empty()) { prev_ = t; return; }
        prev_ = t;
        if (t == "Message Start") { flush(); emit("Message start", "message start"); groups_ = 0; inMsg_ = true; return; }
        if (t == "End Tone") { flush(); groups_ = 0; return; }
        if (t.empty() || t == "Sync High" || t == "Sync Low" || t == "Start High" || t == "Start Low") return;
        line_ += t;
        if (line_.size() > 160) { flush(); groups_ = 0; return; }   // no spaces: not group traffic
        if (t == " ") {
            const size_t e = line_.size() - 1, b = line_.rfind(' ', e - 1);
            const std::string g = line_.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1));
            if (g.size() == 5 && g.find_first_not_of("0123456789") == std::string::npos) wf_++;
            if (++groups_ == 10) { flush(); groups_ = 0; }
            return;
        }
        static const std::string kBlock = "6666622662626", kBreak = "4444444444";
        if (endsWith(kBlock)) { line_.resize(line_.size() - kBlock.size()); flush(); emit("Block sync", kBlock); groups_ = 0; }
        else if (endsWith(kBreak)) { line_.resize(line_.size() - kBreak.size()); flush(); emit("Break", kBreak); groups_ = 0; }
    }
    void flush() {
        while (!line_.empty() && line_.back() == ' ') line_.pop_back();
        if (!line_.empty()) emit("Groups", line_);
        line_.clear();
    }
    void reset() { pending.clear(); line_.clear(); prev_.clear(); groups_ = 0; wf_ = 0; inMsg_ = false; for (int& v : h_) v = -9; }
private:
    bool endsWith(const std::string& s) const { return line_.size() >= s.size() && line_.compare(line_.size() - s.size(), s.size(), s) == 0; }
    void emit(const char* kind, const std::string& t) { pending.push_back({kind, t}); if (pending.size() > 64) pending.erase(pending.begin()); }
    bool two_; int chain_; bool mirror_; int corr_;
    std::string prev_, line_; int groups_ = 0, wf_ = 0; bool inMsg_ = false; int h_[4] = {-9, -9, -9, -9};
};

class XpaDec : public Continuous {
public:
    explicit XpaDec(const Kind& k) : k_(k) {}
    void fill(X40Telemetry& t) const override {
        const Fit& f = lastFit();
        if (!locked()) { t.headline = groups_ ? fmt("%s: %d groups so far, searching", k_.name, groups_) : fmt("searching for %s tones", k_.name); return; }
        if (win_ < 0) { t.headline = fmt("%s tones found (offset %+.0f Hz), reading", k_.name, offsetHz()); }
        else {
            const Proto& p = protos_[(size_t)win_];
            t.headline = fmt("%s %s: %d groups", k_.name, baudName(p.chain()).c_str(), groups_);
            t.facts.push_back({"speed", baudName(p.chain())});
            t.facts.push_back({"sideband", p.mirror() ? "lower (tones reversed)" : "upper"});
            t.facts.push_back({"tone numbering", p.corr() ? fmt("moved by %+d tones", p.corr()) : std::string("as found")});
        }
        t.facts.push_back({"tone offset", fmt("%+.1f Hz", offsetHz())});
        t.facts.push_back({"signal to noise (3 kHz)", fmt("%.1f dB", 10 * std::log10(std::max(f.snr3k, 1e-6)))});
        t.facts.push_back({"groups", fmt("%d", groups_)});
    }
protected:
    Search search() const override {
        Search s;
        for (int i = 0; i < k_.K; i++) s.tones.push_back(k_.base + i * k_.spacing);
        s.halfW = std::min(0.4 * k_.spacing, k_.bauds[0]); s.guardStep = k_.spacing; s.guards = 2; s.minZ = 9;
        s.dfLo = -4500; s.dfHi = 4500; s.track = 0.3 * k_.spacing; s.periodic = true; s.spacing = k_.spacing; s.replaySec = 16;
        return s;
    }
    void setup(double rate) override {
        std::vector<double> f;
        for (int b = 0; b < k_.K + 2 * kGuard; b++) f.push_back(k_.base + (b - kGuard) * k_.spacing);
        chains_.clear(); chains_.resize((size_t)k_.nBauds);
        protos_.clear();
        for (int c = 0; c < k_.nBauds; c++) {
            const double T = rate / k_.bauds[c];
            chains_[(size_t)c].bank.configure(rate, f, (int)std::lround(T));
            chains_[(size_t)c].clk.configure(T, 32, 12);
            for (int m = 0; m < 2; m++) for (int corr = -kGuard; corr <= kGuard; corr++) protos_.emplace_back(k_.two, c, m != 0, corr);
        }
    }
    void restart() override {
        for (auto& c : chains_) { c.bank.reset(); c.clk.reset(); c.q.clear(); c.open = false; }
        for (auto& p : protos_) p.reset();
        win_ = -1;
    }
    void lost() override {
        for (auto& p : protos_) p.flush();
        deliver();
        win_ = -1;
    }
    void sample(cf32 y) override {
        for (size_t c = 0; c < chains_.size(); c++) {
            Chain& ch = chains_[c];
            ch.bank.push(y);
            if (!ch.clk.push(ch.bank.maxE())) continue;
            int b1 = 0, b2 = -1;
            for (int b = 1; b < ch.bank.size(); b++) if (ch.bank.e(b) > ch.bank.e(b1)) b1 = b;
            for (int b = 0; b < ch.bank.size(); b++) if (b != b1 && (b2 < 0 || ch.bank.e(b) > ch.bank.e(b2))) b2 = b;
            const double e1 = ch.bank.e(b1), e2 = b2 >= 0 ? ch.bank.e(b2) : 0;
            ch.q.push((e1 - e2) / (e1 + e2 + 1e-30));
            const bool open = ch.q.full() && ch.q.mean() >= kQualityOpen;
            if (!open) {
                if (ch.open) { for (auto& p : protos_) if (p.chain() == (int)c) p.flush(); deliver(); }
                ch.open = false; continue;
            }
            ch.open = true;
            for (auto& p : protos_) {
                if (p.chain() != (int)c) continue;
                const int n = p.mirror() ? (k_.K - 1) - (b1 - kGuard) : b1 - kGuard;
                p.symbol(n - p.corr());
            }
            deliver();
        }
    }
private:
    static constexpr double kQualityOpen = 0.25;   // mean (e1 - e2) / (e1 + e2) of the last 16 symbols; noise gives about 0.15
    struct Chain { ToneBank bank; SymbolClock clk; RunMean q{16}; bool open = false; };
    std::string baudName(int c) const { return k_.bauds[c] == 7.8125 ? std::string("7.8125 Bd") : fmt("%.0f Bd", k_.bauds[c]); }
    // the reading with the most five-figure groups speaks once it has three and twice as many as any other
    void deliver() {
        if (win_ < 0) {
            int best = -1, second = 0;
            for (size_t i = 0; i < protos_.size(); i++) {
                const int w = protos_[i].wellFormed();
                if (best < 0 || w > protos_[(size_t)best].wellFormed()) { if (best >= 0) second = std::max(second, protos_[(size_t)best].wellFormed()); best = (int)i; }
                else second = std::max(second, w);
            }
            if (best < 0 || protos_[(size_t)best].wellFormed() < 3 || protos_[(size_t)best].wellFormed() < 2 * second + 1) return;
            win_ = best;
            for (auto& p : protos_) if (&p != &protos_[(size_t)win_]) p.pending.clear();
        }
        Proto& p = protos_[(size_t)win_];
        const std::string id = fmt("%s %s", k_.name, baudName(p.chain()).c_str());
        const Fit& f = lastFit();
        const float snr = (float)(10 * std::log10(std::max(f.snr3k, 1e-6)));
        for (const Line& l : p.pending) {
            if (std::string(l.kind) == "Groups") { groups_ += (int)std::count(l.text.begin(), l.text.end(), ' ') + 1; out->good(snr, offsetHz()); }
            out->row(l.kind, id, l.text, snr);
        }
        p.pending.clear();
        for (auto& q : protos_) if (&q != &p && q.pending.size() > 8) q.pending.clear();
    }
    Kind k_;
    std::vector<Chain> chains_;
    std::vector<Proto> protos_;
    int win_ = -1, groups_ = 0;
};

} // namespace

std::unique_ptr<Decoder> makeXpa() { return std::make_unique<XpaDec>(kXpa); }
std::unique_ptr<Decoder> makeXpa2() { return std::make_unique<XpaDec>(kXpa2); }

} // namespace dect2::x40
