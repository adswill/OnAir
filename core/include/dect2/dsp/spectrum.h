#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {

struct SignalStats {
    float peak = 0;        // linear, full scale = 1
    float rmsDbfs = -120;
    float clipFraction = 0; // fraction of samples within 1 LSB of full scale
    float dcI = 0, dcQ = 0;
    uint32_t hist[64] = {}; // |I| and |Q| magnitude histogram (ADC occupancy)
};

// 10*log10(p) for 1e-14 <= p, the fast form the analyser uses (error below 0.0001 dB)
float spectrumDb(float p);

struct SpectrumFrame {
    std::vector<float> dbfs; // fftSize bins, DC-centred (-fs/2 .. +fs/2)
    SignalStats stats;
    uint64_t seq = 0;
};

class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(size_t fftSize = 4096);
    ~SpectrumAnalyzer();
    size_t fftSize() const { return n_; }
    static constexpr size_t kMaxFft = size_t(1) << 20;   // the finest manual choice: 2^20 bins (1M), about 2.3 Hz at 2.4 Msps (4.0; was 2^18, #243)
    static size_t autoSize(double sampleRate);   // the size that gives bins of about 125 Hz or finer, 8192..65536

    // consume samples, averages up to 32 blocks per frame
    void feed(const cf32* x, size_t n);
    bool takeFrame(SpectrumFrame& out); // false if nothing accumulated
    void setTransform(bool on);         // off = stats only
    // Blocks averaged into one frame (1..64). Few blocks give each waterfall row the true grain of the noise (an FFT bin of noise is a
    // chi-square variable; 16 averaged blocks flatten it to +-1.4 dB, a featureless floor) and cost far less CPU at high rates; the app
    // smooths the trace itself (a time constant). Was 16 before 3.0.
    static constexpr size_t kDefaultBlocks = 2;
    void setBlocksPerFrame(size_t n);
    size_t blocksPerFrame() const;
    // Fast waterfall rows (Morse / CW and other short signals): a row every 1/rowsPerSec seconds of signal, independent of the 30 frames a
    // second, each from a transform that is short enough to follow keying (about 20 ms or less; fastRowFft). A row is the mean power of up to
    // four such transforms spread over its hop. The rows queue up (oldest dropped when nobody takes them) and takeFastRows() hands them out in
    // one go. 0 = off (nothing extra is computed). The frames, the trace and everything that uses them are not touched.
    void setFastRows(double rowsPerSec, double sampleRateHz);
    static size_t fastRowFft(double sampleRateHz, double rowsPerSec);   // 1024..65536: ~20 ms of signal, and no more than ~8 M points a second of transforms
    // Moves the queued rows out: rows holds count * bins dB values (DC-centred like a frame); dt: seconds between rows (exact: the hop);
    // dropped: rows lost since the last call. Returns the number of rows (0: none, nothing changed in the outputs but dropped).
    size_t takeFastRows(std::vector<float>& rows, size_t& bins, double& dt, uint64_t& dropped);
    // The sample rate of what feed() gets (0 = unknown). With it, a transform too long for two blocks in a frame period (2 n / fs > 1/30 s) is
    // made from overlapping windows, one every fs / 30 samples, so the frame rate stays 30 a second (it was fs / 2n: 7 a second at 2.4 Msps
    // with 131072 bins). Without it, blocks follow each other as before.
    void setSampleRate(double sampleRateHz);
    void resize(size_t fftSize);        // power of two, 1024..kMaxFft: a new transform size (drops the accumulated data)
    // The window of the transforms (specwin::Type in zoom_spectrum.h; Hann by default). A change drops the accumulated data.
    void setWindow(int type);
    int window() const;
    // The overlap of long transforms (those for which two blocks do not fit in a frame period): at most this part of a transform is shared
    // with the one before (0 .. 0.95). Negative: automatic, the transforms start fs / 35 apart, but never more than 3/4 overlapped above
    // 2^18 bins (a 1M transform at 2.4 Msps then makes about 9 frames a second instead of 35 transforms a second).
    void setMaxOverlap(double f);
    static double hopFor(double fs, size_t n, double maxOverlap);   // samples between the starts of two overlapped transforms
    void reset();                       // forget accumulated data (after retuning)

private:
    struct Impl;
    Impl* p_;
    size_t n_;
};

} // namespace dect2
