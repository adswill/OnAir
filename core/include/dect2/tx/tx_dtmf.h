// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DTMF keypad for the analog voice transmitter (NFM, AM, USB / LSB, airband): the keys 0-9, *, #, A-D as the dual tones of ITU-T Q.23
// (rows 697 / 770 / 852 / 941 Hz, columns 1209 / 1336 / 1477 / 1633 Hz, the table of tones_proto.h). It is for EchoLink / IRLP node control
// and repeater commands over the air. The panel pushes keys into TxDtmfQueue::global() while a voice transmission is on; the voice generator
// (tx_voice.cpp) takes them one at a time and sends each as a tone of toneMs followed by a gap of gapMs. While a key or its gap plays the
// microphone is muted, as a radio does it; the VOX gate is held open. Nothing here keys a transmitter by itself.
#pragma once
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct TxDtmfTiming {
    double toneMs = 100;     // each key's tone (Q.24 asks for at least 40 ms; EchoLink and IRLP nodes decode 60 ms and up reliably)
    double gapMs = 100;      // silence after each key
    double levelPct = 35;    // each of the two tones, in percent of full audio (1.0 = the mode's full deviation or modulation)
    double twistDb = 0;      // the column (high) tone this many dB above the row tone (negative: below)
};

// The keys DTMF can send: 0-9 * # A B C D (lower-case a-d are accepted and sent as upper case)
bool txDtmfValidKey(char c);
// The row and column frequency of a key (false for a key that is not DTMF)
bool txDtmfFreqs(char c, double& rowHz, double& colHz);
// keys with everything that is not a DTMF key dropped, upper-cased
std::string txDtmfClean(const std::string& keys);

// The keys waiting to be sent. Thread safe: the panel pushes, the transmitter's generator thread pops.
class TxDtmfQueue {
public:
    static TxDtmfQueue& global();
    // Adds the valid keys of `keys` (others are ignored); returns how many were added. At most 256 keys wait at a time.
    int push(const std::string& keys);
    void clear();
    size_t pending() const { return n_.load(); }
    void setTiming(const TxDtmfTiming& t);
    TxDtmfTiming timing() const;
    // The next key with the timing in force when it was taken; false when nothing waits
    bool pop(char& key, TxDtmfTiming& t);
    // The keys sent so far in this transmission (cleared by clear()), for the panel
    std::string sent() const;

private:
    mutable std::mutex mu_;
    std::deque<char> q_;
    std::atomic<size_t> n_{0};
    TxDtmfTiming t_;
    std::string sent_;
};

// Makes the tones at an audio rate from a queue: next() is one sample. active() is true while a key or its gap plays.
class TxDtmfSequencer {
public:
    TxDtmfSequencer(double fs, TxDtmfQueue* q) : fs_(fs), q_(q) {}
    bool active() const { return left_ > 0; }
    bool wants() const { return left_ > 0 || (q_ && q_->pending() > 0); }   // a key plays or waits
    float next();

private:
    void start(char key, const TxDtmfTiming& t);
    double fs_;
    TxDtmfQueue* q_;
    long left_ = 0, toneLeft_ = 0, ramp_ = 1, pos_ = 0, toneLen_ = 0;
    double w1_ = 0, w2_ = 0, a1_ = 0, a2_ = 0, p1_ = 0, p2_ = 0;
};

// The audio of a key string at rate fs (tones and gaps, starting with a tone), for tests and for a prepared file
std::vector<float> txDtmfAudio(const std::string& keys, double fs, const TxDtmfTiming& t);

}  // namespace dect2
