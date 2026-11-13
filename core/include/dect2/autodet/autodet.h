// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Automatic standard detection for the one-click "TV (auto)" and "Digital voice (auto)" mode entries (docs/AUTODETECT.md).
//
// TV: from a stretch of samples that holds one TV channel it finds out which standard is on the air and the parameters the receiver needs:
//   spectrum (occupied width, flat top, the lines of an ATSC pilot or an analogue vision / sound carrier) -> OFDM cyclic-prefix
//   autocorrelation at the useful-symbol lengths of every OFDM standard (DVB-T / T2 FFT sizes at 1.7 / 5 / 6 / 7 / 8 MHz, ISDB-T modes 1-3,
//   ATSC 3.0 8K / 16K / 32K, the cyclic PN headers of DTMB) with the guard found by folding at the symbol period -> the DVB-T2 P1 symbol
//   (frequency-shifted repetition at 1024 T) tells T2 from T -> for a single carrier: the symbol rate from the cyclostationary line of |x|^2,
//   the roll-off, PSK or QAM from the fourth moment at the symbol instants, the DVB-S2 PL header (SOF) -> DVB-S / S2, DVB-C.
// Voice: every narrowband signal in the span is found, brought to baseband and demodulated into one soft level per symbol (the integrated
//   frequency, which is also the phase step of pi/4-DQPSK) at each candidate symbol rate (2400, 4800, 6000, 18000 Bd); then the frame syncs
//   of every system are looked for in parallel (DMR, P25 phase 1 and 2, NXDN48 / 96, dPMR, D-STAR, Yaesu System Fusion, M17, TETRA). A sync
//   is counted only with few symbol errors; short syncs only when they repeat at the frame period. No sync: analogue FM when the signal looks
//   like it, otherwise "unsure" with what was measured. Nothing is decoded here and nothing is decrypted.
//
// Both are pure functions of the samples (deterministic); Capture collects samples from the engine in any mode and runs them on a thread of
// its own with a progress text in plain words.
#pragma once
#include "dect2/common/ring.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace autodet {

// How sure: Sure (the decisive feature was found more than once), Likely, Unsure (several fit or the evidence is thin), None (no signal)
enum Verdict { VerdictNone = 0, VerdictUnsure, VerdictLikely, VerdictSure };
const char* verdictName(int v);   // "no signal", "unsure", "likely", "sure"

// ---------------------------------------------------------------------------------------------------------------------------- TV
enum TvStd { TvUnknown = 0, TvDvbt, TvDvbt2, TvDvbc, TvDvbs, TvIsdbt, TvAtsc, TvAtsc3, TvDtmb, TvAnalog, TvCount };
const char* tvStdName(int s);     // "DVB-T", "DVB-T2", ... ("DVB-S/S2" for TvDvbs: the receiver tells S from S2 by itself)
int tvEngineStd(int s);           // the engine standard that receives it (Engine::setStandard): 2, 1, 42, 8, 6, 3, 5, 9, 10; -1 none
int tvAppFamily(int s);           // the app's mode family (mode_table.cpp): 0 DVB, 1 ATSC, 3 ATSC 3.0, 4 ISDB-T, 6 DVB-S/S2, 7 DTMB, 8 analog TV, 40 DVB-C

struct TvParams {
    double centreHz = 0;          // the signal's centre, from the samples' centre
    double bwHz = 0;              // occupied width (half-way between floor and plateau)
    double snrDb = 0;             // plateau over floor
    double chanMhz = 0;           // channel raster width: 1.7, 5, 6, 7, 8 (OFDM standards, DTMB, ATSC, analogue)
    int fftK = 0;                 // OFDM: FFT size in K (1, 2, 4, 8, 16, 32); ISDB-T: 2 / 4 / 8 for modes 1 / 2 / 3; ATSC 3.0: 8 / 16 / 32
    double guard = 0;             // guard interval as a share of the useful symbol (0.25, 0.125, ...); 0 not found
    std::string guardName;        // "1/8", "19/256", "GI 1024 samples" ...
    double tuUs = 0;              // useful symbol length (OFDM) in microseconds
    double cpCorr = 0;            // cyclic-prefix correlation at that length, 0..1
    bool p1 = false;              // the DVB-T2 P1 symbol was found
    int p1Count = 0;
    int dtmbPn = 0;               // DTMB frame header 420 / 595 / 945
    double symRate = 0;           // single carrier: symbols per second
    double rolloff = 0;           // excess bandwidth (occupied width / symbol rate - 1)
    std::string constellation;    // "QPSK", "8PSK", "QAM", "16-QAM", "64/256-QAM", "" not measured
    double kurt = 0;              // fourth moment of the symbols over the square of the second, noise taken out (1 PSK, 1.32..1.39 QAM)
    int s2Sof = 0;                // DVB-S2 start-of-frame headers found
    bool atscPilot = false;
    std::string atvSystem;        // analogue: "B/G", "I", "D/K", "M/N" from the sound carrier spacing
};

