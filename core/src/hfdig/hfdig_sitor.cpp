// SITOR teleprinter decoder (see hfdig_sitor.h).
#include "dect2/hfdig/hfdig_sitor.h"
#include "dect2/marine/marine_navtex.h"
#include <array>

namespace dect2 {
namespace {

// ITA2 letters and figures shift tables (ITU-T F.1), index = 5-bit code. 31 = LTRS, 27 = FIGS.
constexpr char kLetters[32] = {0,   'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
                               'T', 'Z',  'L',  'W', 'H', 'Y', 'P', 'Q', 'O',  'B', 'G', 0,   'M', 'X', 'V', 0};
constexpr char kFigures[32] = {0,   '3', '\n', '-', ' ', '\'', '8', '7', '\r', '$', '4', 0,   ',', '!', ':', '(',
                               '5', '"', ')',  '2', '#', '6',  '0', '1', '9',  '?', '&', 0,   '.', '/', ';', 0};
constexpr int kLtrs = 31;
constexpr int kFigs = 27;

[[maybe_unused]] constexpr int popcount7(unsigned w) {
    int n = 0;
    for (unsigned b = 0; b < 7; b++) n += (w >> b) & 1;
    return n;
}

// ITU-R M.476 words: the word of an ITA2 position is the CCIR 476 word of its letter (the table of marine_navtex.cpp, taken from
// fldigi); the shifts and the null have words of their own.
struct FecBTables {
    std::array<uint8_t, 32> encode{};
    std::array<int8_t, 128> decode{};
};
FecBTables buildFecB() {
    FecBTables t{};
    for (auto& d : t.decode) d = -1;
    for (int c = 0; c < 32; c++) {
        uint8_t w = 0;
        if (c == kLtrs) w = marine::kLtrs;
        else if (c == kFigs) w = marine::kFigs;
        else if (kLetters[c] == 0) w = marine::kAlpha;
        else if (!marine::ccir476Code(kLetters[c], false, w)) continue;
        t.encode[c] = w;
        t.decode[w] = static_cast<int8_t>(c);
    }
    return t;
}
const FecBTables& fecB() { static const FecBTables t = buildFecB(); return t; }
}  // namespace

char sitorIta2Char(uint8_t code5, bool figures) {
    if (code5 > 31) return 0;
    return figures ? kFigures[code5] : kLetters[code5];
}

int sitorFecBDecode(uint8_t word7) {
    if (word7 > 127) return -1;
    return fecB().decode[word7];
}

uint8_t sitorFecBEncode(uint8_t code5) { return fecB().encode[code5 & 31]; }

SitorDecoder::SitorDecoder(Mode mode, int lag) : mode_(mode), lag_(lag < 1 ? 1 : lag) { history_.assign(lag_, -1); }

void SitorDecoder::setMode(Mode mode) {
    mode_ = mode;
    clear();
}

void SitorDecoder::clear() {
    history_.assign(lag_, -1);
    pushes_ = 0;
    figures_ = false;
    text_.clear();
    chars_ = 0;
    errors_ = 0;
}

void SitorDecoder::emit(uint8_t code5) {
    if (code5 == kLtrs) { figures_ = false; return; }
    if (code5 == kFigs) { figures_ = true; return; }
    char c = sitorIta2Char(code5, figures_);
    if (!c) return;
    text_.push_back(c);
    chars_++;
}

void SitorDecoder::push(uint8_t word7) {
    int code = sitorFecBDecode(word7);
    if (code < 0 && (word7 == marine::kAlpha || word7 == marine::kBeta || word7 == marine::kRep || word7 == marine::kSpare)) {
        if (mode_ == Mode::FecA) { history_[static_cast<size_t>(pushes_++ % static_cast<uint64_t>(lag_))] = -1; }
        return;   // idle and repeat words are valid, they print nothing
    }
    if (mode_ == Mode::FecB) {
        if (code < 0) { errors_++; return; }
        emit(static_cast<uint8_t>(code));
        return;
    }
    // FEC-A: accept when this valid word equals the one `lag` words back; then the slot is used up.
    size_t slot = static_cast<size_t>(pushes_++ % static_cast<uint64_t>(lag_));
    int back = history_[slot];
    history_[slot] = code;
    if (code < 0) { errors_++; return; }
    if (back == code) {
        emit(static_cast<uint8_t>(code));
        history_[slot] = -1;
    }
}

}  // namespace dect2
