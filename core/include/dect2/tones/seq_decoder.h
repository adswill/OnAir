// Five-tone sequential selective calling (ZVEI1 / ZVEI2, CCIR, EEA, EIA, DZVEI ...) on 8 kHz audio, as a stand-alone piece so the Pagers mode can
// run it on the same channel as POCSAG / FLEX (CCIR selcalls are sent on the same pager frequencies).
// Input: FM-demodulated audio at 8 kHz in Hz of deviation (the discriminator output), no filtering needed (a DC offset is tolerated).
// A call is reported after its last tone, once the line has been quiet for 160 ms. The standard is picked by the tone plan, and for the
// plans that share their tones (CCIR / EEA) by the tone length.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

struct SeqCall {
    std::string digits;         // "1465": the repeat tone is shown as the digit it repeats
    std::string raw;            // "14E5" style: with the repeat tone as E
    std::string standard;       // "CCIR", "ZVEI1", ...
    std::string alsoFits;       // other plans the tones also fit, "" when unambiguous
    int tones = 0;
    int toneMs = 0;             // median tone length
    double endSec = 0;          // audio seconds since reset() at the end of the call
};

class SeqToneDecoder {
public:
    SeqToneDecoder();
    void reset();
    void setCallback(std::function<void(const SeqCall&)> cb) { cb_ = std::move(cb); }
    void feed(const float* x, size_t n);
    uint64_t rejected() const { return rejected_; }     // tone groups that fitted no plan

private:
    struct Seg { int pool; int dwellMs; };
    void hop();
    void close();
    std::function<void(const SeqCall&)> cb_;
    std::vector<float> ring_;
    int64_t n_ = 0;
    std::vector<Seg> segs_;
    int cur_ = -1, run_ = 0, pend_ = -2, pendRun_ = 0, silent_ = 0;
    uint64_t rejected_ = 0;
};

} // namespace dect2
