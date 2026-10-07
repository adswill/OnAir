// GNSS test signal: the simulated sky of gnss_sim.h as the engine's synthetic source (see gnss_gen.h for the options).
#include "dect2/gnss_gen.h"
#include "dect2/gnss_sim.h"

namespace dect2 {

namespace {
class GnssSynth : public ModeSynth {
public:
    GnssSynth(const GnssSimConfig& c, double rate) : sim_(c, rate), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { sim_.generate(out, n); }
private:
    GnssSim sim_;
    double rate_;
};
} // namespace

GnssSimConfig gnssSimConfigFrom(const SynthConfig& cfg) {
    GnssSimConfig c;
    c.systems = cfg.modeOpt[0] > 0 ? (unsigned)cfg.modeOpt[0] : 1u;
    c.maxSats = cfg.modeOpt[1] > 0 ? cfg.modeOpt[1] : 0;
    c.warmStart = cfg.modeOpt[2] == 0;
    c.jammer = (cfg.modeOpt[3] & 1) != 0;
    if (cfg.modeOpt[3] & 2) c.dcOffset = 0.02;
    c.noiseRms = 0.14;
    c.seed = cfg.modeOpt[4] > 0 ? (unsigned)cfg.modeOpt[4] : 1u;
    if (cfg.modeVal[0] != 0 || cfg.modeVal[1] != 0) { c.latDeg = cfg.modeVal[0]; c.lonDeg = cfg.modeVal[1]; c.heightM = cfg.modeVal[2]; }
    if (cfg.modeVal[3] > 0) c.cn0Top = cfg.modeVal[3];
    c.cfoHz = cfg.cfoHz;
    c.sroPpm = cfg.sroPpm;
    return c;
}

std::unique_ptr<ModeSynth> makeGnssSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 2.0e6 || sampleRate > 21e6) return nullptr;
    return std::make_unique<GnssSynth>(gnssSimConfigFrom(cfg), sampleRate);
}

} // namespace dect2
