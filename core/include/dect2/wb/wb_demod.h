// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: demodulation of a chosen band of a recording and recovery of its bit stream.
//
// Chain: band extraction (wb_dsp.h) to a baseband of at least 8 samples per symbol -> demodulator:
//   AM envelope, FM / NFM discriminator, USB / LSB / CW product detector (all give audio at 48 kHz),
//   ASK / OOK envelope + threshold, 2-FSK / 4-FSK / MSK / GMSK discriminator, AFSK two-tone correlators (Bell 202 and others),
//   BPSK / QPSK / 8PSK with a frequency estimate from the x^M line, a matched filter (RRC or boxcar), Gardner timing and an M-th power PLL
// -> symbol timing (best phase, then an optional Gardner loop) -> decisions -> bits (before the line-code transforms of wb_bits.h).
// It also keeps the plots: instantaneous frequency / phase / magnitude, the eye diagram, the constellation, the soft symbols.
#pragma once
#include "wb_bits.h"
#include "wb_dsp.h"
#include <string>
#include <vector>

namespace dect2 {
namespace wb {

enum Kind { KAm = 0, KFm, KUsb, KLsb, KCw, KAsk, KFsk2, KFsk4, KAfsk, KBpsk, KQpsk, KPsk8, KMsk, KGmsk, KindCount };
const char* kindName(int k);
bool kindHasBits(int k);     // the kinds that recover a bit stream
bool kindIsPsk(int k);

struct DemodParams {
    int kind = KFm;
    double baud = 1200;
    double bwHz = 0;               // the band, centred on centreHz; 0: whatever the extractor was given (the full signal)
    double centreHz = 0;           // from the recording's DC
    bool carrierRecovery = true;   // PSK: frequency estimate and the phase-locked loop; off: the symbols turn
    int clock = 1;                 // 0 best sampling phase only, 1 + Gardner timing loop
    double loopBw = 0.02;          // the loops' bandwidth, as a fraction of the symbol rate
    double rolloff = 0.35;         // PSK matched filter: root raised cosine; 0 = a boxcar over one symbol
    bool differential = false;     // PSK: the symbols are differentially encoded (the phase step is the data); also removes the phase ambiguity
    double markHz = 1200, spaceHz = 2200;   // AFSK tones (from the signal's DC; for a carrier: after FM demodulation)
    bool fmFirst = false;          // AFSK on a carrier: FM-demodulate first, the tones are in the audio
    double cwPitchHz = 600;
    double fskInvert = false;      // swap which side is 1
    int audioRate = 48000;
    size_t maxSamples = 1u << 24;  // longest baseband handled
};

struct DemodResult {
    bool ok = false;
    std::string msg;
    double rate = 0;                    // baseband sample rate
    double baud = 0;                    // symbol rate used (the recovered one when the loop moved)
    double sps = 0;
    double cfoHz = 0;                   // PSK: the frequency offset removed
    double snrDb = 0;                   // from the symbols (EVM / eye)
    std::vector<float> audio;           // demodulated audio at audioRate (peak 0.9), AM / FM / SSB / CW and the soft AFSK / FSK signals
    std::vector<float> instFreq;        // Hz, at `plotRate`
    std::vector<float> instPhase;       // radians, unwrapped
    std::vector<float> magnitude;
    double plotRate = 0;                // samples per second of the three plots above
    std::vector<float> eye;             // eyeTraces x eyeLen, normalised to +-1
    int eyeTraces = 0, eyeLen = 0;
    std::vector<cf32> constellation;    // the symbol decisions' input points, normalised to unit radius
    std::vector<float> soft;            // the soft value per symbol (FSK / ASK / AFSK)
    std::vector<float> levels;          // FSK: the decision levels found (2 or 4 centres), in the soft units
    Bits bits;                          // as demodulated: before the line-code transforms
    int bitsPerSymbol = 1;
    size_t symbols = 0;
    double devHz = 0;                   // FSK: the deviation measured
    uint64_t startSample = 0;           // where in the recording the data starts
};

// A baseband signal already at its band (the recording's DC is the band's centre): for tests and for callers that extracted the band
DemodResult demodulateBaseband(const cf32* x, size_t n, double rate, const DemodParams& p);
// From the recording: the time range [t0, t1) of the source, the band round p.centreHz of width p.bwHz
DemodResult demodulate(const Source& s, uint64_t t0, uint64_t t1, const DemodParams& p);

// A symbol-rate guess from the baseband (see wb_measure.h: estimateSymbolRate); here: the standard rates near it
double snapBaud(double baud);

} // namespace wb
} // namespace dect2
