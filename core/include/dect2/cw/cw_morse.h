// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW (Morse): the code table (ITU-R M.1677-1 letters, figures and punctuation, and the usual prosigns), the callsign pattern, and the
// timing decoder that turns key-down / key-up durations into text while it follows the speed.
//
// Timing: the marks of the last characters are split into two clusters (dots and dashes, a 2-means on their lengths); the dot length
// gives the speed (WPM = 1200 / dot in ms, 5 to 50 WPM). A mark shorter than the midpoint of the clusters is a dot. A gap shorter than
// 2 dots is inside a character, from 2 to 5 dots ends the character, longer is a word space: only the marks and the gaps inside a
// character set the speed, so Farnsworth spacing (characters at full speed, longer gaps) reads as it is sent. Spikes shorter than a third
// of a dot (noise, a click) and drop-outs as short inside a mark (fading, a contact bounce) are merged away before a character is read.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace cw {

// ".-" -> "A"; prosigns as "<AR>", "<SK>", "<BT>", "<KN>", "<AS>", "<HH>", "<SOS>"; nullptr when not in the table
const char* decodeSymbol(const std::string& code);
// the code of one character or prosign token ("A", "<AR>"); "" when not in the table
std::string encodeSymbol(const std::string& tok);
// a text cut into tokens: characters, "<XX>" prosigns, and " " for a word space (lower case is taken as upper case)
std::vector<std::string> tokenize(const std::string& text);

// a word that looks like an amateur callsign (prefix, digit, suffix, an optional "/P" style part): LY2ABC, K1ABC, 9A1AA, DL1XYZ/P
bool isCallsign(const std::string& word);
std::vector<std::string> callsigns(const std::string& text);   // the callsigns in a text, first seen first, each once

class Timing {
public:
    void reset();
    void mark(double ms);                // a key-down that just ended
    void space(double ms);               // a key-up that just ended
    void idle(double ms);                // the key is up and has been for ms: ends the character and the word in time
    std::string take();                  // the text decoded since the last call
    bool locked() const { return dot_ > 0; }
    double wpm() const { return dot_ > 0 ? 1200.0 / dot_ : 0; }
    double dotMs() const { return dot_; }
    double dashRatio() const { return dot_ > 0 ? dash_ / dot_ : 0; }
    double charGapDots() const { return dot_ > 0 ? charGap_ / dot_ : 0; }   // the gap between characters, in dots (3 normal, more: Farnsworth)
    bool farnsworth() const { return charGap_ > 0 && charGapDots() > 4.2; }
    double wordThresholdMs() const;      // a key-up at least this long is a word space
    uint64_t chars() const { return chars_; }
    uint64_t unknown() const { return unknown_; }
private:
    struct Ev { bool on; double ms; };
    void push(bool on, double ms);
    void endChar();
    void wordSpace();
    void estimate();
    void replay();
    void resync();
    std::vector<Ev> cur_;                // the elements of the character being received
    std::vector<Ev> backlog_;            // before the speed is known
    bool replaying_ = false;             // inside replay() or resync(): they feed push() again, and must not start another one (that recursion
                                         // ran a Windows thread's stack out on a noisy band)
    std::vector<double> marks_;          // the last marks (for the clusters)
    double dot_ = 0, dash_ = 0;
    std::vector<double> gaps_;           // the last key-ups that ended a character or a word
    double charGap_ = 0;                 // the gap between characters (0: not known)
    void gapClusters();
    bool spaced_ = true;                 // a word space was written (or nothing yet)
    bool charOpen_ = false;
    std::string out_;
    uint64_t chars_ = 0, unknown_ = 0;
};

// Between the envelope detector and the timing decoder: the key state once a millisecond, debounced (a change counts once it has lasted
// a third of a dot, at least 3 ms), so noise spikes in a gap and drop-outs in a mark never reach the timing decoder.
class Keyer {
public:
    void reset() { key_ = false; run_ = 0; cnt_ = 0; started_ = false; }
    void sample(bool raw, Timing& tm);   // one millisecond
    bool key() const { return key_; }
private:
    bool key_ = false, started_ = false;
    int run_ = 0, cnt_ = 0;
};

} // namespace cw
} // namespace dect2
