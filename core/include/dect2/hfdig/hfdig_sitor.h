// SITOR teleprinter decoder (ITU-R M.476, clean-room from the public description): 7-unit words of the CCIR 476 family carrying
// ITA2 (ITU-T F.1) characters. This header and hfdig_sitor.cpp take already-demodulated 7-element words (bit 6 first, mark = 1);
// the FSK front end is not part of this module.
//   FEC-B: each word is one character, valid only with exactly four marks; a word that is not valid is counted as an error.
//   FEC-A: time diversity. A word is accepted only when the same valid word arrives again `lag` words later; a single corrupted
//          copy is dropped instead of printed.
// The word table is the ITU-R M.476 / M.625 one (shared with NAVTEX, marine_navtex.cpp); the idle and repeat words print nothing.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// ITA2 5-bit value to a character for the letters or figures shift. Returns 0 for shift codes and blanks that print nothing.
char sitorIta2Char(uint8_t code5, bool figures);

// FEC-B word table: word -> ITA2 5-bit code (-1 when the word is not a valid four-of-seven word) and the reverse.
int sitorFecBDecode(uint8_t word7);
uint8_t sitorFecBEncode(uint8_t code5);

// Streaming decoder. Feed one 7-element word per character period; text() returns what has been printed so far.
class SitorDecoder {
public:
    enum class Mode { FecA, FecB };
    explicit SitorDecoder(Mode mode = Mode::FecB, int lag = 3);
    void setMode(Mode mode);
    void push(uint8_t word7);
    void clear();
    const std::string& text() const { return text_; }
    uint32_t chars() const { return chars_; }
    uint32_t errors() const { return errors_; }

private:
    void emit(uint8_t code5);
    Mode mode_;
    int lag_;
    std::vector<int> history_;   // FEC-A: the last `lag` words (-1 for an invalid word)
    uint64_t pushes_ = 0;        // words received since clear()
    bool figures_ = false;
    std::string text_;
    uint32_t chars_ = 0;
    uint32_t errors_ = 0;
};

}  // namespace dect2