struct TvCandidate { int std = TvUnknown; double score = 0; std::string why; };

struct TvResult {
    bool signal = false;          // something above the floor at all
    int std = TvUnknown;          // the answer (TvUnknown: unsure, see cands)
    double confidence = 0;        // 0..1
    int verdict = VerdictNone;
    TvParams p;
    std::vector<TvCandidate> cands;   // best first
    std::vector<std::string> evidence;
    std::string headline;         // "DVB-T2, 32K, GI 1/8, 8 MHz (sure)"
    double ms = 0;
};

struct TvOptions {
    double rfHz = 0;              // RF of the samples' centre (0 unknown): 950 - 2150 MHz and above 3 GHz count as a satellite IF
    bool satellite = false;       // the user said the input comes through an LNB
};

TvResult detectTv(const cf32* x, size_t n, double rateHz, const TvOptions& o = TvOptions());
std::string tvSummary(const TvResult& r);   // the headline as plain words, English

// ---------------------------------------------------------------------------------------------------------------------------- voice
enum VoiceSys { VoiceUnknown = 0, VoiceDmr, VoiceP25, VoiceP25p2, VoiceNxdn48, VoiceNxdn96, VoiceDstar, VoiceDpmr, VoiceYsf, VoiceTetra, VoiceM17,
                VoiceAnalogFm, VoiceCount };
const char* voiceSysName(int s);  // "DMR", "P25 Phase 1", ... "Analogue FM"
const char* voiceModeId(int s);   // the ModeTuning id of the receiver: "dmr", "p25", "p25p2", "nxdn", "dstar", "dpmr", "fusion", "tetra", "m17", "general"
int voiceAppFamily(int s);        // the app's mode family: 9 DMR, 50 P25, 180 P25 Phase 2, 38 NXDN, 33 D-STAR, 45 dPMR, 128 Fusion, 46 TETRA, 31 M17, 55 general

struct VoiceSignal {
    int sys = VoiceUnknown;
    double offHz = 0;             // centre from the samples' centre
    double rfHz = 0;              // offHz + VoiceOptions::rfHz
    double bwHz = 0, snrDb = 0;
    double baud = 0;              // the symbol rate the sync was found at (0: none)
    double devHz = 0;             // FSK: the outer deviation measured (Hz); DQPSK: 0
    int syncHits = 0;             // frame syncs found
    int syncErr = 0;              // median symbol errors in them
    bool inverted = false;        // the levels came the other way round (a radio that inverts the spectrum)
    double confidence = 0;        // 0..1
    int verdict = VerdictNone;
    int runnerUp = VoiceUnknown;  // the next best system, when there is one with any evidence
    std::string why;              // "DMR base station sync found 14 times (0-1 symbol errors), 4800 Bd, +-1.9 kHz"
};

struct VoiceResult {
    std::vector<VoiceSignal> signals;   // strongest first
    std::string headline;         // "DMR on 446.006 MHz, P25 Phase 1 on 446.206 MHz"
    double ms = 0;
};

struct VoiceOptions {
    double rfHz = 0;              // RF of the samples' centre
    size_t maxSignals = 6;
    double thresholdDb = 8;       // a signal must stand this far above the floor
    double minBwHz = 2500, maxBwHz = 30000;   // narrowband voice only
};

VoiceResult detectVoice(const cf32* x, size_t n, double rateHz, const VoiceOptions& o = VoiceOptions());
// One signal already at baseband (centred, other signals filtered out), at any rate of 48 kHz or more
VoiceSignal classifyVoice(const cf32* z, size_t n, double rateHz);
std::string voiceSummary(const VoiceResult& r);

// How long to collect: TV 0.35 s (a DVB-T2 frame is at most 250 ms), voice 1.2 s (a D-STAR data sync comes every 420 ms)
inline double tvCaptureSec() { return 0.35; }
inline double voiceCaptureSec() { return 1.2; }

// ---------------------------------------------------------------------------------------------------------------------------- capture
// Collects samples from the engine (push() from the analysis thread: a copy while armed) and runs the detector on a thread of its own.
class Capture {
public:
    enum Kind { KindTv = 1, KindVoice = 2 };
    enum State { Idle = 0, Collecting, Working, Done };
    Capture();
    ~Capture();
    void start(int kind, double rateHz, double rfHz, double seconds, const TvOptions& tvo = TvOptions());
    void cancel();
    bool armed() const { return armed_.load(std::memory_order_relaxed); }
    void push(const cf32* x, size_t n);
    int state() const;
    int kind() const;
    double progress() const;      // 0..1 while collecting
    std::string status() const;   // plain words: "listening for a digital voice signal...", "working out the standard..."
    bool tvResult(TvResult& out, uint64_t& seq) const;       // true when a result newer than seq is there (seq updated)
    bool voiceResult(VoiceResult& out, uint64_t& seq) const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<bool> armed_{false};
};

} // namespace autodet
} // namespace dect2
