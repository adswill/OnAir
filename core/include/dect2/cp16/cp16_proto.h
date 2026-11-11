// CP16 ("Chinese Pattern 16"): the 16-carrier picture-text mode of the Chinese Radio Amateurs Club (CRAC), China Radio Association group standard
// T/RAC 019-2019 (业余无线电16载频图形文字（CP16）技术要求, 2019). A character is a 16 x 16 dot matrix (the GB2312 dot fonts, as HZK16); the transmitter scans it
// row by row and keys 16 audio carriers on and off, one carrier per column: a waterfall shows the character upright and a person reads it, with no
// decoder (16ASK, A1A-like on-off keying of each carrier, upper or lower sideband through an SSB transmitter's microphone input).
//
// What the public CRAC documents give, and what OnAir uses (docs/CP16-3.9.md has the sources and the choices):
//   carriers      16, equally spaced; spacing 10 .. 20 Hz (the 2013 field trials used 15 .. 19 Hz, mostly 19 Hz; the CRAC article's worked example 11 Hz)
//   scan          one dot row per "pixel period", top row first; carrier i = column i, the lowest audio frequency the left column (upper sideband)
//   character     16 rows, then one blank row between characters: pixel period = character time / 17 (2 s a character: 117.6 ms)
//   speed         1 .. 4 s a character is readable (the CRAC LabView sender: about 4 s; its sample: 225 ms rows, 19.2 Hz from 1107 Hz)
//   keying        a carrier is on for an inked dot, off for a blank one; OnAir keeps each carrier's phase continuous and shapes the edges
//                 (raised cosine, 6 % of a row, at most 8 ms) against the clicks the CRAC guide complains about; every carrier has the same level
//   text          GB2312 characters; ASCII is sent as its full-width form (U+FF01 .. U+FF5E) so every character is one 16 x 16 cell
// Not in the public documents (T/RAC 019-2019 itself is not published free): the audio position of the lowest carrier (OnAir: 1000 Hz by default, any
// value), the exact font (any GB2312 16 x 16 font: OnAir uses the user's HZK16 file or TrueType / OpenType font, else the bundled Noto Sans CJK SC
// drawn at 16 pixels), and how a space is sent (OnAir: a blank cell).
#pragma once
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace cp16 {

constexpr int kCarriers = 16;
constexpr int kRows = 16;               // dot rows of a character
constexpr int kCellRows = kRows + 1;    // with the blank separator row
constexpr double kMinSpacingHz = 5, kMaxSpacingHz = 30;      // what OnAir accepts (CRAC: 10 .. 20; CP16+ 18.75 / 25)
constexpr double kMinRowSec = 0.025, kMaxRowSec = 0.5;        // pixel period accepted (CRAC 1 .. 4 s a character: 59 .. 235 ms)

struct Params {
    double baseHz = 1000;      // audio frequency of column 0 (the left column), Hz
    double spacingHz = 15;     // carrier spacing, Hz
    double charSec = 2.0;      // seconds a character including its blank row (row = charSec / 17)
    bool lsb = false;          // lower sideband: in the complex baseband the carriers sit below the suppressed carrier, at -(baseHz + i x spacing)
    double rowSec() const { return charSec / kCellRows; }
    double audioHz(int col) const { return baseHz + spacingHz * col; }
    double carrierHz(int col) const { return lsb ? -audioHz(col) : audioHz(col); }   // offset from the suppressed carrier in the complex baseband
    double bandwidthHz() const { return spacingHz * (kCarriers - 1) + 5.0 / rowSec(); }   // CRAC: spread + 5 x the keying rate
};
bool validParams(const Params& p, std::string* why = nullptr);

// Speeds and spacings offered in the interface (the CRAC ranges); index 0 is the default
constexpr double kCharSecChoices[] = {2.0, 1.0, 1.5, 3.0, 4.0};
constexpr double kSpacingChoices[] = {15, 10, 11, 12, 17, 19, 20};
constexpr int kNumCharSec = 5, kNumSpacing = 7;

// One character cell: row 0 is the top, bit 15 of a row the left column (column 0), as the bytes of an HZK16 font
struct Glyph {
    uint16_t row[kRows] = {};
    bool empty() const { for (uint16_t r : row) if (r) return false; return true; }
    int ink() const;
};

// ---------------------------------------------------------------- fonts
// The glyphs come from (first that has the character): the font the user chose (an HZK16 file, raw GB2312 16 x 16, or a .ttf / .otf), the font data the
// program handed over (the desktop app: its bundled Noto Sans CJK SC subset, SIL OFL 1.1), and the built-in Latin glyphs of the Hellschreiber mode
// (ASCII, accented Latin, Cyrillic, Greek) drawn in the middle of the cell. Thread-safe.
bool loadFontFile(const std::string& path, std::string& err);   // empty path: forget the user's font
std::string fontFilePath();
bool setDefaultFontData(std::vector<uint8_t> ttf, const std::string& name, std::string& err);   // TrueType / OpenType bytes
std::string fontName();                                         // what draws the CJK characters now ("" none: only the built-in Latin glyphs)
bool glyphFor(uint32_t cp, Glyph& g);                           // false: no font has it (g is then a box)
uint32_t fullWidth(uint32_t cp);                                // ASCII 0x21 .. 0x7E -> U+FF01 .. U+FF5E, space -> U+3000, others unchanged
std::vector<uint32_t> utf8ToCodepoints(const std::string& s);
std::string codepointToUtf8(uint32_t cp);
const std::vector<uint32_t>& gb2312Repertoire();                // every GB2312 character (7445) as Unicode, the recognition set
uint32_t gb2312Code(uint32_t cp);                               // EUC-CN bytes (lead << 8 | trail) or 0

// The cells a text is sent as: full-width forms, a space a blank cell, line breaks and controls dropped
std::vector<uint32_t> textCells(const std::string& utf8);

// ---------------------------------------------------------------- transmitter
// The rows of a message: kCellRows per character (16 glyph rows and the blank row), each a 16-bit column mask (bit 15 = column 0)
std::vector<uint16_t> messageRows(const std::vector<uint32_t>& cells);

// The signal of a run of rows as complex baseband at `rate`: the carriers at p.carrierHz (upper sideband: the analytic signal of the audio; lower
// sideband: its mirror). amp: each carrier's amplitude. Every carrier's phase runs on from row to row (no steps).
class Modulator {
public:
    Modulator(const Params& p, double rate, double amp, uint32_t seed = 1);
    void push(uint16_t row);                       // queue a row
    size_t pending() const { return rows_.size() - next_; }
    // Up to n samples; returns how many were made (fewer only when the rows ran out)
    size_t generate(std::complex<float>* out, size_t n);
    double samplesPerRow() const { return spr_; }
private:
    Params p_;
    double rate_, amp_, spr_;
    std::vector<uint16_t> rows_;
    size_t next_ = 0;
    double pos_ = 0;                                // samples into the current row
    double ph_[kCarriers] = {}, dph_[kCarriers] = {};
    uint16_t prev_ = 0;
};

// ---------------------------------------------------------------- recognition
// The best character for 16 received rows (soft dots 0 .. 1): normalised correlation with the font's glyph of every candidate. Candidates: the GB2312
// set and the full-width ASCII (or `only` when not empty). score 0 .. 1; second: the runner-up's score.
struct Match { uint32_t cp = 0; float score = 0, second = 0; };
Match recognise(const float dots[kRows][kCarriers], const std::vector<uint32_t>* only = nullptr);
void clearGlyphCache();                                         // after the font changed

} // namespace cp16
} // namespace dect2
