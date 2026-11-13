#include "x40_dsp.h"
#include "dect2/x40/gsc_proto.h"
#include "dect2/voice/vocoder_fec.h"
#include "dect2/common/bits.h"
#include <array>

namespace dect2::x40 {

namespace {

using namespace dect2::gsc;

constexpr double kWorkRate = 24000, kChanCut = 9000;
constexpr int kYRing = 1024, kBitRing = 4096;
constexpr double kVoiceTimeout = 60, kPreambleMemory = 4;

uint32_t txMask(uint32_t cw) { uint32_t m = 0; for (int k = 0; k < 23; k++) m |= (uint32_t)golayTxBit(cw, k) << k; return m; }
int pc(uint32_t v) { return dect2::bits::popcount(v); }

struct Slow { uint32_t hard = 0; std::array<float, 23> soft{}; };

struct Cand { int idx = -1; bool inv = false; int dist = 99; };

class GscDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkRate));
        if (!dec_.configure(fs, d, std::min(kChanCut, 0.45 * fs / d))) return false;
        rate_ = dec_.outRate();
        sps_ = rate_ / kFastBaud;
        box_ = std::max(2, (int)std::lround(sps_));
        for (int i = 0; i < 50; i++) w1Tx_[i] = txMask(golayWord((uint32_t)kWord1[i]));
        for (int i = 0; i < 10; i++) preTx_[i] = txMask(golayWord((uint32_t)kPreambleWords[i]));
        scTx_ = txMask(golayWord(kStartCode));
        for (int i = 0; i < 128; i++) bch_[i] = bchWord(i);
        reset();
        return true;
    }
    void reset() override {
        dec_.reset(); prev_ = cf32(1, 0); y_.assign(kYRing, 0.f); n_ = 0; acc_ = 0; fq_.assign((size_t)box_, 0.f); fqPos_ = 0;
        nextT_ = sps_ + box_; omega_ = 0; sPrev_ = 0;
        bits_.assign(kBitRing, 0.f); bi_ = -1; hi_ = 1500; lo_ = -1500;
        locked_ = false; pol_ = 1; blockEnd_ = 0; misses_ = 0; pend_ = Pending(); voiceOpen_ = false;
        preIdx_ = -1; preRawInv_ = false; preT_ = -1e9; lastPreEvent_ = -1e9; batch_ = false; group_ = -1;
        pages_ = 0; tone_ = numeric_ = alpha_ = voice_ = 0; snrDb_ = 0; have_ = false; nPre_ = nSc_ = nAddrHunt_ = nAcHunt_ = nLost_ = 0; lastKind_.clear(); lastWhat_.clear();
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 z) { sample(z); });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        t.headline = pages_ ? fmt("%d page(s); last: %s %s", pages_, lastKind_.c_str(), lastWhat_.c_str()) : locked_ ? "receiving GSC blocks" : "searching for GSC (preamble, start code, address)";
        t.facts.push_back({"pages", fmt("%d tone, %d numeric, %d alphanumeric, %d voice", tone_, numeric_, alpha_, voice_)});
        t.facts.push_back({"state", locked_ ? "block lock" : voiceOpen_ ? "voice message on the channel" : "searching"});
        if (group_ >= 0) t.facts.push_back({"preamble", fmt("%d (%s mode)", group_, batch_ ? "batch" : "single call")});
        t.facts.push_back({"polarity", pol_ > 0 ? "normal (higher frequency = 1)" : "inverted"});
        t.facts.push_back({"deviation", fmt("%.0f Hz", 0.5 * (hi_ - lo_))});
        t.facts.push_back({"carrier offset", fmt("%+.0f Hz", 0.5 * (hi_ + lo_))});
        t.facts.push_back({"bit clock", fmt("%+.0f ppm", -omega_ * 1e6)});
        t.facts.push_back({"search", fmt("%d preambles, %d start codes, %d addresses, %d activation words found while searching; %d lock losses", nPre_, nSc_, nAddrHunt_, nAcHunt_, nLost_)});
        t.facts.push_back({"bit rates", "300 bit/s preamble, start code and address words; 600 bit/s comma and data"});
    }

