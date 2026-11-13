// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF ALE, second generation (MIL-STD-188-141 appendix A, the same as FED-STD-1045): the word layer and the call layer.
//
// Signal: 8-FSK on upper sideband, 125 baud (8 ms symbols), tones 750 .. 2500 Hz in 250 Hz steps, three bits a tone (Gray coded:
// 750 Hz 000, 1000 001, 1250 011, 1500 010, 1750 110, 2000 111, 2250 101, 2500 100), 375 bit/s.
// Word: 24 bits, a 3-bit preamble (the word type) and three 7-bit characters. Its two 12-bit halves A and B each become an extended
// Golay (24,12) code word (B sent inverted); the two are interleaved A1 B1 A2 B2 .. A24 B24 and a stuff bit (0) closes 49 bits; the
// 49 bits are sent three times (147 bits, 49 tones, 392 ms: Tw) and the receiver takes a majority vote of each bit. The count of
// votes that were not unanimous (0 .. 49) is the word's BER figure, as in the LQA.
//
// Calls (the assembler): a TO address (repeated through the scanning part), an optional message part (CMD words: an AMD text in
// ASCII-64, an LQA report, other commands), a conclusion TIS (answer me) or TWS (no answer). Addresses longer than three characters
// go on in DATA and REP words (alternating), padded with '@'; THRU words list the members of a group call; a sounding is the
// conclusion alone, repeated. Addresses use the 38-character set A-Z 0-9 @ ?; "@?@" is the allcall, "@@?" the anycall.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace ale {

constexpr int kBaud = 125;
constexpr double kTone0 = 750, kToneStep = 250;
constexpr int kWordBits = 49;          // after the vote
constexpr int kWordSymbols = 49;       // 147 bits / 3
constexpr double kTw = 0.392;          // one word, s

enum WordType { Data = 0, Thru = 1, To = 2, Tws = 3, From = 4, Tis = 5, Cmd = 6, Rep = 7 };
const char* typeName(int t);           // "DATA", "THRU", "TO", "TWS", "FROM", "TIS", "CMD", "REP"

int tone(int tribit);                  // Gray code: tribit -> tone index 0 .. 7
int tribit(int tone);

// the extended Golay (24,12) code (generator x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1, then an overall parity bit)
uint32_t golayEncode(uint32_t data12);                 // data in the top 12 bits of the 24
int golayDecode(uint32_t rx24, uint32_t& data12);      // the bits corrected (0 .. 3), or -1 when more than 3 are wrong

bool basicChar(char c);                // the 38 address characters: A-Z 0-9 @ ?
bool asciiChar64(char c);              // ASCII-64: 0x20 .. 0x5F (AMD text)

uint32_t makeWord(int type, char c1, char c2, char c3);   // 24 bits
// the 147 bits of a word as sent, MSB first: the 49 interleaved bits, three times
void wordBits(uint32_t word24, uint8_t bits147[147]);
void bitsToTones(const uint8_t* bits, int nbits, std::vector<int>& tones);   // 3 bits a tone, MSB first

struct Word {
    bool ok = false;
    uint32_t w = 0;                    // 24 bits
    int type = 0;
    char c[3] = {0, 0, 0};
    int voteErrs = 0;                  // bits whose three copies were not unanimous (0 .. 49)
    int golayErrs = 0;                 // bits Golay corrected in A and B
    bool stuffOk = true;               // the 49th bit was 0
    std::string text() const { return std::string(c, c + 3); }
};
// the vote, deinterleaving and both Golay words; ok when both decode
Word decodeBits(const uint8_t bits147[147]);

// the words of an address: the first word of type `first` ("TO", "TIS" ...), then DATA, REP, DATA ... padded with '@'
std::vector<uint32_t> addressWords(int first, const std::string& addr);
// AMD text (ASCII-64, padded with spaces) in CMD, DATA, REP ... words
std::vector<uint32_t> amdWords(const std::string& text);
// LQA report as a CMD word: 'a', then BER (5 bits, 0 .. 30, 31 none), SINAD (5 bits, dB 0 .. 30, 31 none), multipath (4 bits)
uint32_t lqaWord(int ber, int sinadDb, int mp);

std::string trimAddress(const std::string& a);   // the '@' padding off (not of "@?@" or "@@?")

// One transmission put together from its words
struct Call {
    std::vector<std::string> to, thru;   // called addresses (TO), group members (THRU)
    std::string from;                    // the conclusion's address (TIS / TWS), else FROM
    int conclusion = -1;                 // Tis, Tws or -1 none
    std::string amd;                     // AMD text
    std::vector<std::string> cmds;       // other commands, as text ("LQA BER 3 SINAD 21 MP 0", "CMD c 0x1234")
    bool lqa = false;
    int lqaBer = -1, lqaSinad = -1, lqaMp = -1;
    int words = 0;
    enum Kind { Sounding, Individual, Group, AllCall, AnyCall, Unknown };
    Kind kind = Unknown;
};

// Words to calls: feed the words of one transmission in order, then finish() when the signal is gone
class Assembler {
public:
    void add(const Word& w);
    bool empty() const { return words_ == 0; }
    Call finish();                       // and starts over
private:
    void close();
    struct Field { int type; std::string text; };
    std::vector<Field> fields_;
    bool open_ = false;
    Field cur_{0, ""};
    int words_ = 0;
};

const char* kindName(int k);   // "Sounding", "Individual", "Group", "AllCall", "AnyCall", "Unknown"

} // namespace ale
} // namespace dect2
