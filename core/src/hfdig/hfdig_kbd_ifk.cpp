// DominoEX and THOR (hfdig_kbd.h), after fldigi's src/dominoex and src/thor.
//   tones     18 tones tonespacing = fs * ds / symlen apart (ds 1, 2 or 4 bins), one tone per symbol, centred on the audio frequency
//   IFK+      each symbol changes the tone by 2 + the 4 bit symbol (modulo 18): a tuning error does not matter, only the steps do
//   DominoEX  the symbols are the nibbles of the DominoEX varicode (a nibble with bit 3 set continues the character), no FEC
//   THOR      the IZ8BLY varicode bits, K = 7 (K = 15 for THOR 25x4 .. 100) convolutional code with the polynomials of fldigi,
//             a 4 x 4 x depth interleaver (10, 50 for the fast modes), four bits per symbol
//   receiver  a sliding DFT bank over the 18 tones and nine tones on each side at 3 or 5 sub-bin offsets, the symbol clock from the peak
//             sharpness at 16 hop phases per symbol, the tone step between decisions
#include "hfdig_kbd_dsp.h"
#include "hfdig_kbd_tables.h"

namespace dect2 {
namespace kbd {
namespace {

struct IfkP {
    bool thor;
    int fs, symlen, ds, depth;
    bool k15;
};
const IfkP kDomino[6] = {
    {false, 8000, 2048, 2, 0, false},    // 4
    {false, 11025, 2048, 2, 0, false},   // 5
    {false, 8000, 1024, 2, 0, false},    // 8
    {false, 11025, 1024, 1, 0, false},   // 11
    {false, 8000, 512, 1, 0, false},     // 16
    {false, 11025, 512, 1, 0, false},    // 22
};
const IfkP kThor[10] = {
    {true, 8000, 2048, 2, 10, false},    // 4
    {true, 11025, 2048, 2, 10, false},   // 5
    {true, 8000, 1024, 2, 10, false},    // 8
    {true, 11025, 1024, 1, 10, false},   // 11
    {true, 8000, 512, 1, 10, false},     // 16
    {true, 11025, 512, 1, 10, false},    // 22
    {true, 8000, 320, 4, 50, true},      // 25x4
    {true, 8000, 160, 1, 50, true},      // 50x1
    {true, 8000, 160, 2, 50, true},      // 50x2
    {true, 8000, 80, 1, 50, true},       // 100
};
constexpr int kTones = 18;

IfkP params(const KbdConfig& c) {
    if (c.family == KbdThor) return kThor[std::clamp(c.variant, 0, 9)];
    return kDomino[std::clamp(c.variant, 0, 5)];
}
// the Viterbi codes of THOR
constexpr unsigned kK7a = 0x6d, kK7b = 0x4f, kK15a = 044735, kK15b = 063057;

// DominoEX characters: the nibbles of each character as one key (last nibble lowest), -1 when none
int dominoDecode(unsigned key) {
    static const std::vector<short> tab = [] {
        std::vector<short> t(4096, -1);
        for (int c = 0; c < 256; c++) {
            const unsigned char* n = kDominoVaricode[c];
            int len = 1;
            while (len < 3 && (n[len] & 8)) len++;
            unsigned k = 0;
            for (int i = 0; i < len; i++) k |= (unsigned)n[len - 1 - i] << (4 * i);
            t[k] = (short)c;
        }
        return t;
    }();
    return key < 4096 ? tab[key] : -1;
}

// ------------------------------------------------------------------ transmitter
class IfkMod : public Modulator {
public:
    IfkMod(const KbdConfig& c, double centre) : p_(params(c)), enc_(p_.k15 ? 15 : 7, p_.k15 ? kK15a : kK7a, p_.k15 ? kK15b : kK7b), inl_(4, std::max(1, p_.depth), true) {
        spacing_ = (double)p_.fs * p_.ds / p_.symlen;
        f0_ = centre - kTones * spacing_ / 2.0;
        if (p_.fs != 8000) rs_ = std::make_unique<Resampler>(p_.fs, 8000);
        if (p_.thor) {   // Clearbits(): the interleaver is filled with the code of zero bits; 16 symbols of zero, then an idle character
            const int data = enc_.encode(0);
            for (int k = 0; k < 1400; k++)
                for (int i = 0; i < 2; i++) {
                    bitsh_ = (bitsh_ << 1) | (unsigned)((data >> i) & 1);
                    if (++bitstate_ == 4) { inl_.bits(&bitsh_); bitstate_ = 0; bitsh_ = 0; }
                }
            enc_.reset();
            for (int i = 0; i < 16; i++) sendSymbol(0);
        }
        sendChar(0);
        for (int ch : {13, 2, 13}) sendChar(ch);
    }
    void next(std::vector<float>& out, TextSrc& src) override {
        if (pending_.empty()) sendChar(src.next());
        const std::vector<float> v = std::move(pending_);
        pending_.clear();
        if (rs_) rs_->process(v.data(), v.size(), out); else out.insert(out.end(), v.begin(), v.end());
    }
private:
    void sendChar(int c) {
        c &= 255;
        if (!p_.thor) {
            const unsigned char* n = kDominoVaricode[c];
            sendSymbol(n[0]);
            for (int i = 1; i < 3; i++) { if (n[i] & 8) sendSymbol(n[i]); else break; }
            return;
        }
        for (const char* q = kMfskVaricode[c]; *q; q++) {
            const int data = enc_.encode(*q - '0');
            for (int i = 0; i < 2; i++) {
                bitsh_ = (bitsh_ << 1) | (unsigned)((data >> i) & 1);
                if (++bitstate_ == 4) { inl_.bits(&bitsh_); sendSymbol((int)bitsh_); bitstate_ = 0; bitsh_ = 0; }
            }
        }
    }
    void sendSymbol(int sym) {
        prevTone_ = (prevTone_ + 2 + sym) % kTones;
        const double w = 2 * kPi * (f0_ + (prevTone_ + 0.5) * spacing_) / p_.fs;
        for (int i = 0; i < p_.symlen; i++) {
            pending_.push_back((float)std::cos(ph_));
            ph_ += w;
            if (ph_ > 2 * kPi) ph_ -= 2 * kPi;
        }
    }
    IfkP p_;
    ConvEnc enc_;
    Interleaver inl_;
    std::unique_ptr<Resampler> rs_;
    double spacing_, f0_, ph_ = 0;
    unsigned bitsh_ = 0;
    int bitstate_ = 0, prevTone_ = 0;
    std::vector<float> pending_;
};

// ------------------------------------------------------------------ receiver
class IfkEngine : public Engine {
public:
    explicit IfkEngine(const KbdConfig& c) : cfg_(c), p_(params(c)) {
        spacing_ = (double)p_.fs * p_.ds / p_.symlen;
        paths_ = p_.ds == 1 ? 5 : 3;
        hop_ = std::max(1, p_.symlen / kHops);
        if (p_.fs != 8000) rs_ = std::make_unique<Resampler>(8000, p_.fs);
        setCentre(c.centreHz);
    }
    double bandwidth() const override { return kTones * spacing_; }
    void setCentre(double hz) override { centre_ = hz; build(); }
    void reset() override { build(); }
    void feed(const float* x, size_t n) override {
        if (rs_) { tmp_.clear(); rs_->process(x, n, tmp_); for (float v : tmp_) sample(v); }
        else for (size_t i = 0; i < n; i++) sample(x[i]);
    }

private:
    static constexpr int kHops = 16, kExtra = 9;
    void build() {
        const double binW = (double)p_.fs / p_.symlen;
        const double fStart = centre_ - kTones * spacing_ / 2.0 - kExtra * spacing_;
        const int total = (int)std::lround((kTones + 2 * kExtra) * p_.ds * paths_);
        std::vector<double> f;   // only frequencies the audio has (a bin below 0 Hz would copy the tones from above)
        for (int m = 0; m < total; m++) { const double fm = fStart + m * binW / paths_; if (fm > 80 && fm < 3900) f.push_back(fm); }
        M_ = (int)f.size();
        bank_.init(p_.symlen, p_.fs, f);
        ph_.assign(kHops, 0.f);
        count_ = 0; hopIdx_ = 0; nextDec_ = kHops; prevPos_ = -1; ratio_ = 1;
        const int k = p_.thor ? std::max(1, p_.depth) : 1;
        inl_ = std::make_unique<Interleaver>(4, k, false);
        vit_ = std::make_unique<Viterbi>(p_.k15 ? 15 : 7, p_.k15 ? kK15a : kK7a, p_.k15 ? kK15b : kK7b, p_.k15 ? 15 * 12 : 45);
        pair_[0] = pair_[1] = 0; symcounter_ = 0; shreg_ = 1; conf_ = 0; nib_ = 0;
        for (auto& v : symbuf_) v = 0;
        locked = false; snrDb = 0; offsetHz = 0; sig2_ = 0; amp2_ = 0; refPos_ = 0;
        tf_ = TextFilter();
    }
    void sample(float v) {
        bank_.feed(v);
        if (++count_ >= hop_) { count_ = 0; hop(); }
    }
    void peak(int& pos, double& mx, double& sum) const {
        mx = 0; sum = 0; pos = 0;
        for (int m = 0; m < M_; m++) { const double a = bank_.mag(m); sum += a; if (a > mx) { mx = a; pos = m; } }
    }
    void hop() {
        hopIdx_++;
        const int phase = (int)(hopIdx_ % kHops);
        int pos; double mx, sum;
        peak(pos, mx, sum);
        ph_[phase] += 0.1f * ((sum > 1e-12 ? (float)(mx / sum) : 0.f) - ph_[phase]);
        if (hopIdx_ < nextDec_) return;
        decide(pos, mx, sum);
        int best = 0;
        for (int i = 1; i < kHops; i++) if (ph_[i] > ph_[best]) best = i;
        const int cur = (int)(nextDec_ % kHops);
        int d = best - cur;
        if (d > kHops / 2) d -= kHops;
        if (d < -kHops / 2) d += kHops;
        int adj = 0;
        if (d != 0 && ph_[best] > ph_[cur] * 1.03f + 1e-5f) adj = d > 0 ? 1 : -1;
        nextDec_ += kHops + adj;
    }
    void decide(int pos, double mx, double sum) {
        const double others = (sum - mx) / std::max(1, M_ - 1);
        const double r = others > 1e-12 ? mx / others : 1.0;
        ratio_ = decayAvg(ratio_, r, 12);
        snrDb = (float)(20.0 * std::log10(std::max(1.0, ratio_)));
        const bool burst = mx < 1.2 * (sum / M_);
        if (prevPos_ < 0 || sum < 1e-9) { prevPos_ = pos; return; }
        const double fdiff = (double)(pos - prevPos_) / paths_ / p_.ds;
        refPos_ = prevPos_;
        prevPos_ = pos;
        int c = (int)std::floor(fdiff + 0.5);
        const bool outOfRange = std::fabs(fdiff) > 17.5 || c == 0 || c == 1 || c > 17 || c < -16;
        c -= 2;
        if (c < 0) c += kTones;
        const bool sig = ratio_ >= kLockRatio;
        if (p_.thor) {
            uint8_t s[4];
            thorSoft(s);
            inl_->symbols(s);
            for (int i = 0; i < 4; i++) pairs(s[i]);
        } else {
            locked = sig;
            if (!(c & 8)) {   // a new character starts: finish the previous one
                if (nib_ <= 3 && nib_ > 0) {
                    unsigned key = 0;
                    for (int i = 0; i < nib_; i++) key |= (unsigned)symbuf_[i] << (4 * i);
                    const int ch = dominoDecode(key);
                    if (ch > 0 && sig && !burst && !outOfRange) tf_.put(out, ch);
                }
                nib_ = 0;
            }
            for (int i = 3; i > 0; i--) symbuf_[i] = symbuf_[i - 1];
            symbuf_[0] = (uint8_t)(outOfRange || c > 15 ? 0 : c);
            if (++nib_ > 4) nib_ = 4;
        }
    }
    // THOR soft bits (3.9 sensitivity pass D): the 16 symbol values are the tone steps of 2 .. 17 tones (modulo the 18) from the last
    // tone; each one's magnitude is the larger of the bins at the two steps that alias to it, and the bits get their log likelihood ratios
    // from those (fskSoftBits). The hard decision on the strongest bin, erased below a peak-to-average of 6 or 1.2 (a "burst"), lost THOR16
    // under -6 dB in 2.5 kHz; it now copies to -11 dB.
    void thorSoft(uint8_t* s) {
        double a[16], e2 = 0, mx = 0;
        const int step = paths_ * p_.ds;
        for (int m = 0; m < M_; m++) { const double v = bank_.mag(m); e2 += v * v; mx = std::max(mx, v); }
        for (int c = 0; c < 16; c++) {
            double v = 0;
            for (int d : {c + 2, c + 2 - kTones}) { const int j = refPos_ + d * step; if (j >= 0 && j < M_) v = std::max(v, bank_.mag(j)); }
            a[c] = v;
        }
        const double noise = (e2 - mx * mx) / std::max(1, M_ - 1);
        sig2_ = sig2_ > 0 ? decayAvg(sig2_, noise, 32) : noise;
        amp2_ = decayAvg(amp2_, std::max(0.0, mx * mx - sig2_), 32);
        static const unsigned label[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        fskSoftBits(a, 16, label, 4, std::sqrt(amp2_), std::max(sig2_, kSnrCap * amp2_), kSoftGain, s);
    }
    void pairs(uint8_t s) {
        pair_[0] = pair_[1];
        pair_[1] = s;
        symcounter_ = symcounter_ ? 0 : 1;
        if (symcounter_) return;
        int met = 0;
        const int bit = vit_->decode(pair_, &met);
        if (met < 255 / 2) conf_ -= 2 + conf_ / 2; else conf_ += 2;
        conf_ = std::clamp(conf_, 0, 100);
        locked = conf_ >= kLockConf;
        if (!locked) { shreg_ = 1; return; }
        shreg_ = (shreg_ << 1) | (unsigned)bit;
        if ((shreg_ & 7) == 1) {
            const unsigned code = shreg_ >> 1;
            if (code < 0xB80) { const int ch = mfskVaridec(code); if (ch > 0) tf_.put(out, ch); }
            shreg_ = 1;
        }
    }

    // the squelch: peak over the average of the other bins (noise alone: 2.5 to 2.8; 3.9 sensitivity pass D: 6 held DominoEX 11 at -7 dB in
    // 2.5 kHz, 4 lets it copy to -12 dB, and 6 minutes of noise still give no character)
    static constexpr double kLockRatio = 4.0;
    static constexpr double kSoftGain = 32.0, kSnrCap = 0.1;   // as MFSK (hfdig_kbd_mfsk.cpp)
    double sig2_ = 0, amp2_ = 0;   // noise and signal power per bin, for the soft bits
    int refPos_ = 0;               // the bin of the previous tone
    static constexpr int kLockConf = 40;
    KbdConfig cfg_;
    IfkP p_;
    double spacing_, centre_ = 1500, ratio_ = 1;
    int paths_, hop_, M_ = 0, count_ = 0, prevPos_ = -1, symcounter_ = 0, conf_ = 0, nib_ = 0;
    uint64_t hopIdx_ = 0, nextDec_ = 0;
    std::unique_ptr<Resampler> rs_;
    std::vector<float> tmp_, ph_;
    Bank bank_;
    std::unique_ptr<Interleaver> inl_;
    std::unique_ptr<Viterbi> vit_;
    uint8_t pair_[2] = {0, 0};
    uint8_t symbuf_[4] = {0, 0, 0, 0};
    unsigned shreg_ = 1;
    TextFilter tf_;
};

} // namespace

std::unique_ptr<Engine> makeIfkEngine(const KbdConfig& c) { return std::make_unique<IfkEngine>(c); }
std::unique_ptr<Modulator> makeIfkMod(const KbdConfig& c, double centre) { return std::make_unique<IfkMod>(c, centre); }

} // namespace kbd
} // namespace dect2
