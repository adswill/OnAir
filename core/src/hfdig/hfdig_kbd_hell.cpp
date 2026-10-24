// Hellschreiber (hfdig_kbd.h), after fldigi's src/feld/feld.cxx: the characters as pixel columns of 14 pixels (the 7 x 7 Feld font, every
// pixel sent twice in height), sent bottom row first, on and off keying of one tone (Feld Hell, Slow Hell, HellX5, HellX9) or a
// frequency shift of the tone (FSK Hell 245, FSK Hell 105, Hell 80). Nothing is decoded to text: the receiver draws what it hears.
//   mode          columns/s  pixels/s   keying
//   Feld Hell     17.5       245        on / off, 4 ms raised cosine edges
//   Slow Hell     2.1875     30.6       on / off
//   Hell 80       35         490        +-150 Hz
//   HellX5        87.5       1225       on / off, square
//   HellX9        157.5      2205       on / off, square
//   FSK Hell 245  17.5       245        +-61.25 Hz
//   FSK Hell 105  17.5       245        +-27.5 Hz
// The receiver mixes the tone to 0 Hz, low passes, takes the envelope (or the frequency), averages one pixel period and cuts the pixel
// stream into columns. Hell has no clock: the column boundary is found from the blank rows of the font (the rows 0, 1, 12, 13 are
// never inked), so the text stands upright without the user shifting the picture.
#include "hfdig_kbd_dsp.h"
#include "hfdig_kbd_tables.h"

