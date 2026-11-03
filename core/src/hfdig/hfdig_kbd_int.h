// Inside the keyboard-to-keyboard decoders (hfdig_kbd.h): the engine each family implements, the transmitter base, and the small DSP parts
// they share (FFT, fast Hadamard transform, Gray code, Viterbi decoder for the K = 7 / K = 15 codes, the text filter).
#pragma once
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace kbd {

constexpr double kFs = 8000.0;
using dect2::math::kPi;
using cf = std::complex<float>;

// One receiver of a family. feed() takes the 8 kHz audio; what it decodes appears in `out` (taken by the caller) and `hell`.
struct Engine {
    virtual ~Engine() = default;
    virtual void feed(const float* x, size_t n) = 0;
    virtual void reset() = 0;                       // the signal is forgotten
    virtual void setCentre(double hz) = 0;          // audio frequency of the middle of the signal (resets the signal)
    virtual double bandwidth() const = 0;           // Hz
    std::string out;                                // new characters since the caller last took them
    std::vector<uint8_t> hell;                      // Hell: new pixel columns (kKbdHellRows bytes each)
    bool locked = false;
    float snrDb = 0;
    double offsetHz = 0;                            // tuning error found
    // MFSK pictures: the engine owns the picture being received; the caller copies it when `picDirty` (and clears the flags)
    std::shared_ptr<KbdPicture> pic;
    bool picDirty = false, picDone = false;
    bool busy = false;                              // a picture is coming: leave the centre alone
};
std::unique_ptr<Engine> makeOliviaEngine(const KbdConfig& c);   // also Contestia
std::unique_ptr<Engine> makeMfskEngine(const KbdConfig& c);
std::unique_ptr<Engine> makeIfkEngine(const KbdConfig& c);      // DominoEX and THOR
std::unique_ptr<Engine> makeHellEngine(const KbdConfig& c);

// Characters of the text: printable ASCII, tab and newline kept (13 is a newline unless a 10 follows), backspace erases; the rest dropped
struct TextFilter {
    bool lastCr = false;
    void put(std::string& o, int c) {
        if (c == 13) { o += '\n'; lastCr = true; return; }
        if (c == 10) { if (!lastCr) o += '\n'; lastCr = false; return; }
        lastCr = false;
        if (c == 8) { if (!o.empty() && o.back() != '\n') o.pop_back(); else o += '\b'; return; }
        if (c == 9 || (c >= 32 && c < 127)) o += (char)c;
        else if (c >= 128) o += (char)c;   // UTF-8 bytes as sent
    }
};

// ------------------------------------------------------------------ FFT
class Fft {
public:
    Fft() = default;
    explicit Fft(int n) { init(n); }
    void init(int n) {
        n_ = n;
        tw_.resize(n / 2);
        for (int i = 0; i < n / 2; i++) tw_[i] = cf((float)std::cos(2 * kPi * i / n), (float)-std::sin(2 * kPi * i / n));
        rev_.assign(n, 0);
        int lg = 0;
        while ((1 << lg) < n) lg++;
        for (int i = 0; i < n; i++) { int r = 0; for (int b = 0; b < lg; b++) if (i & (1 << b)) r |= 1 << (lg - 1 - b); rev_[i] = r; }
    }
    int size() const { return n_; }
    void run(cf* x) const {
        const int n = n_;
        for (int i = 0; i < n; i++) if (rev_[i] > i) std::swap(x[i], x[rev_[i]]);
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; j++) {
                    const cf t = x[i + j + half] * tw_[j * step];
                    x[i + j + half] = x[i + j] - t;
                    x[i + j] += t;
                }
        }
    }
private:
    int n_ = 0;
    std::vector<cf> tw_;
    std::vector<int> rev_;
};

// Fast Hadamard transform (the order of fldigi's FHT) and its inverse
template <class T> void fht(T* d, int len) {
    for (int step = 1; step < len; step *= 2)
        for (int p = 0; p < len; p += 2 * step)
            for (int q = p; q - p < step; q++) { const T a = d[q], b = d[q + step]; d[q] = b + a; d[q + step] = b - a; }
}
template <class T> void ifht(T* d, int len) {
    for (int step = len / 2; step; step /= 2)
        for (int p = 0; p < len; p += 2 * step)
            for (int q = p; q - p < step; q++) { const T a = d[q], b = d[q + step]; d[q] = a - b; d[q + step] = a + b; }
}

inline unsigned grayCode(unsigned b) { return b ^ (b >> 1); }
inline unsigned binaryCode(unsigned g) { for (unsigned s = 16; s; s >>= 1) g ^= g >> s; return g; }

// ------------------------------------------------------------------ transmitters
// Characters for a modulator: the text, repeated; `pos` counts what was taken
struct TextSrc {
    std::string text;
    size_t pos = 0;
    uint64_t sent = 0;
    int next() { if (text.empty()) return 0; const int c = (uint8_t)text[pos]; pos = (pos + 1) % text.size(); sent++; return c; }
};
// A modulator produces the audio of the text, a piece at a time (a symbol or a block), at the centre it was made for, and has no
// clock error; Tx adds the lead-in, the clock error and the amplitude.
struct Modulator {
    virtual ~Modulator() = default;
    virtual void next(std::vector<float>& out, TextSrc& src) = 0;   // append audio (the amplitude about +-1)
};
std::unique_ptr<Modulator> makeOliviaMod(const KbdConfig& c, double centreHz);
std::unique_ptr<Modulator> makeMfskMod(const KbdConfig& c, double centreHz, std::shared_ptr<const KbdPicture> pic = nullptr, int picAfterChars = 40);
std::unique_ptr<Modulator> makeIfkMod(const KbdConfig& c, double centreHz);
std::unique_ptr<Modulator> makeHellMod(const KbdConfig& c, double centreHz);

int mfskVaridec(unsigned code);   // hfdig_kbd_mfsk.cpp: the character of an MFSK varicode word with its leading 1, -1 when none

} // namespace kbd
} // namespace dect2
