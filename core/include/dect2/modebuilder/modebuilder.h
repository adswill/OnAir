// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode Builder: a decoder built from a description instead of code. A mode definition (JSON, docs/MODE-BUILDER.md, the same format the
// Signal Lab writes) gives the modulation, line code, sync word, frame length, scrambler, FEC, interleaver, CRC, fields and a display
// template; Receiver builds its pipeline from it and turns IQ into decoded frames, and generate() makes a test signal from the same
// definition (round trip tests, the "Generate test signal" button). Library dect2_modebuilder, no app dependencies.
#pragma once
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2::modebuilder {

using cf = std::complex<float>;
constexpr int kFormatVersion = 1;

enum class Mod { OOK, ASK, FSK, GFSK, GMSK, PSK };
enum class Pulse { None, Rrc, Rc, Gaussian };
enum class Line { NRZ, NRZI, Manchester, ManchesterThomas, DiffManchester, Miller, B4B5, B8B10 };
enum class Polarity { Normal, Inverted, Auto };
enum class Scrambler { None, Additive, Multiplicative };
enum class LenMode { Fixed, LengthField, EndFlag };
enum class Fec { None, Repetition, Hamming, Convolutional, ReedSolomon };
enum class FieldType { Uint, Int, Bool, Enum, Bitflags, Ascii, Bcd, Float32, Lat, Lon, Hex };
enum class CrcKind { None, Crc, Sum8, Xor8 };

const char* modName(Mod m);
const char* lineName(Line l);
const char* fecName(Fec f);
const char* fieldTypeName(FieldType t);

struct BitPattern {
    uint64_t value = 0;            // right-aligned: the last bit sent is bit 0
    int bits = 0;
    std::vector<uint8_t> toBits() const;   // first bit sent first
};

struct CrcModel {
    std::string name;              // preset name or "" (custom)
    int width = 16;
    uint32_t poly = 0x1021, init = 0xFFFF, xorout = 0;
    bool refin = false, refout = false;
};
// the named models (CRC-8, CRC-16/CCITT-FALSE, CRC-16/IBM, CRC-32 ...); false when the name is not known (case does not matter)
bool crcPreset(const std::string& name, CrcModel& out);
std::vector<std::string> crcPresetNames();
uint32_t crcCompute(const CrcModel& m, const uint8_t* d, size_t n);

struct Field {
    std::string name;
    int offsetBits = 0, widthBits = 8;
    FieldType type = FieldType::Uint;
    bool little = false;
    double scale = 1, offset = 0;
    std::string units, format;     // format: "" / "dec", "hex", or "%.2f"
    std::vector<std::pair<int64_t, std::string>> enumTable;
    std::vector<std::pair<int, std::string>> flags;
};

struct DisplayRule {
    std::string whenField;         // "" always
    double equals = 0;
    std::string tmpl;
};

struct Definition {
    int version = kFormatVersion;
    std::string id, name, description;
    double freqMhz = 433.92, sampleRate = 250000;
    // modulation
    Mod mod = Mod::FSK;
    int order = 2;
    double symbolRate = 0, deviationHz = 0, bt = 0.5, offsetHz = 0;
    bool differential = false, gray = true;
    std::vector<int> symbolMap;    // empty: natural or Gray
    // filter
    double bandwidthHz = 0;        // 0: automatic
    Pulse pulse = Pulse::None;
    bool pulseSet = false;
    double rolloff = 0.35;
    // line code, bits
    Line line = Line::NRZ;
    int nrziToggleOn = 0;          // NRZI: the bit value that changes the level (0 HDLC, 1 NRZ-M / "Differential")
    Polarity polarity = Polarity::Auto;
    bool lsbFirst = false;
    BitPattern preamble, sync;
    int syncMaxErrors = 0;
    double squelchDb = 6;          // 0: always open
    // scrambler
    Scrambler scrambler = Scrambler::None;
    std::vector<int> taps;
    uint32_t seed = 0xFFFFFFFFu;
    // frame length
    LenMode lenMode = LenMode::Fixed;
    int fixedBytes = 8;
    int lenOffsetBits = 0, lenWidthBits = 8, lenAdjust = 0, minBytes = 1, maxBytes = 255;
    double lenMultiplier = 1;
    BitPattern flag;
    bool bitStuffing = false;
    // FEC
    Fec fec = Fec::None;
    int repN = 3;
    int hamN = 7, hamK = 4;
    bool hamExtended = false;
    int convK = 7;
    std::vector<uint32_t> convPolys{0x79, 0x5B};
    bool convTail = true;
    int rsN = 255, rsK = 223, rsM = 8, rsFcr = 0, rsPrim = 1;
    uint32_t rsPoly = 0x11D;
    // interleaver
    int ilRows = 0, ilCols = 0;
    // check value
    CrcKind crcKind = CrcKind::None;
    CrcModel crc;
    int crcFrom = 0;
    int crcTo = 0;                 // 0: up to the CRC; > 0 a byte index (exclusive); < 0 bytes before the CRC
    bool crcToSet = false;
    int crcPosition = -1;          // -1: at the end
    bool crcLittle = false;
    bool showBadFrames = false;
    // fields and display
    std::vector<Field> fields;
    std::vector<DisplayRule> display;

    // derived
    int levels() const;            // M
    int bitsPerSymbol() const;
    double deviation() const;      // effective deviation of the outer tones
    double bandwidth() const;      // effective channel filter width
    Pulse pulseShape() const;
    int crcBytes() const;          // bytes the check value takes in the frame (0: none)
    int longestFrameBytes() const;
};

struct Issue {
    bool error = true;
    std::string where;             // "modulation.symbol_rate", "line 7", ""
    std::string msg;
    std::string text() const { return where.empty() ? msg : where + ": " + msg; }
};
struct ParseResult {
    bool ok = false;               // no errors (there may be warnings)
    Definition def;
    std::vector<Issue> issues;
};
ParseResult parseDefinition(const std::string& json);
std::vector<Issue> validate(const Definition& d);   // the checks of meaning (parseDefinition runs them too)
std::string toJson(const Definition& d);            // pretty, keys in the order of the documentation
std::string makeId(const std::string& name);        // "OOK 433 sensor" -> "ook-433-sensor"
bool parsePattern(const std::string& text, int bits, BitPattern& out, std::string& err);   // "0x2DD4", "0b1011", "1011"; bits 0: from the text
std::string patternText(const BitPattern& p);       // hex when the length is a multiple of 4, else bits

// ---------------------------------------------------------------- the stages (also used by the tests)
namespace stage {
std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& bytes, bool lsbFirst);
std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits, bool lsbFirst);   // whole bytes only
std::vector<uint8_t> lineEncode(const std::vector<uint8_t>& bits, Line l, int nrziToggleOn);
// decodes chips from phase on; pos[i] is the chip index where decoded bit i starts; errors counts invalid code words
std::vector<uint8_t> lineDecode(const std::vector<uint8_t>& chips, Line l, int nrziToggleOn, int phase, std::vector<int>* pos, int* errors);
int linePhases(Line l);            // the alignments a decoder has to try: 1, 2, 5 or 10
double lineExpansion(Line l);      // chips per bit
void scramble(std::vector<uint8_t>& bits, const Definition& d, bool encode);
void interleave(std::vector<uint8_t>& bits, int rows, int cols, bool forward);
size_t codedBits(const Definition& d, size_t dataBits);
std::vector<uint8_t> fecEncode(const Definition& d, const std::vector<uint8_t>& dataBits);
// coded -> data bits (dataBits of them); corrected: bits or symbols fixed; false when a code word could not be corrected
bool fecDecode(const Definition& d, const std::vector<uint8_t>& coded, size_t dataBits, std::vector<uint8_t>& out, int& corrected);
std::vector<uint8_t> hdlcStuff(const std::vector<uint8_t>& bits);
}   // namespace stage

