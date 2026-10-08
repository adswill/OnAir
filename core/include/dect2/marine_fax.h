// Weather fax (HF radiofax) on USB audio (no radio): the decoder and an endless test-signal audio source.
// Audio convention: real USB audio, black 1500 Hz, white 2300 Hz, any audio rate from 8 kHz (12 kHz recommended).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct FaxImage {
    int width = 0, height = 0;
    std::vector<uint8_t> pix;           // grey, row-major, 0 black .. 255 white
};

struct FaxStatus {
    int state = 0;                      // 0 waiting for the start tone, 1 phasing, 2 receiving, 3 stopped
    int ioc = 576, lpm = 120;
    int lines = 0;
    double slantPpm = 0;                // correction in use
    double toneHz = 0;                  // last tone measured (start/stop detection, tuning aid)
    double snrDb = 0;                   // tones: tone power over total audio power; phasing: 400 Hz swing over the noise of the black level
    // Added by the fax layer (the fields above keep their meaning):
    double blackHz = 1500;              // black level measured in the phasing (1500 when nothing measured yet): the mistuning aid
    int phasingLines = 0;               // phasing lines used to lock the line start
    int width = 0;                      // image width in pixels (0 until an image starts)
};

// Image width for an index of cooperation: IOC x pi, cut to a whole number as fldigi does (576 -> 1809, 288 -> 904).
int faxImageWidth(int ioc);

// The synthetic weather chart the test source sends (grid, coast, isobars, title block, grey wedge). Same chart for the
// same arguments, so a test can compare a decoded image with it.
FaxImage faxTestChart(int width, int rows, uint32_t seed);

// push() runs on the receiver thread; status() and latestImage() may be called from another thread at the same time.
class FaxDecoder {
public:
    FaxDecoder();
    ~FaxDecoder();
    void configure(double audioRate);
    void setLpm(int lpm);               // 60, 90, 120, 240; 0 = detect
    void setIoc(int ioc);               // 576 or 288; 0 = from the start tone
    void setSlantPpm(double ppm);       // trim of the line period in ppm (positive = a line lasts longer in audio samples,
                                        // i.e. the audio clock runs fast); changes the rows already received too
    void setAutoSlant(bool on);         // estimate the slant from the phasing lines (default on); the manual value is added to it
    void setMaxLines(int n);            // image height limit, default 1500; the image stops growing there
    void reset();
    void push(const float* audio, size_t n);
    FaxStatus status() const;
    bool latestImage(FaxImage& out, uint64_t& seq) const;   // true and a copy when the image changed since seq
private:
    struct State;
    std::unique_ptr<State> s_;
};

// Endless test transmission: start tone, phasing, a synthetic weather chart, stop tone, a pause, again.
class FaxAudioSource {
public:
    FaxAudioSource(double audioRate, int ioc, int lpm, int lines, uint32_t seed);
    ~FaxAudioSource();
    void generate(float* audio, size_t n);      // amplitude about 0.5
    // Seconds of start tone (300 Hz for IOC 576, 675 Hz for 288), phasing (rounded to whole lines), stop tone (450 Hz)
    // and black after the stop tone; defaults 5, 30, 5, 10 (fldigi wefax.cxx sends the same stop and black times).
    // Call before the first generate().
    void setTiming(double startToneS, double phasingS, double stopToneS, double blackS);
private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace dect2