namespace dect2 {
namespace kbd {
namespace {

struct HellP {
    double colRate;
    bool fsk;
    double shift;   // FSK: total shift (the "bandwidth" of fldigi)
};
// The order of the names in hfdig_kbd.cpp: Feld Hell, Slow Hell, Hell 80, HellX5, HellX9 (+ FSK Hell 245, FSK Hell 105 below)
HellP params(int v) {
    switch (v) {
    case 0: return {17.5, false, 0};
    case 1: return {2.1875, false, 0};
    case 2: return {35, true, 300};      // Hell 80
    case 3: return {87.5, false, 0};     // HellX5
    case 4: return {157.5, false, 0};    // HellX9
    case 5: return {17.5, true, 122.5};  // FSK Hell 245
    default: return {17.5, true, 55};    // FSK Hell 105
    }
}

} // namespace

// Cyrillic and Greek capitals (lower case folds to the capital): drawn as the Latin letter they look like, or as five rows of five pixels
struct Lk { int cp; char a; };
const Lk kLook[] = {{0x410,'A'},{0x412,'B'},{0x415,'E'},{0x41A,'K'},{0x41C,'M'},{0x41D,'H'},{0x41E,'O'},{0x420,'P'},{0x421,'C'},{0x422,'T'},{0x425,'X'},{0x423,'Y'},
    {0x391,'A'},{0x392,'B'},{0x395,'E'},{0x396,'Z'},{0x397,'H'},{0x399,'I'},{0x39A,'K'},{0x39C,'M'},{0x39D,'N'},{0x39F,'O'},{0x3A1,'P'},{0x3A4,'T'},{0x3A5,'Y'},{0x3A7,'X'},{0x406,'I'}};
struct Dr { int cp; unsigned char r[5]; };
const Dr kDraw[] = {
    {0x411,{31,16,30,17,30}},{0x413,{31,16,16,16,16}},{0x414,{14,10,10,31,17}},{0x416,{21,14,4,14,21}},{0x417,{30,1,14,1,30}},{0x418,{17,19,21,25,17}},
    {0x41B,{15,9,9,9,25}},{0x41F,{31,17,17,17,17}},{0x424,{4,14,21,14,4}},{0x426,{18,18,18,31,1}},{0x427,{17,17,15,1,1}},{0x428,{21,21,21,21,31}},
    {0x429,{21,21,21,31,1}},{0x42A,{24,8,14,9,14}},{0x42B,{17,17,25,21,25}},{0x42C,{16,16,30,17,30}},{0x42D,{30,1,15,1,30}},{0x42E,{18,21,29,21,18}},{0x42F,{15,17,15,5,9}},
    {0x393,{31,16,16,16,16}},{0x394,{4,10,10,17,31}},{0x398,{14,17,31,17,14}},{0x39B,{4,10,10,17,17}},{0x39E,{31,0,14,0,31}},{0x3A0,{31,17,17,17,17}},
    {0x3A3,{31,16,8,16,31}},{0x3A6,{4,14,21,14,4}},{0x3A8,{21,21,14,4,4}},{0x3A9,{14,17,17,10,27}}};
// Latin-1 letters 0xC0 .. 0xFF: base letter and accent (g grave, a acute, c circumflex, t tilde, d diaeresis, r ring, '-' none)
namespace {
bool hellGlyphBuiltin(int cp, unsigned short out[14]) {
    static const char* kBase = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    static const char* kAcc  = "gactdr--gacdgacd-tgactd--gacda--gactdr--gacdgacd-tgactd--gacda-d";
    int base = cp;
    char acc = '-';
    bool ok = true;
    if (cp >= 0xC0 && cp <= 0xFF) {
        base = (uint8_t)kBase[cp - 0xC0]; acc = kAcc[cp - 0xC0];
        if (base == 's') base = 'B'; else if (base == 'T' || base == 't') base = 'P'; else if (base == '*') base = 'x';
        if (base == 'P' + 0 && cp == 0xFE) base = 'p';
    } else if (cp == 0xA0 || cp == '\t') base = ' ';
    else if (cp >= 0x20 && cp <= 0x7E) base = cp;
    else {
        int u = cp;
        if (u >= 0x430 && u <= 0x44F) u -= 0x20;                        // Cyrillic small -> capital
        else if (u == 0x451) u = 0x415, acc = 'd';                      // io
        else if (u == 0x401) u = 0x415, acc = 'd';
        else if (u == 0x419 || u == 0x439) u = 0x418, acc = 'r';        // short i
        else if (u >= 0x3B1 && u <= 0x3C9 && u != 0x3C2) u -= 0x20;     // Greek small -> capital
        else if (u == 0x3C2) u = 0x3A3;
        else if (u >= 0x3AC && u <= 0x3CE) u = 0;                       // Greek with tonos: not drawn
        base = '?'; ok = false;
        for (const Lk& l : kLook) if (l.cp == u) { base = l.a; ok = true; }
        if (ok == false)
            for (const Dr& d : kDraw) if (d.cp == u) {
                for (int i = 0; i < 14; i++) out[i] = 0;
                for (int i = 0; i < 5; i++) out[2 + 2 * i] = out[3 + 2 * i] = (unsigned short)(d.r[i] << 11);
                ok = true;
                break;
            }
        if (ok && base == '?') {   // drawn glyph: add the accent and return
            unsigned short m = acc == 'd' ? 0x5000 : acc == 'r' ? 0x6000 : 0;
            if (m) { out[0] |= m; out[1] |= m; }
            return true;
        }
    }
    for (int i = 0; i < 14; i++) out[i] = kHellFont[base - ' '][i];
    unsigned short m = 0;
    switch (acc) { case 'g': m = 0x4000; break; case 'a': m = 0x2000; break; case 'c': m = 0x5000; break; case 't': m = 0x6800; break;
                   case 'd': m = 0x5000; break; case 'r': m = 0x6000; break; default: break; }
    if (m) { out[0] |= m; out[1] |= m; }
    return ok;
}
} // namespace

bool hellGlyph(int cp, unsigned short out[14]) {
    if (hellGlyphBuiltin(cp, out)) return true;
    return hellUserGlyph(cp, out);   // the user's font file, for what the built-in glyphs cannot draw
}

namespace {

// the bits of column `col` of a glyph (bit k: pixel k in the order sent, bottom row first), -1 when the character has ended
int fontColumn(const unsigned short* g, int col) {
    if (col > 15) return -1;
    int ord = 0;
    for (int i = 0; i < 14; i++) ord |= g[i];
    const int mask = 1 << (15 - col);
    int bits = 0;
    for (int k = 0; k < 14; k++) if (g[13 - k] & mask) bits |= 1 << k;
    const int test = (1 << (15 - col)) - 1;
    if (bits == 0 && (ord & test) == 0) return -1;
    return bits;
}

// ------------------------------------------------------------------ transmitter
class HellMod : public Modulator {
public:
    HellMod(const KbdConfig& c, double centre) : p_(params(std::clamp(c.variant, 0, 6))), centre_(centre) {
        pixRate_ = 14 * p_.colRate;
        for (int i = 0; i < 32; i++) on_[i] = 0.5 * (1.0 - std::cos(kPi * i / 33));
        for (int i = 0; i < 32; i++) off_[31 - i] = on_[i];
        for (int i = 0; i < 3; i++) { pendChar('.'); }
    }
    void next(std::vector<float>& out, TextSrc& src) override {
        if (pix_.empty()) {
            int c = src.next();
            if (c >= 0x80) {   // UTF-8: gather the continuation bytes
                const int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
                int cp = n == 3 ? (c & 7) : n == 2 ? (c & 15) : (c & 31);
                for (int i = 0; i < n; i++) cp = (cp << 6) | (src.next() & 63);
                c = n ? cp : '?';
            }
            if (c == '\r' || c == '\n') c = ' ';
            pendChar(c);
        }
        // one character at a time is queued as pixels; send the pixels of one column
        for (int i = 0; i < 14 && !pix_.empty(); i++) { const int cur = pix_.front(); pix_.erase(pix_.begin()); sendPixel(cur, pix_.empty() ? 0 : pix_.front(), out); }
    }
private:
    void nullColumn() { for (int i = 0; i < 14; i++) pix_.push_back(0); }
    void pendChar(int c) {
        nullColumn();
        if (c == ' ') { nullColumn(); nullColumn(); nullColumn(); }
        else {
            int col = 0, bits;
            unsigned short g[14];
            hellGlyph(c, g);
            while ((bits = fontColumn(g, col)) != -1) {
                for (int i = 0; i < 14; i++) pix_.push_back((bits >> i) & 1);
                col++;
            }
        }
        nullColumn();
    }
    void sendPixel(int cur, int next, std::vector<float>& out) {
        const double fs = kFs;
        double tone = centre_;
        if (p_.fsk) tone += (cur ? -1 : 1) * p_.shift / 2.0;
        int n = 0;
        for (;;) {
            double amp;
            if (p_.fsk) amp = 1.0;
            else if (p_.colRate > 50) amp = cur;
            else if (prev_ == 0 && cur == 1) amp = n < 32 ? on_[n] : 1.0;
            else if (cur == 1 && next == 0) amp = n < 32 ? off_[n] : 1.0;
            else amp = cur;
            out.push_back((float)(amp * std::sin(ph_)));
            ph_ += 2 * kPi * tone / fs;
            if (ph_ > 2 * kPi) ph_ -= 2 * kPi;
            n++;
            cnt_ += pixRate_ / fs;
            if (cnt_ >= 1.0) { cnt_ -= 1.0; break; }
        }
        prev_ = cur;
    }
    HellP p_;
    double centre_, pixRate_, ph_ = 0, cnt_ = 0, on_[32], off_[32];
    int prev_ = 0;
    std::vector<int> pix_;
};

// ------------------------------------------------------------------ receiver
class HellEngine : public Engine {
public:
    explicit HellEngine(const KbdConfig& c) : p_(params(std::clamp(c.variant, 0, 6))) {
        pixRate_ = 14 * p_.colRate;
        filtBw_ = p_.fsk ? 5 * std::round(4.0 * p_.shift / 5.0) : 5 * std::round(1.2 * pixRate_ / 5.0);
        if (!p_.fsk) filtBw_ = std::max(filtBw_, 2.0 * pixRate_ * 0.6);
        setCentre(c.centreHz);
    }
    double bandwidth() const override { return p_.fsk ? p_.shift * 1.5 : filtBw_; }
    void setCentre(double hz) override { centre_ = hz; build(); }
    void reset() override { build(); }
    void feed(const float* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            // mix to 0 Hz
            const double c = std::cos(ph_), s = std::sin(ph_);
            ph_ -= 2 * kPi * centre_ / kFs;
            if (ph_ < -2 * kPi) ph_ += 2 * kPi;
            const double zr = x[i] * c, zi = x[i] * s;
            // low pass: FIR over the last kTaps mixed samples
            pTot_ += ((double)x[i] * x[i] - pTot_) * alpha_;
            hr_[hp_] = zr; hi_[hp_] = zi;
            double ar = 0, ai = 0;
            for (int k = 0; k < kTaps; k++) { const int j = (hp_ - k + kTaps) % kTaps; ar += h_[k] * hr_[j]; ai += h_[k] * hi_[j]; }
            hp_ = (hp_ + 1) % kTaps;
            double v;
            const double env = std::hypot(ar, ai);
            pFilt_ += (env * env - pFilt_) * alpha_; warm_ += 1;
            if (p_.fsk) {
                const double dr = ar * pr_ + ai * pi_, di = ai * pr_ - ar * pi_;   // z * conj(prev)
                pr_ = ar; pi_ = ai;
                const double f = std::atan2(di, dr) * kFs / (2 * kPi);          // Hz from the centre
                fs_ += (f - fs_) * 0.25;                                          // average of a few samples
                v = std::clamp(0.5 - fs_ / p_.shift, 0.0, 1.0) * (env > 1e-9 ? 1.0 : 0.0);
            } else v = env;
            acc_ += v; accN_++;
            envMax_ = std::max(envMax_, env);
            cnt_ += pixRate_ / kFs;
            if (cnt_ >= 1.0) { cnt_ -= 1.0; pixel(acc_ / std::max(1, accN_), envMax_); acc_ = 0; accN_ = 0; envMax_ = 0; }
        }
    }

private:
    static constexpr int kTaps = 129;
    void build() {
        // the power in the filter is averaged over about 150 / B seconds (0.5 .. 8 s): its noise-only scatter is 8 % or less, the lock level 6 scatters above 1
        const double tau = std::clamp(150.0 / filtBw_, 0.5, 8.0);
        alpha_ = 1.0 / (tau * kFs);
        const double fc = filtBw_ / 2.0 / kFs;
        double sum = 0;
        for (int k = 0; k < kTaps; k++) {
            const double m = k - (kTaps - 1) / 2.0;
            const double a = 2 * kPi * fc * m;
            const double w = 0.54 + 0.46 * std::cos(2 * kPi * m / (kTaps - 1));
            h_[k] = (std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a) * w;
            sum += h_[k];
        }
        for (double& v : h_) v /= sum;
        double g = 0;
        for (double v : h_) g += v * v;
        noiseFrac_ = g;   // what a white noise leaves in the filter (the short filter is wider than B at the slow modes)
        lockRatio_ = 1.0 + 6.0 / std::sqrt(noiseFrac_ * kFs * tau);
        std::fill(std::begin(hr_), std::end(hr_), 0.0);
        std::fill(std::begin(hi_), std::end(hi_), 0.0);
        hp_ = 0; ph_ = 0; cnt_ = 0; acc_ = 0; accN_ = 0; envMax_ = 0; pr_ = 1; pi_ = 0; fs_ = 0;
        peak_ = 1e-9; pTot_ = pFilt_ = 0; warm_ = 0; count_ = 0; start_ = 2; col_.clear(); pend_.clear();
        for (double& v : prof_) v = 0;
        locked = false; snrDb = 0;
    }
    void pixel(double v, double env) {
        // level for the picture: AM: against the strongest pixels; FSK: the frequency already is 0 .. 1
        double ink;
        if (p_.fsk) ink = v;
        else {
            peak_ = std::max(v, peak_ * (1.0 - 0.02 / 14));
            ink = std::clamp(v / peak_, 0.0, 1.0);
        }
        // presence: the power in the filter against what white noise of the same total power would put there (1 for noise alone)
        const double ratio = pTot_ > 1e-12 ? (pFilt_ / pTot_) / noiseFrac_ : 0.0;
        (void)env;
        locked = ratio > lockRatio_ && warm_ > 3.0 / alpha_;
        snrDb = (float)(10 * std::log10(std::max(1.0, ratio)));
        const int m = (int)(count_ % 14);
        count_++;
        prof_[m] += (ink - prof_[m]) * 0.02;
        if ((count_ % 14) == 0) realign();
        col_.push_back((uint8_t)std::lround(ink * 255));
        if ((count_ % 14) == (uint64_t)start_) {   // the pixel just stored was the last of a column
            if (col_.size() == 14) {
                // top row first: the last pixel sent is the top. Before the lock the columns wait (the lock takes up to 3 averaging times, longer than a
                // short HellX5 / X9 burst): when it comes, the columns of that wait are drawn too, so a burst is not cut at its start
                uint8_t c14[14];
                for (int r = 0; r < 14; r++) c14[r] = col_[col_.size() - 1 - r];
                if (locked) {
                    if (!pend_.empty()) { hell.insert(hell.end(), pend_.begin(), pend_.end()); pend_.clear(); }
                    hell.insert(hell.end(), c14, c14 + 14);
                } else {
                    pend_.insert(pend_.end(), c14, c14 + 14);
                    const size_t keep = 14 * (size_t)(p_.colRate * 3.2 / (alpha_ * kFs) + 2);
                    if (pend_.size() > keep) pend_.erase(pend_.begin(), pend_.begin() + (long)(pend_.size() - keep));
                }
            }
            col_.clear();
        }
        if (col_.size() > 28) col_.clear();
    }
    // the four consecutive pixel phases with the least ink are the blank rows 12, 13, 0, 1: the column starts two phases after them
    void realign() {
        int bw = 0;
        double bs = 1e9, cur = 0;
        for (int w = 0; w < 14; w++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += prof_[(w + k) % 14];
            if (s < bs) { bs = s; bw = w; }
        }
        // the phase after the last pixel of a column: the blank window starts at pixel 12 of a column, so the column ends 2 + 2 pixels ahead
        for (int k = 0; k < 4; k++) cur += prof_[(blankStart_ + k) % 14];
        if (bs < cur * 0.7 - 1e-6) {
            blankStart_ = bw;
            start_ = (bw + 2) % 14;   // the column starts at pixel k = 0 which is two after the window's end... see fontColumn
            col_.clear();
        }
    }

    HellP p_;
    double pixRate_, filtBw_, centre_ = 1500, h_[kTaps], hr_[kTaps], hi_[kTaps];
    double ph_ = 0, cnt_ = 0, acc_ = 0, envMax_ = 0, pr_ = 1, pi_ = 0, fs_ = 0, peak_ = 1e-9, pTot_ = 0, pFilt_ = 0, prof_[14];
    double alpha_ = 1e-4, lockRatio_ = 1.5, warm_ = 0, noiseFrac_ = 0.01;
    int hp_ = 0, accN_ = 0, start_ = 2, blankStart_ = 12;
    uint64_t count_ = 0;
    std::vector<uint8_t> col_;
    std::vector<uint8_t> pend_;   // columns before the lock (the last 3.2 averaging times)
};

} // namespace

std::unique_ptr<Engine> makeHellEngine(const KbdConfig& c) { return std::make_unique<HellEngine>(c); }
std::unique_ptr<Modulator> makeHellMod(const KbdConfig& c, double centre) { return std::make_unique<HellMod>(c, centre); }

} // namespace kbd
} // namespace dect2
