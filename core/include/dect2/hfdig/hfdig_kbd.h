// Keyboard-to-keyboard modes as one decoder of the HF digital receiver (hfdig_rx.h, decoder bit 32): Olivia, Contestia, MFSK,
// THOR, DominoEX and Hellschreiber. One family runs at a time (setConfig); each is its own entry in the mode list (modes.cpp).
// This header, hfdig_kbd*.cpp and app/hfdig/hfdig_kbd_ui.cpp belong to these modes alone. The parameters follow fldigi (GPL, w1hkj/fldigi):
//   Olivia     MFSK of 2^n tones (4 .. 256) in 125 .. 2000 Hz; each block of n seven-bit characters is Walsh-Hadamard coded (64 symbols),
//              scrambled with a fixed code, spread over the tones; the receiver searches time and frequency (Jalocha's code)
//   Contestia  the same with six-bit characters (32 symbols per block), another scrambling code, upper case only
//   MFSK       MFSK4 .. MFSK64: 32 (MFSK4 and MFSK8: 64 / 32 .. see mfskVariants) tones, differential Gray coded, K = 7 convolutional
//              code with a 10 symbol diagonal interleaver, varicode text
//   DominoEX   18 tones, incremental frequency keying (the tone changes by 2 + symbol), no FEC, the DominoEX varicode
//   THOR       as DominoEX with the K = 7 and K = 15 convolutional codes, the interleaver and the THOR varicode
//   Hell       Feld Hell, Slow Hell, Hell 80, HellX5 and HellX9: the characters as 7 x 14 pixel columns, on and off keying of one tone,
//              shown as a picture
// Test audio (hfdig_gen.h): modeOpt[0] = 26 Olivia, 27 Contestia, 28 MFSK, 29 THOR, 30 DominoEX, 31 Hell;
//   modeOpt[1] Olivia / Contestia: tones index (0: 32 tones .. see kbdTonesList)   modeOpt[2] bandwidth index (0: 1000 Hz)
//   modeOpt[1] other families: variant (0: the family's default)                 modeVal[0] tuning error of the audio in Hz
//   modeVal[1] sample clock error in ppm of the transmitter
// Checked against the fldigi source's tables and parameters; no recordings of real stations were available.
#pragma once
#include "hfdig_rx.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SynthConfig;
class HfdigTestAudio;

enum KbdFamily { KbdOlivia = 0, KbdContestia, KbdMfsk, KbdThor, KbdDominoex, KbdHell, kKbdFamilies };

constexpr int kKbdTextMax = 8000;       // characters kept in the telemetry text
constexpr int kKbdHellRows = 14;        // pixel rows of a Hell character
constexpr int kKbdHellMaxCols = 3000;   // columns kept in the picture (a ring)

const char* kbdFamilyName(int family);                  // "Olivia", ...
int kbdVariantCount(int family);                        // MFSK, THOR, DominoEX, Hell: the variants; Olivia and Contestia: 0
const char* kbdVariantName(int family, int variant);    // "MFSK16", "THOR 22", "DominoEX 11", "Feld Hell"
extern const int kKbdTonesList[8];                      // Olivia / Contestia tones: 2 4 8 16 32 64 128 256
extern const int kKbdBwList[5];                         // 125 250 500 1000 2000
int kbdDefaultVariant(int family);
double kbdBandwidthHz(const struct KbdConfig& c);       // occupied bandwidth of the signal

struct KbdConfig {
    int family = KbdOlivia;
    int tones = 32, bandwidth = 1000;   // Olivia and Contestia
    int variant = -1;                   // the others (-1: the default of the family)
    double centreHz = 1500;             // audio frequency of the middle of the signal
    bool autoCentre = true;             // look for the signal in the audio while none is locked
};

struct HfdigKbdTelemetry {
    int state = 0;                      // 0 searching, 1 locked (signal found and decoding)
    uint64_t audioSamples = 0;
    KbdConfig cfg;
    double centreHz = 0;                // where the decoder looks now (follows the signal)
    double offsetHz = 0;                // tuning error found by the decoder (Olivia / Contestia, MFSK)
    float snr = 0;                      // signal to noise ratio of the decoder's own measure, dB
    std::string text;                   // decoded text, the last kKbdTextMax characters
    uint64_t chars = 0;
    std::string callsign;               // the last callsign seen in the text ("" none)
    // Hell: the picture. Columns of kKbdHellRows pixels (0 .. 255, row 0 on top), oldest first; hellCols counts the columns kept.
    int hellCols = 0;
    uint64_t hellTotal = 0;             // columns received so far
    std::vector<uint8_t> hell;          // hellCols * kKbdHellRows
};

class HfdigKbd : public HfdigDecoder {
public:
    HfdigKbd();
    ~HfdigKbd() override;
    void reset() override;                            // the signal is forgotten; the text stays
    void feedAudio(const float* x, size_t n) override;
    void telemetry(HfdigKbdTelemetry& out) const;     // any thread
    void setConfig(const KbdConfig& c);               // any thread; takes effect at the next feedAudio (the text stays)
    KbdConfig config() const;
    void clearText();
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
std::unique_ptr<HfdigKbd> makeKbdDecoder();

// Callsign in a line of text (ITU shape: prefix, digit, suffix; at least one letter after the digit); "" when none
std::string kbdFindCallsign(const std::string& text);

// The transmitter (test audio and tests): the text repeated with a pause, with a tuning error and a sample clock error
struct KbdTxConfig {
    KbdConfig cfg;
    std::string text = "CQ CQ CQ DE LY0AIR LY0AIR PSE K  THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789";
    double offsetHz = 0;                // added to the centre
    double clockPpm = 0;                // sample clock error of the transmitter
    double amp = 0.5;                   // peak
    double pauseSec = 0;                // silence after each pass of the text
    double leadSec = 0.3;
};
std::unique_ptr<HfdigTestAudio> makeKbdTx(const KbdTxConfig& cfg);
std::unique_ptr<HfdigTestAudio> makeKbdTestAudio(const SynthConfig& cfg);   // hfdig_kbd_gen.cpp: modeOpt[0] 26 .. 31

// Hellschreiber text in any script: the built-in glyphs cover ASCII, accented Latin, Cyrillic and Greek; a font file (.ttf / .otf) the user chose draws
// the rest (Chinese, Japanese, Korean, symbols). Nothing is bundled. An empty path goes back to the built-in glyphs; false with err when the file cannot be used.
namespace kbd {
bool hellLoadUserFont(const std::string& path, std::string& err);
std::string hellUserFontPath();
} // namespace kbd

} // namespace dect2
