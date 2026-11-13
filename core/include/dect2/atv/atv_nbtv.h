// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Narrow Bandwidth Television (NBTV) and mechanical television: a picture of a few dozen lines whose video fits in the audio band.
// The NBTV Association's 32-line standard (after Baird's 30-line service of the 1930s): 32 lines, 12.5 pictures a second (400 lines a second),
// scanned vertically (each line runs from the top of the picture to the bottom, the lines follow each other from left to right), aspect
// ratio 2:3 (width : height), a synchronising pulse below black at the start of every line. Generic mechanical television is the same with
// another line count (24 to 240) and picture rate, scanned vertically or horizontally.
//
// The video is a baseband audio-rate signal: sync = 0, black = 0.3, white = 1 (sound-card files carry it as -1 .. +1, usually AC coupled).
// Off air it is the audio of an AM or narrow FM broadcast or amateur transmission; the receiver demodulates it (AtvNbtvRadio) and the decoder
// (AtvNbtvDecoder) finds line and picture sync and makes the picture. The picture sync used here is the line sync pulse of the first line of each
// picture lengthened to three times its width (the NBTVA standard leaves the frame sync to the equipment; a decoder without one still runs and
// only loses the position of the first line).
#pragma once
#include "atv_tel.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct NbtvFormat {
    int lines = 32;               // lines per picture (24 .. 240)
    double frameHz = 12.5;        // pictures a second
    double aspect = 2.0 / 3;      // picture width / height
    bool vertical = true;         // lines run top to bottom (and follow each other left to right); false: left to right, top to bottom
    double syncFrac = 0.06;       // line sync pulse, share of a line
    double frameSyncMul = 3;      // the first line's sync pulse is this many times as long
    double porchFrac = 0.03;      // black after the sync pulse (the level reference), share of a line
    double endFrac = 0.01;        // black before the next sync pulse
    double blackLevel = 0.3;      // sync 0, white 1

    double lineHz() const { return lines * frameHz; }
    // samples along a line in the picture (the picture is `lines` wide and this high when vertical)
    int alongLine() const { const double a = vertical ? lines / aspect : lines * aspect; return a < 8 ? 8 : (int)(a + 0.5); }
    double activeFrac() const { return 1.0 - syncFrac - porchFrac - endFrac; }
    double activeStartFrac() const { return syncFrac + porchFrac; }
    double videoBwHz() const { return 0.5 * lineHz() * alongLine() / activeFrac(); }   // the highest frequency the pixels along a line need
    std::string name() const;     // "NBTV 32 lines 12.5/s" or "mechanical 60 lines 25/s"
    bool valid() const { return lines >= 24 && lines <= 240 && frameHz >= 1 && frameHz <= 60 && aspect > 0.1 && aspect < 10; }
};
NbtvFormat nbtvStandard();                                   // the NBTVA 32-line standard
NbtvFormat nbtvMechanical(int lines, double frameHz);         // the same scanning with another line count and rate

// The test picture: pattern 0: the top part is luminance bars across the lines (the luminances of the EBU colour bars, white to black), the
// bottom part a grey ramp along the lines (black at the top, white at the bottom of the picture); 1: bars only; 2: ramp only.
// x: 0..1 across the picture, y: 0..1 down it. Returns the luminance 0..1.
float nbtvTestPicture(int pattern, double x, double y);

// ---- the video as a sound signal
class AtvNbtvGen {
public:
    AtvNbtvGen(const NbtvFormat& f, double rate, int pattern = 0);
    const NbtvFormat& format() const { return f_; }
    // the next n samples of video (sync 0, black 0.3, white 1)
    void generate(float* out, size_t n);
    // the video level at time t seconds from the start (the generator is a function of time)
    float at(double t) const;
private:
    NbtvFormat f_;
    double rate_;
    int pattern_;
    uint64_t n_ = 0;
};

// ---- the video on a radio carrier, as IQ: AM (carrier 15 % at the sync tip, 100 % at white) or narrow FM (the sync tip at -dev, white at +dev)
struct NbtvRfConfig {
    NbtvFormat fmt;
    double rate = 1e6;
    bool fm = false;
    double devHz = 5000;          // FM: peak deviation
    double cfoHz = 0;
    double cnrDb = 40;            // carrier to noise in 2 * the video bandwidth; 100 and above: no noise
    double level = 0.4;
    int pattern = 0;
    uint64_t seed = 1;
};
class AtvNbtvRfGen {
public:
    explicit AtvNbtvRfGen(const NbtvRfConfig& c);
    ~AtvNbtvRfGen();
    void generate(cf32* out, size_t n);
    const NbtvRfConfig& config() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---- the decoder: audio-rate video in, pictures out
struct NbtvStatus {
    bool lineLocked = false, frameLocked = false;
    double lineHz = 0;            // measured
    double lineErrPpm = 0;        // against the format
    float syncQuality = 0;        // share of lines whose pulse came where it was expected
    uint64_t lines = 0, frames = 0;
    float snrDb = 0;              // the video against the noise on the sync tips
    std::vector<float> lineWave;  // one line, 256 points, normalised (sync 0, black 0.3, white 1)
};

class AtvNbtvDecoder {
public:
    AtvNbtvDecoder();
    ~AtvNbtvDecoder();
    void configure(double rate, const NbtvFormat& f);
    void reset();
    // any number of samples; the polarity is found by itself (sync pulses are the lowest level; an inverted signal is turned round)
    void process(const float* x, size_t n);
    std::function<void(std::shared_ptr<const AtvFrame>)> frameReady;
    std::function<void(const std::string&)> log;
    NbtvStatus status() const;
    const NbtvFormat& format() const;
    // the size of the pictures: `lines` x alongLine() samples (vertical scan: lines are columns), each scaled up by scale() in both directions
    int scale() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---- the radio side: AM or narrow FM from IQ (the transmitter near the centre of the input, within +-60 kHz), audio out at about 4 times the
// video bandwidth, then the decoder
class AtvNbtvRadio {
public:
    AtvNbtvRadio();
    ~AtvNbtvRadio();
    void configure(double fs, const NbtvFormat& f, bool fm, double devHz = 5000);
    void reset();
    void process(const cf32* x, size_t n);
    AtvNbtvDecoder& decoder();
    double audioRate() const;
    float carrierDbfs() const;     // AM: the carrier level; FM: the signal level
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