private:
    struct Pending { bool on = false; std::string id, desc; int function = 0; std::vector<std::array<int, 8>> blocks; bool more = false; double t = 0; };

    float yAt(double t) const {
        const int64_t i0 = (int64_t)std::floor(t); const float f = (float)(t - (double)i0);
        const float y0 = y_[(size_t)(i0 & (kYRing - 1))], y1 = y_[(size_t)((i0 + 1) & (kYRing - 1))];
        return y0 + (y1 - y0) * f;
    }
    void sample(cf32 z) {
        const cf32 p = z * std::conj(prev_); prev_ = z;
        float f = std::atan2(p.imag(), p.real()) * (float)(rate_ / (2 * kPi));
        acc_ += f - fq_[(size_t)fqPos_]; fq_[(size_t)fqPos_] = f; if (++fqPos_ == box_) fqPos_ = 0;
        y_[(size_t)(n_ & (kYRing - 1))] = (float)(acc_ / box_);
        if ((n_ & 4095) == 0) { double s = 0; for (float v : fq_) s += v; acc_ = s; }
        n_++;
        if ((double)(n_ - 1) >= nextT_ + 1) {
            const float raw = yAt(nextT_), mid = yAt(nextT_ - 0.5 * sps_);
            bit(raw, mid);
        }
    }
    void bit(float raw, float mid) {
        have_ = true;
        const double dc = 0.5 * (hi_ + lo_), amp = std::max(400.0, 0.5 * (hi_ - lo_));
        const double a = locked_ ? 0.01 : 0.04;
        if (raw > dc) hi_ += a * (raw - hi_); else lo_ += a * (raw - lo_);
        float s = (float)((raw - dc) / amp), m = (float)((mid - dc) / amp);
        s = std::clamp(s, -2.f, 2.f); m = std::clamp(m, -2.f, 2.f);
        const bool edge = (s > 0) != (sPrev_ > 0) && std::fabs(s - sPrev_) > 0.8f;
        const float e = edge ? std::clamp((s - sPrev_) * m, -2.f, 2.f) : 0.f;
        sPrev_ = s;
        omega_ = std::clamp(omega_ * (locked_ ? 1.0 : 0.9995) - 4e-5 * e, -0.004, 0.004);
        nextT_ += sps_ * (1 + omega_) - 0.04 * e * sps_;
        bi_++;
        bits_[(size_t)(bi_ & (kBitRing - 1))] = s;
        step();
    }
    float b(int64_t k) const { return bits_[(size_t)(k & (kBitRing - 1))]; }
    double now() const { return out ? out->now() : 0; }

    Slow slow(int64_t start, int pol) const {
        Slow w;
        for (int k = 0; k < 23; k++) { const float v = pol * (b(start + 2 * k) + b(start + 2 * k + 1)); w.soft[(size_t)k] = v; if (v > 0) w.hard |= 1u << k; }
        return w;
    }
    int commaTransitions(int64_t start) const { int c = 0; for (int k = 0; k < kCommaBits - 1; k++) c += (b(start + k) > 0) != (b(start + k + 1) > 0); return c; }
    Cand bestW1(uint32_t hard) const {
        Cand c;
        for (int i = 0; i < 50; i++) {
            const int d = pc(hard ^ w1Tx_[i]);
            if (d < c.dist) { c.dist = d; c.idx = i; c.inv = false; }
            if (23 - d < c.dist) { c.dist = 23 - d; c.idx = i; c.inv = true; }
        }
        return c;
    }
    bool knownWord(uint32_t tx) const {
        for (int i = 0; i < 50; i++) { const int d = pc(tx ^ w1Tx_[i]); if (d <= 4 || d >= 19) return true; }
        for (int i = 0; i < 10; i++) { const int d = pc(tx ^ preTx_[i]); if (d <= 4 || d >= 19) return true; }
        const int d = pc(tx ^ scTx_);
        return d <= 4 || d >= 19;
    }
    voicefec::Decoded soft23(const Slow& w) const {
        uint8_t rel[23]; uint32_t rx = 0;
        for (int k = 0; k < 23; k++) {
            const int pos = k < 12 ? 11 + k : k - 12;
            if (w.hard >> k & 1u) rx |= 1u << pos;
            rel[pos] = (uint8_t)std::min(255.f, std::fabs(w.soft[(size_t)k]) * 64.f + 1.f);
        }
        return voicefec::golaySoft(rx, rel, 23);
    }

    struct Addr { bool ok = false; int w1 = 0, w2 = 0, function = 1, flips = 0; };
    Addr address(int64_t st, int pol, int maxW1, int maxW2, bool strict) const {
        Addr r;
        const Slow s1 = slow(st + kCommaBits, pol), s2 = slow(st + kCommaBits + 47, pol);
        const Cand c = bestW1(s1.hard);
        if (c.dist > maxW1) return r;
        const voicefec::Decoded d2 = soft23(s2);
        if (d2.flips > maxW2) return r;
        uint32_t data = d2.data; bool inv2 = false;
        if (data >= 2048) { data ^= 0xFFFu; inv2 = true; }
        if (data == 0) return r;
        const uint32_t cw2 = golayWord(data) ^ (inv2 ? kAllOnes23 : 0u);
        if (strict) {
            const int first2 = golayTxBit(cw2, 0), sep = pol * b(st + kCommaBits + 46) > 0;
            const int first1 = golayTxBit(golayWord((uint32_t)kWord1[c.idx]) ^ (c.inv ? kAllOnes23 : 0u), 0), comma0 = pol * b(st) > 0;
            if (sep == first2 || comma0 != first1) return r;
        }
        r.ok = true; r.w1 = c.idx; r.w2 = (int)data; r.function = functionOf(c.inv, inv2); r.flips = c.dist + d2.flips;
        return r;
    }
    bool scMatch(int64_t st, int pol, int maxTotal, int& rawPol) const {
        const Slow s1 = slow(st + kCommaBits, pol), s2 = slow(st + kCommaBits + 47, pol);
        const int dn = pc(s1.hard ^ scTx_) + pc(s2.hard ^ scTx_ ^ 0x7FFFFFu);
        const int di = pc(s1.hard ^ scTx_ ^ 0x7FFFFFu) + pc(s2.hard ^ scTx_);
        if (dn <= maxTotal && dn <= di) { rawPol = pol; return true; }
        if (di <= maxTotal) { rawPol = -pol; return true; }
        return false;
    }
    bool activation(int64_t st, int pol, int maxTotal, int& word) const {
        const Slow s1 = slow(st + kCommaBits, pol), s2 = slow(st + kCommaBits + 47, pol);
        Slow c;
        for (int k = 0; k < 23; k++) { const float v = s1.soft[(size_t)k] - s2.soft[(size_t)k]; c.soft[(size_t)k] = v; if (v > 0) c.hard |= 1u << k; }
        const voicefec::Decoded d = soft23(c);
        const uint32_t cw = golayWord(d.data), tx = txMask(cw);
        if (pc(s1.hard ^ tx) + pc(s2.hard ^ tx ^ 0x7FFFFFu) > maxTotal) return false;
        if (knownWord(tx)) return false;
        word = (int)d.data;
        return true;
    }
    bool dataBlock(int64_t st, int pol, std::array<int, 8>& info, int& flips) const {
        flips = 0;
        for (int w = 0; w < 8; w++) {
            float r[15];
            for (int p = 0; p < 15; p++) r[p] = pol * b(st + 1 + p * 8 + w);
            float best = -1e30f; int bi = 0;
            for (int c = 0; c < 128; c++) {
                float s = 0;
                for (int p = 0; p < 15; p++) s += (bch_[c] >> (14 - p) & 1) ? r[p] : -r[p];
                if (s > best) { best = s; bi = c; }
            }
            int f = 0; for (int p = 0; p < 15; p++) f += ((bch_[bi] >> (14 - p) & 1) != 0) != (r[p] > 0);
            if (f > 2) return false;
            flips += f;
            info[(size_t)w] = bchInfo(bch_[bi]);
        }
        int s = 0; for (int w = 0; w < 7; w++) s += info[(size_t)w];
        return (s & 0x7F) == info[7];
    }
    void blockQuality(int64_t st) {
        double m = 0, v = 0;
        for (int k = 0; k < kBlockBits; k++) m += std::fabs(b(st + k));
        m /= kBlockBits;
        for (int k = 0; k < kBlockBits; k++) { const double d = std::fabs(b(st + k)) - m; v += d * d; }
        v /= kBlockBits;
        const double snr = 10 * std::log10(std::max(1e-3, m * m / std::max(v, 1e-4)));
        snrDb_ = (float)snr;
        out->good(snrDb_, 0.5 * (hi_ + lo_));
    }

    void step() {
        if (bi_ < kBlockBits + 200) return;
        if (!locked_) {
            preambleHunt();
            if (voiceOpen_ && now() - voiceT_ > kVoiceTimeout) voiceOpen_ = false;
            hunt();
        } else if (bi_ == blockEnd_) lockedBlock();
    }
    void preambleHunt() {
        for (int pol : {1, -1}) {
            const Slow w = slow(bi_ - 45, pol);
            for (int i = 0; i < 10; i++) {
                const int d = pc(w.hard ^ preTx_[i]);
                if (d > 2) continue;
                const Slow w0 = slow(bi_ - 45 - 46, pol);
                if (pc(w0.hard ^ preTx_[i]) > 3) continue;
                preIdx_ = i; preRawInv_ = pol < 0; preT_ = now();
                if (now() - lastPreEvent_ > 1.0) { lastPreEvent_ = now(); out->energy(); nPre_++; }
                return;
            }
        }
    }
    void hunt() {
        const int64_t st = bi_ - (kBlockBits - 1);
        const int ct = commaTransitions(st);
        if (ct < 14) return;
        int rawPol = 1;
        if (scMatch(st, 1, 6, rawPol)) {
            pol_ = rawPol; lock(st); nSc_++;
            onStart();
            return;
        }
        if (ct >= 24) {
            const Addr a = address(st, pol_, 2, 2, true);
            if (a.ok) { lock(st); nAddrHunt_++; group_ = -1; batch_ = false; onAddress(a); return; }
        }
        if (voiceOpen_ && ct >= 20) {
            int word = 0;
            if (activation(st, pol_, 6, word)) { lock(st); nAcHunt_++; onActivation(word); }
        }
    }
    void lock(int64_t st) { locked_ = true; blockEnd_ = st + (kBlockBits - 1) + kBlockBits; misses_ = 0; blockQuality(st); }
    void lockedBlock() {
        const int64_t st = blockEnd_ - (kBlockBits - 1);
        blockEnd_ += kBlockBits;
        const int ct = commaTransitions(st);
        int rawPol = 1;
        if (ct >= 10 && scMatch(st, pol_, 8, rawPol) && rawPol == pol_) { misses_ = 0; blockQuality(st); onStart(); return; }
        std::array<int, 8> info{}; int flips = 0;
        if (dataBlock(st, pol_, info, flips)) { misses_ = 0; blockQuality(st); onData(info); return; }
        if (ct >= 20) {
            const Addr a = address(st, pol_, 3, 3, true);
            if (a.ok) { misses_ = 0; blockQuality(st); onAddress(a); return; }
        }
        if (ct >= 16) {
            int word = 0;
            if (activation(st, pol_, 6, word)) { misses_ = 0; blockQuality(st); onActivation(word); return; }
        }
        finish();
        out->bad();
        if (++misses_ >= 2) { locked_ = false; misses_ = 0; nLost_++; }
    }

    void onStart() {
        finish();
        if (preIdx_ >= 0 && now() - preT_ < kPreambleMemory) { group_ = preIdx_; batch_ = preRawInv_ != (pol_ < 0); }
        else { group_ = -1; batch_ = false; }
        preIdx_ = -1;
    }
    std::string codeText(const Addr& a) const {
        int g = 0, ad = 0;
        if (!codeOfWords(a.w1, a.w2, g, ad)) return fmt("W1 %02d W2 %d", a.w1, a.w2);
        if (group_ >= 0) return fmt("%d%05d", (group_ - g % 10 + 10) % 10, g * 1000 + ad);
        return fmt("N%05d", g * 1000 + ad);
    }
    void onAddress(const Addr& a) {
        finish();
        pend_ = Pending();
        pend_.on = true; pend_.function = a.function; pend_.id = codeText(a); pend_.t = now();
        int g = 0, ad = 0;
        const bool plan = codeOfWords(a.w1, a.w2, g, ad);
        std::string where = group_ >= 0 ? fmt("preamble %d, %s", group_, batch_ ? "batch" : "single call") : "no preamble seen";
        if (plan && illegalAddress(g, ad)) where += ", a code the plan forbids";
        pend_.desc = fmt("address %d (W1%s W2%s), %s, %d bit(s) corrected", a.function, a.function >= 3 ? " inverted," : ",", (a.function == 2 || a.function == 4) ? " inverted" : "", where.c_str(), a.flips);
    }
    void onData(const std::array<int, 8>& info) {
        if (!pend_.on) { out->bad(); return; }
        pend_.blocks.push_back(info);
        bool more = false; std::vector<int> sym;
        unpackBlock(info.data(), 4, sym, more);
        pend_.more = more;
        if (!more || pend_.blocks.size() >= 20) finish();
    }
    void onActivation(int word) {
        if (pend_.on && pend_.blocks.empty()) {
            emit("Voice page", pend_.id, fmt("tone and voice: alert, then analog voice on the channel (not decoded); activation word %d; %s", word, pend_.desc.c_str()));
            voice_++; pend_ = Pending(); voiceOpen_ = true; voiceT_ = now(); voiceWord_ = word;
            return;
        }
        finish();
        if (voiceOpen_ && word == voiceWord_) {
            emit("Voice end", "", fmt("activation word %d again: the voice message ended after %.1f s", word, now() - voiceT_));
            voiceOpen_ = false;
            locked_ = false;
        }
    }
    static std::string numericText(const std::vector<std::array<int, 8>>& bl, int& good, int& total) {
        std::string s; good = total = 0;
        for (const auto& blk : bl) {
            std::vector<int> sym; bool more = false;
            unpackBlock(blk.data(), 4, sym, more);
            for (int v : sym) {
                total++;
                const char c = numericChar(v);
                if (!c) { good++; continue; }
                if ((c >= '0' && c <= '9') || c == ' ' || c == '-') good++;
                s += c;
            }
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    static std::string alphaText(const std::vector<std::array<int, 8>>& bl, int& good, int& total) {
        std::string s; good = total = 0; bool ended = false;
        for (const auto& blk : bl) {
            std::vector<int> sym; bool more = false;
            unpackBlock(blk.data(), 6, sym, more);
            for (int v : sym) {
                total++;
                const int c = alphaChar(v);
                if (c == 0) { good++; ended = true; continue; }
                if (c < 0) continue;
                if (ended) { s += ' '; ended = false; }
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == ',' || c == '?' || c == '!' || c == '\n' || c == '-' || c == ':' || c == '/') good++;
                s += c == '\n' ? '|' : (char)c;
            }
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    void finish() {
        if (!pend_.on) return;
        Pending p = pend_; pend_ = Pending();
        if (p.blocks.empty()) { emit("Tone page", p.id, "tone only; " + p.desc); tone_++; return; }
        int gn = 0, tn = 0, ga = 0, ta = 0;
        const std::string num = numericText(p.blocks, gn, tn), al = alphaText(p.blocks, ga, ta);
        const double qn = tn ? (double)gn / tn : 0, qa = ta ? (double)ga / ta : 0;
        const bool isNum = qn >= qa;
        const std::string tail = fmt("; %d block(s)%s; %s", (int)p.blocks.size(), p.more ? ", the last says more follow (cut short)" : "", p.desc.c_str());
        std::string other;
        if (std::fabs(qn - qa) < 0.15) other = fmt(" [read as %s: %s]", isNum ? "alphanumeric" : "numeric", isNum ? al.c_str() : num.c_str());
        if (isNum) { emit("Numeric page", p.id, num + other + tail); numeric_++; }
        else { emit("Alpha page", p.id, al + other + tail); alpha_++; }
    }
    void emit(const char* kind, const std::string& id, const std::string& text) {
        pages_ += std::string(kind) != "Voice end";
        lastKind_ = kind; lastWhat_ = id;
        out->row(kind, id, text, snrDb_);
    }

    Decimator dec_;
    double rate_ = kWorkRate, sps_ = 40, nextT_ = 0, omega_ = 0, acc_ = 0, hi_ = 1500, lo_ = -1500;
    int box_ = 40, fqPos_ = 0;
    cf32 prev_{1, 0};
    std::vector<float> y_, fq_, bits_;
    int64_t n_ = 0, bi_ = -1, blockEnd_ = 0;
    float sPrev_ = 0, snrDb_ = 0;
    bool locked_ = false, voiceOpen_ = false, preRawInv_ = false, batch_ = false, have_ = false;
    int pol_ = 1, misses_ = 0, preIdx_ = -1, group_ = -1, voiceWord_ = -1;
    double preT_ = -1e9, lastPreEvent_ = -1e9, voiceT_ = 0;
    int nPre_ = 0, nSc_ = 0, nAddrHunt_ = 0, nAcHunt_ = 0, nLost_ = 0;
    int pages_ = 0, tone_ = 0, numeric_ = 0, alpha_ = 0, voice_ = 0;
    std::string lastKind_, lastWhat_;
    Pending pend_;
    uint32_t w1Tx_[50] = {}, preTx_[10] = {}, scTx_ = 0;
    uint16_t bch_[128] = {};
};

}

std::unique_ptr<Decoder> makeGsc() { return std::make_unique<GscDec>(); }

}
