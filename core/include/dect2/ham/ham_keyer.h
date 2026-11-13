// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham CW keyer: paddles or a straight key to key-down / key-up edges and to sidetone / transmit audio.
// Modes (the usual definitions of the Curtis 8044 keyer chip and its successors):
//   Straight    the key is the key-down
//   Iambic A    each paddle makes its element while held, both squeezed alternate; dit and dah memories: an opposite paddle pressed
//               during an element is sent after it; releasing both stops after the element under way
//   Iambic B    as A, and a squeeze that is released during an element adds one more opposite element (the "squeeze bonus")
//   Ultimatic   both held: the paddle pressed last repeats
//   Bug         dits automatic from the dit paddle, the dah paddle keys straight (a semi-automatic "bug")
// Timing: dot = 1200 / wpm ms, dash = 3 dots, gaps 1 dot inside the character; weight shifts the mark / space ratio (50 = 1 : 1).
// Pure logic with a caller clock (milliseconds): nothing here transmits; the audio goes through the same gate as the keyboard modes.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace dect2::ham {

enum class KeyerMode { Straight, IambicA, IambicB, Ultimatic, Bug };
const char* keyerModeName(KeyerMode m);

struct KeyerCfg {
    KeyerMode mode = KeyerMode::IambicB;
    double wpm = 20;              // 5 .. 60
    double weight = 50;           // 25 .. 75: the mark's share of a mark + space pair, %
    bool swap = false;            // left / right paddle swapped
    double dashRatio = 3;         // 2.5 .. 4.5
};

struct KeyEdge { double ms; bool down; };

class CwKeyer {
public:
    explicit CwKeyer(const KeyerCfg& c = KeyerCfg()) : c_(c) {}
    KeyerCfg& config() { return c_; }
    // The inputs, at the time they change (nondecreasing ms). dit / dah are the paddle contacts (before the swap).
    void paddles(double ms, bool dit, bool dah);
    void straight(double ms, bool down);
    // Moves the keyer's clock on to ms (call it often, e.g. every 1 ms or at each audio block); key edges up to ms are made.
    void run(double ms);
    bool keyDown() const { return key_; }
    std::vector<KeyEdge> takeEdges();     // the edges since the last call
    void reset();
    double dotMs() const;
private:
    enum El { None, Dit, Dah };
    void edge(double ms, bool down);
    void startElement(double ms, El e);
    El nextElement();
    KeyerCfg c_;
    bool dit_ = false, dah_ = false, sk_ = false;      // paddles (after the swap), straight key
    bool ditMem_ = false, dahMem_ = false, squeezeSeen_ = false;
    El cur_ = None, last_ = None, lastPressed_ = None;
    double elEnd_ = 0, spEnd_ = 0;                     // the element's mark end and its following gap's end
    bool inEl_ = false;
    double now_ = 0;
    bool key_ = false;
    std::vector<KeyEdge> edges_;
};

// The edges as keyed audio (raised-cosine edges of riseMs, no clicks) at `rate`, from edges[0].ms (minus 10 ms) to the last edge + 50 ms.
std::vector<float> keyerAudio(const std::vector<KeyEdge>& edges, double hz, double rate, double riseMs = 5);
// The text as the keyer would send it (each character's elements on the paddles, a word gap for spaces): the test and the "send memory" path.
std::vector<KeyEdge> keyerText(const std::string& text, const KeyerCfg& c, double startMs = 0);
// The edges back to text with the repo's CW timing decoder (cw::Timing); wpmOut: the speed it read.
std::string keyerDecode(const std::vector<KeyEdge>& edges, double* wpmOut = nullptr);

}  // namespace dect2::ham
