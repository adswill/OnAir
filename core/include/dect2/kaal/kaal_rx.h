// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice-layer receiver (streaming C++ port of the Python prototype kaal_rx.py): IQ in, 16 kHz mono speech out.
// Pipeline: DC removal, sweep (signature) removal per period of 24240 samples with the variant decided per period (and the station ID
// read from the variants), matched filter, symbol timing, preamble frame sync on frames of 160 symbols, per-frame complex gain fit on
// preamble + pilots with a per-frame noise estimate, max-log 16-QAM demap, pseudo-random deinterleave (4-frame blocks), rate-matched
// LDPC decode (the word size 240 or 320 information bits is tried), codec v3 whose profile (12 / 16 / 24 / 32 kbit/s) is read from the
// frame header. The first 3 s of a stream are used to estimate the static parameters (sweep period, symbol clock, CFO), then the
// receiver runs frame by frame and tracks the sweep and the symbol clock. Decoding starts at the first interleaver block boundary.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "dect2/kaal/kaal_sweepid.h"

namespace dect2::kaal {

struct RxStats {
    bool locked = false;
    uint64_t framesGood = 0, framesBad = 0;
    float snrDb = 0, cfoHz = 0, sweepDb = 0;
    uint64_t samplesIn = 0;
    int bitrateKbps = 0;           // codec profile of the last decoded frame (12, 16, 24 or 32), read from the frame header
    // station ID carried in the sweep (kaal_sweepid.h): valid once the same ID decoded in two cycles at least 12 periods apart
    bool idValid = false;
    std::string idCall;            // callsign with the suffix class, e.g. LY2ABC/P
    float idGateDb = 0;            // mean winner-to-other variant energy of the sweep detection over the last cycle
};

class VoiceRx {
public:
    VoiceRx();
    ~VoiceRx();
    VoiceRx(const VoiceRx&) = delete;
    VoiceRx& operator=(const VoiceRx&) = delete;

    void configure(double sampleRate);          // input sample rate; the receiver resamples to 48 kHz internally if it differs
    void reset();
    // Fast acquisition: lock on a 1.2 s buffer, then longer ones (default off = the 3 s buffer only). Standalone lock in about 1.2 s; in the engine chain
    // (coarse carrier search, refine restart, beacon slice) the short seeds were not stable enough yet, so the engine does not turn it on.
    void setFastAcquire(bool on);
    // The input's carrier offset changed by dHz (the caller retuned its mixer): keep the lock, move the carrier and sweep trackers.
    void shiftCarrier(double dHz);
    void feed(const std::complex<float>* x, size_t n);   // IQ in, any chunk size
    size_t popAudio(int16_t* out, size_t maxSamples);    // decoded 16 kHz mono speech, delay already removed
    RxStats stats() const;
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Sweep-only station ID scanner for signals too weak for the voice receiver (the sweep is found by correlation, 40 dB of processing gain,
// where the voice frames need about 0 dB per-sample SNR): keeps the last 36 s of input and, from 26 s on, searches it every 10 s for the four
// sweep variants and the ID cycle. An ID is reported once it decoded in two cycles (about 18 s of good sweep at least). Costs about a second of
// processing per scan, done on a worker thread (feed() only copies the window), so run it only while the voice receiver is not locked.
class SweepIdScanner {
public:
    SweepIdScanner();
    ~SweepIdScanner();
    SweepIdScanner(const SweepIdScanner&) = delete;
    SweepIdScanner& operator=(const SweepIdScanner&) = delete;
    void configure(double sampleRate);
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    bool poll(SweepIdResult& r);          // true once for every new ID
    SweepIdResult last() const;
    uint64_t scans() const;
    void setSynchronous(bool on);         // tests: run each scan inside feed() instead of on the worker thread
    void wait();                          // blocks until the scan in flight is done
    float lastGateDb() const;             // mean winner-to-others variant energy of the last scan (a sweep present gives several dB)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2::kaal
