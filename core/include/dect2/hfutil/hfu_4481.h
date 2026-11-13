// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// STANAG 4481 naval HF broadcast, FSK part: receiver and test generator. From the text of STANAG 4481 Annex C: single-channel RATT with a
// +-425 Hz shift about the assigned frequency (the space tone the higher, the mark tone on a frequency ending in 75 Hz), and multi-channel
// RATT on up to 16 sub-carriers at (255 + 170 n) Hz, n = 1 .. 16, each shifted +-42.5 Hz (channels n and n + 8 may be twinned for
// frequency diversity). The PSK part of STANAG 4481 is the STANAG 4285 waveform, which OnAir reads in its own mode.
// Per channel: the tones are found, the speed is measured (45.45, 50, 75 or 100 Bd), and the bits are tried as ITA2 start-stop
// characters (1 start, 5 data, 1.5 stop). Plain-text broadcasts are shown as text; a stream that is not start-stop (an encrypted,
// synchronous broadcast) is reported with its speed and bit count only. Nothing is decrypted. Receive only.
// Input: complex baseband at 24 kHz, audio tones at positive frequencies (USB).
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::hfu {

inline double s4481ChannelHz(int n) { return 255.0 + 170.0 * n; }   // n = 1 .. 16

struct S4481Channel {
    int n = 0;                   // 1 .. 16 for the multi-channel sub-carriers, 0 for the single channel
    double markHz = 0, spaceHz = 0;
    bool active = false;
    double baud = 0;             // measured
    bool startStop = false;      // the bits frame as ITA2 start-stop characters
    uint64_t chars = 0, framingErrors = 0, bits = 0;
    double snrDb = 0;
    std::string text;            // the last characters
};

struct S4481Status {
    int mode = 0;                // 0 nothing found, 1 single channel (+-425 Hz), 2 multi-channel (+-42.5 Hz sub-carriers)
    std::vector<S4481Channel> channels;   // the active ones
};

class Stanag4481Rx {
public:
    Stanag4481Rx();
    ~Stanag4481Rx();
    void reset();
    void push(const cf32* x, size_t n);
    void setTextSink(std::function<void(int channel, const std::string&)> cb);   // new characters of a channel (0 single, 1 .. 16)
    void setEventSink(std::function<void(const std::string&)> cb);
    S4481Status status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct S4481GenCfg {
    bool multi = false;
    double singleMarkHz = 1275;  // single channel: the mark tone (ends in 75 Hz), the space 850 Hz above it
    double baud = 75;
    double leadSec = 0.3, tailSec = 0.3;
};
// one channel's content: ITA2 text (start-stop), or random synchronous bits when text is empty (an encrypted stream, for tests)
struct S4481Content { int n = 0; std::string text; int syncBits = 0; unsigned seed = 1; };
std::vector<cf32> genStanag4481(const S4481GenCfg& c, const std::vector<S4481Content>& channels, double seconds);

}  // namespace dect2::hfu
