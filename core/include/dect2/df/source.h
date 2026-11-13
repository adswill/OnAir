// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// N-channel coherent IQ sources for direction finding: a deterministic simulator, an interleaved cf32 / SigMF file reader, a parser for the
// KrakenSDR Heimdall DAQ IQ frame header (public layout, see docs/direction-finding.md for what is verified) and a tolerant importer of
// bearings from a KrakenSDR DoA server JSON/CSV line.
#pragma once
#include "dect2/df/df.h"
#include <memory>
#include <string>

namespace dect2::df {

// One block of synchronous samples: x[channel][t]
using Frame = std::vector<std::vector<cd>>;

class MultiChannelSource {
public:
    virtual ~MultiChannelSource() = default;
    virtual int channels() const = 0;
    virtual double sampleRate() const = 0;
    virtual double centerFreq() const = 0;
    // Fills `out` with up to n samples per channel; false at the end of the data
    virtual bool read(Frame& out, size_t n) = 0;
};

struct SimSource {          // one transmitter in the simulated scene
    double bearingDeg = 0;
    double snrDb = 10;      // per-element SNR
    double offsetHz = 0;    // baseband frequency offset of this source (decorrelates two sources)
    double bandwidthHz = 0; // 0 = white (fills the whole sample rate); > 0 = a narrowband message of about this width around offsetHz
    int copyOf = -1;        // >= 0: coherent copy of that source's waveform (a reflection), delayed by `delay` samples
    int delay = 0;
    uint64_t seed = 0;
};

struct SimConfig {
    Array array;
    double freqHz = 100e6;
    double sampleRate = 250e3;
    std::vector<SimSource> sources;
    std::vector<cd> elementGains;       // unknown receiver chain amplitude/phase per element (empty = ideal)
    bool calibrationNoise = false;      // the receiver's noise source is on: a common noise feeds every element, no antenna signal
    double calibrationSnrDb = 20;
    uint64_t seed = 1;
};

class SimMultiChannelSource : public MultiChannelSource {
public:
    explicit SimMultiChannelSource(SimConfig c);
    int channels() const override { return cfg_.array.n(); }
    double sampleRate() const override { return cfg_.sampleRate; }
    double centerFreq() const override { return cfg_.freqHz; }
    bool read(Frame& out, size_t n) override;
    SimConfig& config() { return cfg_; }
private:
    SimConfig cfg_;
    Rng rng_;
    uint64_t t_ = 0;
    std::vector<Rng> srcRng_;
    std::vector<std::array<cd, 2>> lp_;   // narrowband message filter state per source
    std::vector<std::vector<cd>> hist_;   // per-source waveform history for delayed copies
};

// Interleaved complex float32 (ch0 ch1 ... chN-1 per time step), raw file or the .sigmf-data next to a .sigmf-meta.
class FileMultiChannelSource : public MultiChannelSource {
public:
    // `channels` = 0 reads core:num_channels from the .sigmf-meta (unverified against other SigMF writers), else it must be given.
    FileMultiChannelSource(const std::string& path, int channels, double sampleRate, double centerFreq);
    ~FileMultiChannelSource() override;
    bool ok() const { return f_ != nullptr && n_ > 0; }
    int channels() const override { return n_; }
    double sampleRate() const override { return fs_; }
    double centerFreq() const override { return fc_; }
    bool read(Frame& out, size_t n) override;
private:
    void* f_ = nullptr;
    int n_ = 0;
    double fs_ = 0, fc_ = 0;
};

// ---- KrakenSDR Heimdall DAQ IQ header (1024 bytes, little-endian host layout, sync word 0x2BF7B95A), from the public iq_header.py
struct KrakenIqHeader {
    uint32_t syncWord = 0, frameType = 0, unitId = 0, activeAntChs = 0, iooType = 0, cpiLength = 0, daqBlockIndex = 0, cpiIndex = 0;
    uint32_t dataType = 0, sampleBitDepth = 0, adcOverdriveFlags = 0, delaySyncFlag = 0, iqSyncFlag = 0, syncState = 0, noiseSourceState = 0, headerVersion = 0;
    char hardwareId[17] = {0};
    uint64_t rfCenterFreq = 0, adcSamplingFreq = 0, samplingFreq = 0, timeStamp = 0, extIntegrationCntr = 0;
    uint32_t ifGains[32] = {0};
};
constexpr uint32_t kKrakenSyncWord = 0x2BF7B95Au;
constexpr size_t kKrakenHeaderSize = 1024;
bool parseKrakenIqHeader(const uint8_t* buf, size_t len, KrakenIqHeader& h);
// Payload after a header: cpiLength samples for each of activeAntChs channels. Assumes CHANNEL-MAJOR cf32 (all of channel 0, then 1, ...)
// when sampleBitDepth == 32 with dataType 1, else interleaved int8 I/Q: UNVERIFIED, see the doc.
bool decodeKrakenPayload(const KrakenIqHeader& h, const uint8_t* payload, size_t len, Frame& out);

// ---- bearings imported from a KrakenSDR DoA server line (no coherent IQ needed in OnAir)
struct ImportedBearing { bool ok = false; double bearingDeg = 0, confidence = 0, freqHz = 0, latDeg = 0, lonDeg = 0; bool hasPos = false; };
// Accepts a JSON object with any of doa/max_doa/bearing/angle (deg), confidence/conf (0..100 or 0..1), freq/frequency (Hz or MHz), latitude/longitude,
// or a CSV line "timestamp,doa,confidence,rssi,freq_hz,..." (field order UNVERIFIED: the first two numeric fields after a timestamp are taken as bearing, confidence).
ImportedBearing parseKrakenDoaLine(const std::string& line);

}   // namespace dect2::df
