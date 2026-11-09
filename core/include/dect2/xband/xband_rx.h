// Receiver of the X-band links (xband_tel.h): one class, one profile per mode-list entry. BPSK, QPSK or OQPSK with a root raised cosine, Gardner timing and a Costas loop; the phase and
// pairing ambiguities settled by the sync word; K = 7 Viterbi or none; the shared CADU layer and the instrument decoders. Written to the engine's contract: configure() once the
// sample rate is known, feed() from the analysis thread, telemetry() and the setters from the interface thread. pushCadu() takes decoded CADUs (a .cadu file) without the demodulator.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/xband/xband_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct XbProductImage { std::string name, note; int width = 0, height = 0; std::vector<uint32_t> rgba; };

class XbandReceiver {
public:
    XbandReceiver(int profile);
    ~XbandReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void clearImage();
    void feed(const cf32* x, size_t n);
    bool telemetry(XbTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const { return {}; }
    bool loadResults(const std::string&) { return false; }
    void setLogCallback(std::function<void(const std::string&)> cb);
    int profile() const;
    void finish();                                          // the end of a pass: what is buffered goes to the channels, the telemetry is published
    void pushCadu(const uint8_t* cadu);                     // frames already synchronised, starting at the 0x1ACFFC1D sync word, not yet derandomised / corrected
    bool setCaduOutput(const std::string& path);            // the decoded frames are appended to this file; "" closes it
    std::vector<XbProductImage> products(int maxWidth) const;   // colour composites and products of the pass so far (xband_products.h)
    bool readPlane(int channel, int& width, int& height, std::vector<uint16_t>& px) const;   // tests
    std::vector<XbProductImage> channelImages() const;           // each channel full size, 8 bit grey

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning xbandTuning(int profile);

// A recording played straight through the receiver at CPU speed, off the analysis thread (X-band rates are often too much for a live chain):
// .cadu, or baseband as WAV (8 or 16 bit), SigMF (.sigmf-meta next to .sigmf-data), or raw (cs8, cu8, cs16, cf32, by extension or the format argument).
struct XbFileJob {
    std::string path, format;            // format "": from the extension
    double sampleRate = 0;               // 0: from the file
    double offsetHz = 0;
};
class XbFileRunner {
public:
    XbFileRunner();
    ~XbFileRunner();
    bool start(XbandReceiver& rx, const XbFileJob& j, std::string* err);
    void stop();
    bool running() const;
    float progress() const;
    std::string status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