// Reed-Solomon over GF(2^m) (stage of the FEC, exposed for the tests). Code words are highest degree first: data then parity.
class ReedSolomon {
public:
    ReedSolomon(int m, uint32_t poly, int n, int k, int fcr, int prim);
    bool valid() const { return ok_; }
    std::vector<int> encode(const std::vector<int>& data) const;            // data.size() <= k (shortened), returns data + parity
    int decode(std::vector<int>& cw) const;                                 // in place, cw.size() = data + n - k; corrected symbols, -1 failure
private:
    int n_, k_, fcr_, prim_, nn_;
    bool ok_ = false;
    std::vector<int> exp_, log_, gen_;
    int mul(int a, int b) const;
};

// ---------------------------------------------------------------- frames
struct FieldValue {
    std::string name, text, units;
    double num = 0;                // the scaled value (numbers)
    int64_t raw = 0;
    bool numeric = false;
};
struct Frame {
    double timeSec = 0;            // from the first sample fed
    std::vector<uint8_t> bytes;    // the decoded frame (after the sync word), check value included
    bool hasCrc = false, crcOk = true;
    int syncErrors = 0;
    int fecCorrected = 0;
    bool fecFailed = false;
    double snrDb = 0;
    std::vector<FieldValue> fields;
    std::string display;
    bool good() const { return crcOk && !fecFailed; }
};
// the fields and the display line of a decoded frame (bytes, hasCrc, crcOk filled in)
void interpretFrame(const Definition& d, Frame& f);
// check value of frame bytes: present = the definition has one, ok = it matches
void checkCrc(const Definition& d, const std::vector<uint8_t>& bytes, bool& present, bool& ok);
// writes the check value into the frame (generator)
void fillCrc(const Definition& d, std::vector<uint8_t>& bytes);
// decode the frame that starts at the first bit after the sync word; bits are line-decoded channel bits. usedBits: how many were taken.
bool decodeFrameBits(const Definition& d, const std::vector<uint8_t>& bits, size_t start, bool final, Frame& out, size_t& usedBits);

