// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M17 test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of transmissions, each a full M17 burst: preamble, LSF, stream or packet frames, end-of-transmission marker; 4FSK at 4800
// symbols/s, root-raised-cosine (alpha 0.5) shaped, +-2.4 / +-0.8 kHz deviation, over complex white noise. Stream voice is a synthetic
// voice coded with the user's libcodec2 when it is installed, otherwise a known byte pattern (m17KnownPayload).
//
// SynthConfig (makeM17Synth):
//   snrDb       carrier to noise in 9 kHz
//   cfoHz       the radio's tuning error
//   modeOpt[0]  0 the test schedule (m17TestSchedule), 1 only its voice stream, 2 only its packet SMS
#pragma once
#include "dect2/engine/mode_synth.h"
#include "m17_proto.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct M17GenTx {
    bool packet = false;
    std::string src = "N0CALL", dst = "@ALL";
    int can = 0;
    int dataType = 2;            // stream: 1 data, 2 voice 3200, 3 voice + data 1600
    int enc = 0, encSub = 0;     // encryption type and subtype as flagged in TYPE (the payload is then random bytes)
    int meta = 0;                // without encryption: 0 text, 1 GNSS, 2 extended callsign
    std::string text;            // META text (meta 0), or the SMS (packet)
    m17::Gnss gnss;              // meta 1
    std::string ext1, ext2;      // meta 2
    double durSec = 2;           // stream length (frames of 40 ms)
    double gapSec = 1;           // silence after it
    // 8 kHz PCM for Codec 2. Empty: the synthetic voice. Used when the library can encode.
    std::function<void(int16_t* pcm, int n)> pullPcm;
};

struct M17GenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the channel sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double snrDb = 30;           // carrier to noise in 9 kHz; >= 99: no noise
    double cfoHz = 0;            // the radio's tuning error (the signal moves by it)
    double devScale = 1;         // deviation relative to the specification (1.2 = 20 % too wide)
    double amp = 0.05;
    double leadSec = 0.3;        // silence before the first transmission
    bool loop = true;            // repeat the schedule
    bool voice = true;           // code the voice with libcodec2 when installed (false: always the known pattern)
    uint32_t seed = 1;
    std::vector<M17GenTx> txs;
};

class M17Generator {
public:
    explicit M17Generator(const M17GenConfig& c);
    ~M17Generator();
    void generate(cf32* out, size_t n);
    double timeSec() const;
    bool codedVoice() const;     // the voice frames are Codec 2 (else the known pattern)
    static double noiseSigma(const M17GenConfig& c);   // per component
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The symbols of one whole transmission (preamble to end marker). codec2: code the voice (when the library is there)
void m17TxSymbols(const M17GenTx& tx, bool codec2, std::vector<int8_t>& sym, uint32_t seed = 1);
// The payload bytes of stream frame fn when no Codec 2 is used
void m17KnownPayload(uint16_t fn, uint8_t out[16]);
std::vector<M17GenTx> m17TestSchedule();

std::unique_ptr<ModeSynth> makeM17Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
