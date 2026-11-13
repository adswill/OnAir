// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transmit into an IQ file instead of a radio: a TxSink that writes what the generator makes. No radio is opened or claimed and no radio-only prompt
// applies. The formats: SigMF (.sigmf-data + .sigmf-meta with the rate, the centre frequency and the sample count; cf32_le, ci16_le or ci8_le),
// a raw .cf32 file, and a two-channel 16-bit WAV (I left, Q right). paced = true writes at the signal's own speed (a microphone as the source);
// paced = false writes as fast as the generator makes it (a WAV file as the source).
#pragma once
#include "dect2/source/source.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

enum class IqFileFormat { SigmfCf32 = 0, SigmfCi16 = 1, SigmfCs8 = 2, RawCf32 = 3, WavIq16 = 4,
                         BlueCf32 = 5, BlueCi16 = 6, BlueCs8 = 7 };   // X-Midas BLUE (.tmp): complex float / int16 / int8, rate and centre in the header
constexpr int kIqFileFormats = 8;
const char* iqFileFormatName(IqFileFormat f);   // "SigMF cf32", ...
// "onair_tx_<mode>_<MHz>MHz_<rate>sps_<yyyymmdd-hhmmss>" without an extension
std::string iqFileBaseName(const std::string& modeId, double centerHz, double rateHz);
// folderOrBase + the file name: a path that ends in a separator, or names an existing folder, gets baseName appended; the extension of the format is
// added when the path has none of its own (.sigmf-data / .sigmf-meta are written side by side)
std::string iqFilePath(const std::string& folderOrBase, const std::string& baseName, IqFileFormat f);
// the sample rates the panel offers for a file (Hz); the first is the mode's own choice when it is on the list
const double* iqFileRates(int& count);

// what the panel reads while the session owns the sink
struct IqFileTxCounters {
    std::atomic<uint64_t> samples{0}, bytes{0};
    std::atomic<double> rateHz{0};
    double seconds() const { const double r = rateHz.load(); return r > 0 ? (double)samples.load() / r : 0.0; }
};

struct IqFileTxConfig {
    std::string path;                 // the data file (see iqFilePath)
    IqFileFormat format = IqFileFormat::SigmfCf32;
    double centerHz = 0;              // written to the SigMF sidecar (0: left out)
    bool paced = false;
    std::string description;          // SigMF description, e.g. the mode id
    std::shared_ptr<IqFileTxCounters> counters;   // optional: kept up to date by the sink
};

class IqFileTxSink : public TxSink {
public:
    explicit IqFileTxSink(const IqFileTxConfig& c);
    ~IqFileTxSink() override;
    bool open(const TxTune& t, std::string& err) override;
    bool write(const cf32* x, size_t n, std::string& err) override;
    void close() override;
    double sampleRate() const override { return rate_; }
    bool realtime() const override { return cfg_.paced; }
    std::string name() const override { return "IQ file"; }
    uint64_t samplesWritten() const { return samples_.load(); }
    uint64_t bytesWritten() const { return bytes_.load(); }
    double seconds() const { return rate_ > 0 ? (double)samples_.load() / rate_ : 0.0; }
    const std::string& dataPath() const { return cfg_.path; }
private:
    void writeMeta(bool final);
    IqFileTxConfig cfg_;
    double rate_ = 0;
    void* f_ = nullptr;   // FILE*
    void* blue_ = nullptr;   // blue::Writer* for the BLUE formats
    std::atomic<uint64_t> samples_{0}, bytes_{0};
    std::chrono::steady_clock::time_point t0_;
    bool started_ = false;
    std::string when_;
};

// Shared with the panel: the sink as a TxSink, plus the counters through the raw pointer the caller keeps (the session owns the sink)
std::unique_ptr<IqFileTxSink> makeIqFileTxSink(const IqFileTxConfig& c);

} // namespace dect2