std::string framesCsv(const Definition& d, const std::vector<Frame>& frames);
std::string framesJson(const Definition& d, const std::vector<Frame>& frames);

// ---------------------------------------------------------------- receiver
struct RxStats {
    uint64_t symbols = 0, syncHits = 0, frames = 0, badFrames = 0;
    double snrDb = 0;              // of the last frame
    bool open = false;             // squelch
    double symbolRateHz = 0;       // the tracked clock
    double fs1 = 0;                // the working rate after decimation
};

class Receiver {
public:
    explicit Receiver(const Definition& d);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    // input rate and an extra offset of the signal from the centre (added to modulation.offset_hz); resets everything
    void configure(double inputRate, double extraOffsetHz = 0);
    void feed(const cf* iq, size_t n);
    void flush();                  // end of the recording: decode what is left
    std::vector<Frame> take();     // frames since the last call
    RxStats stats() const;
    const Definition& definition() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// a whole recording at once
std::vector<Frame> decodeIq(const Definition& d, const std::vector<cf>& iq, double fs, double extraOffsetHz = 0, RxStats* stats = nullptr);

// ---------------------------------------------------------------- generator
struct GenOptions {
    double fs = 0;                 // 0: the definition's sample_rate
    double snrDb = 30;             // in the channel filter bandwidth; >= 200: no noise
    double cfoHz = 0;              // frequency error added to modulation.offset_hz
    double clockPpm = 0;           // transmitter symbol clock error
    double gapSymbols = 60;        // silence between frames (and before the first)
    double phase = 0.7;            // carrier phase at the start (radians)
    uint32_t seed = 1;
};
// a frame with random field contents, a valid length field and check value
std::vector<uint8_t> randomFrame(const Definition& d, uint32_t& rng);
// on-air chips of one frame: preamble, sync word and the coded frame, line coded
std::vector<uint8_t> frameChips(const Definition& d, const std::vector<uint8_t>& frameBytes);
std::vector<cf> generate(const Definition& d, const std::vector<std::vector<uint8_t>>& frames, const GenOptions& o);

// ---------------------------------------------------------------- library
struct Example { std::string file, text; };
const std::vector<Example>& examples();     // data/modes/*.json, compiled in
// the definitions saved in a folder (*.json), sorted by name; files that do not parse are listed with their error
struct Saved { std::string path, id, name, error; };
std::vector<Saved> listFolder(const std::string& dir);
bool saveDefinition(const std::string& dir, const Definition& d, const std::string& text, std::string& pathOut, std::string& err);
bool readText(const std::string& path, std::string& out, std::string& err);
bool writeText(const std::string& path, const std::string& text, std::string& err);

}   // namespace dect2::modebuilder
