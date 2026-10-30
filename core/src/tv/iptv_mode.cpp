// Internet TV (IPTV) as a mode row: no radio and no samples, the engine only needs a tuning entry (engine standard 211, family 209) and a silent
// test signal so that the mode can be selected like any other. The stream itself is read by iptv::Stream (iptv_stream.cpp) and played by the Player.
#include "dect2/tv/iptv_mode.h"

namespace dect2 {
ModeTuning iptvTuning() {
    ModeTuning t;
    t.stdMode = 211; t.id = "iptv"; t.name = "Internet TV (IPTV)";
    t.minMhz = 1; t.maxMhz = 6000; t.defMhz = 100;
    t.sampleRate = 2e6;
    t.bandwidthMhz = 1;
    t.minSampleRate = 1e5;
    return t;
}
namespace {
class SilentSynth : public ModeSynth {
public:
    explicit SilentSynth(double r) : r_(r) {}
    double sampleRate() const override { return r_; }
    void generate(cf32* out, size_t n) override { for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f); }
private:
    double r_;
};
}
std::unique_ptr<ModeSynth> makeIptvSynth(const SynthConfig&, double sampleRate) { return std::make_unique<SilentSynth>(sampleRate); }
}
