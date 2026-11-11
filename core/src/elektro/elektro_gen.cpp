// Elektro-L HRIT / LRIT test signal (elektro_gen.h): GOES xRIT products at the symbol rate of the link in modeOpt[0] (665.4 ksym/s by default).
#include "dect2/elektro/elektro_gen.h"
#include "dect2/goes/goes_gen.h"
#include "dect2/elektro/elektro_tel.h"

namespace dect2 {

namespace {
class ElektroSynth : public ModeSynth {
public:
    ElektroSynth(const SynthConfig& s, double rate) : rate_(rate) {
        GoesGenConfig c;
        c.rate = rate;
        const int link = s.modeOpt[0];   // the receiver's link numbers (elektro_tel.h); GGAK (4) has its own generator: the HRIT rate then
        c.symbolRate = link == 4 ? 665400.0 : elektroSymbolRate(link);
        c.qpsk = c.oqpsk = link == 6;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.sroPpm = s.sroPpm;
        gen_ = std::make_unique<GoesGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<GoesGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeElektroSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<ElektroSynth>(cfg, sampleRate);
}

} // namespace dect2
