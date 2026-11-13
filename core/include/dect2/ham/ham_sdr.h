// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The SDR transmit path of the ham modes: HamSlotTx audio (48 kHz mono) to single-sideband IQ at the radio's rate, into a TxSink of a radio that
// transmits (HackRF, LimeSDR, PlutoSDR, USRP, bladeRF). The chain: complex band-pass (300 .. 2900 Hz, the upper sideband; 361 taps, about 70 dB
// opposite-sideband rejection) -> drive -> ALC (peak hold below the power limit, instant attack, 10 dB/s release) -> cubic B-spline
// interpolation to the radio rate (images at 48 kHz +- the audio below -90 dBc; the output is a convex mix of ALC'd samples, so it never
// exceeds the limit). SdrHamIo keys the radio only through HamSlotTx: prepare() (after the gate) stores the checked frequency, ptt(true) refuses
// without it and tunes to exactly that frequency. Tests pass a recording sink; nothing here opens a radio by itself.
#pragma once
#include "dect2/ham/ham.h"
#include "dect2/source/source.h"
#include <atomic>
#include <complex>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::ham {

struct SdrTxConfig {
    double drive = 1.0;                // gain on the audio as it arrives (HamSlotTx has scaled it to its level already)
    double powerLimitPct = 100;        // percent of the radio's full output power (the IQ amplitude stays below sqrt(pct / 100) of full scale)
    double gainDb = 0;                 // the radio's TX gain (DeviceInfo::txGainMinDb .. txGainMaxDb)
    bool ampOn = false;                // HackRF: the +14 dB amplifier
    bool lsb = false;                  // lower sideband instead of upper (the digital modes use USB on every band)
    bool bandFilterConfirmed = false;  // the operator says a low-pass / band filter for this band is fitted (required: an SDR has strong harmonics)
    bool externalPa = false;           // an amplifier follows the SDR (warnings only)
    double rateHz = 0;                 // the radio rate; 0: chosen from the radio's range (a multiple of 48 kHz, about 2 MHz)
    double sinkLatencySec = 0.02;      // the radio's own buffering (added to latencySec())
};

// Hard refusals (why non-empty): only a radio that cannot transmit at all. Warnings are the things the operator should know (no PA, band
// filter, full or half duplex for watching one's own signal, gain at the top).
std::string sdrTxLimitWarnings(const DeviceInfo& d, const SdrTxConfig& c, double dialHz, double audioHz, double bwHz);   // warnings only, "" = none
bool sdrTxRefused(const DeviceInfo& d, const SdrTxConfig& c, double dialHz, double audioHz, double bwHz, std::string& why);
std::vector<std::string> sdrTxWarnings(const DeviceInfo& d, const SdrTxConfig& c, double dialHz);
double sdrTxRate(const DeviceInfo& d, const SdrTxConfig& c);   // the rate asked of the radio
constexpr double kSdrAudioLoHz = 300, kSdrAudioHiHz = 2900;      // the audio the SDR chain passes

// The DSP alone (pure, testable): 48 kHz audio in, IQ at outRate out.
class SdrTxChain {
public:
    SdrTxChain(double outRateHz, const SdrTxConfig& c);
    void reset();
    void process(const float* audio, size_t n, std::vector<cf32>& out);   // appends to out
    void flush(std::vector<cf32>& out);                                   // the filter tail (silence in)
    double latencySec() const;                                            // the chain's own delay
    double outRate() const { return outRate_; }
    struct Stats { float inPeak = 0, outPeak = 0, alcDb = 0; uint64_t alcSamples = 0, samplesOut = 0; };
    Stats stats() const { return st_; }
    std::function<void(const float*, size_t)> monitor;   // what is sent, as 48 kHz audio after the ALC (watching one's own signal)
private:
    void push48(cf32 z, std::vector<cf32>& out);
    double outRate_;
    SdrTxConfig c_;
    float limit_ = 1, gain_ = 1, release_ = 1;
    std::vector<float> hr_, hi_;          // the band-pass, real and imaginary part
    std::vector<float> hist_;             // input history, doubled for a contiguous window
    size_t hpos_ = 0;
    cf32 s_[4];                           // B-spline neighbours
    double u_ = 0, step_ = 1;
    int ratio_ = 0;                       // integer outRate / 48000, 0 when not an integer
    std::vector<float> w_;                // B-spline weights per phase (ratio_ * 4)
    uint64_t in_ = 0;
    Stats st_;
    std::vector<float> mon_;
};

class SdrHamIo : public HamRigIo {
public:
    // A new sink per burst (the factory claims the radio; see sdrSinkFactory). The sink is opened at the checked frequency and closed after the burst.
    using SinkFactory = std::function<std::unique_ptr<TxSink>(std::string& err)>;
    SdrHamIo(DeviceInfo d, SinkFactory make, SdrTxConfig c = SdrTxConfig());
    ~SdrHamIo() override;
    void setConfig(const SdrTxConfig& c);              // takes effect at the next burst
    SdrTxConfig config() const;
    void setMonitor(std::function<void(const float*, size_t)> m);
    std::vector<std::string> warnings(double dialHz) const { return sdrTxWarnings(dev_, config(), dialHz); }
    SdrTxChain::Stats stats() const;
    bool keyed() const { return keyed_; }

    bool prepare(const HamTxCheck& gate, std::string& err) override;
    bool ptt(bool on, std::string& err) override;
    bool beginAudio(int rate, std::string& err) override;
    int write(const float* mono, int frames) override;
    int pending() override { return 0; }   // write() returns when the radio has taken the samples
    void flushAudio() override { aborted_ = true; }
    void endAudio() override;
    double latencySec() const override;
    const char* kind() const override { return "sdr"; }
    std::string lastError() const override;

private:
    void closeSink(bool tail);
    DeviceInfo dev_;
    SinkFactory make_;
    mutable std::mutex cfgMu_, sinkMu_;
    SdrTxConfig cfg_;
    HamTxCheck gate_;
    bool prepared_ = false;
    std::atomic<bool> keyed_{false}, aborted_{false};
    std::unique_ptr<TxSink> sink_;
    std::unique_ptr<SdrTxChain> chain_;
    std::function<void(const float*, size_t)> monitor_;
    std::string err_;
    std::vector<cf32> buf_;
};

// The real radio: claims it for transmitting (txClaimRadio) and makes its TxSink; the claim is released when the sink is destroyed.
SdrHamIo::SinkFactory sdrSinkFactory(const DeviceInfo& d);

}  // namespace dect2::ham
