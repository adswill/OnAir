// FT8 / FT4 / FT2 / WSPR test audio (see dect2/hfdig_ftx.h): every slot the same stations with known messages, frequencies, levels and
// time offsets, from sample 0 = the start of a slot. Nothing is transmitted.
#include "dect2/hfdig_ftx.h"
#include "dect2/hfdig_gen.h"
#include "hfdig_ftx_int.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace dect2 {

std::vector<FtxTestStation> ftxTestStations(int mode) {
    switch (mode) {
    case ftx::kFt8:
        return {{"CQ K1ABC FN42", 600, 0, 0.0},        {"K1ABC W9XYZ -12", 900, -6, 0.3},     {"W9XYZ K1ABC R-05", 1250, -10, -0.4},
                {"CQ PJ4/K1ABC", 1500, -3, 0.1},       {"TNX BOB 73 GL", 1800, -12, 0.6},     {"G4ABC DL1XYZ RR73", 2100, -8, 0.0},
                {"CQ JA1XYZ PM95", 2450, -14, 1.0},    {"7123456789ABCDEF01", 2800, -9, -0.2}};
    case ftx::kFt4:
        return {{"CQ K1ABC FN42", 700, 0, 0.0},        {"K1ABC W9XYZ EN37", 1100, -5, 0.2},   {"W9XYZ K1ABC +03", 1600, -8, -0.3},
                {"CQ TEST DL1XYZ JO62", 2100, -3, 0.0}, {"VK2ABC ZL1XYZ 73", 2600, -10, 0.5}};
    case ftx::kFt2:
        return {{"CQ K1ABC FN42", 800, 0, 0.0}, {"K1ABC W9XYZ -10", 1400, -4, 0.1}, {"W9XYZ K1ABC RRR", 2200, -7, -0.1}};
    case ftx::kWspr:
        return {{"K1ABC FN42 37", 1450, 0, 0.0},  {"G4XYZ IO91 23", 1480, -6, 0.5},     {"PJ4/K1ABC 30", 1520, -10, -0.5},
                {"DL1XYZ/P 10", 1550, -4, 1.0},   {"<K1ABC> FN42AX 37", 1580, -8, 0.2}};
    default: return {};
    }
}

namespace {

class FtxTestAudio : public HfdigTestAudio {
public:
    FtxTestAudio(int mode, const SynthConfig& c) {
        const ftx::Spec& s = ftx::spec(mode);
        const size_t n = (size_t)std::lround(s.period * kHfdigAudioRate);
        slot_.assign(n, 0.f);
        const bool mir = c.modeOpt[1] == 1;
        for (const auto& st : ftxTestStations(mode)) {
            const double amp = 0.5 * std::pow(10.0, st.relDb / 20.0);
            const double hz = st.hz + c.modeVal[0];
            for (int w = -1; w <= 1; w++)   // the slot repeats: a transmission over its end comes back at its start
                ftxAddTransmission(mode, st.msg, hz, amp, kHfdigAudioRate, s.t0 + st.dt + w * s.period, mir, slot_);
        }
        if (c.modeOpt[2] == 1) {   // live demonstration: start where the system clock is in the slot, so that the decodes sit at DT 0
            const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            pos_ = (size_t)(std::fmod(now, s.period) * kHfdigAudioRate) % slot_.size();
        }
    }
    void generate(float* out, size_t n) override {
        for (size_t i = 0; i < n; i++) { out[i] = std::max(-1.f, std::min(1.f, slot_[pos_])); if (++pos_ >= slot_.size()) pos_ = 0; }
    }
private:
    std::vector<float> slot_;
    size_t pos_ = 0;
};

} // namespace

std::unique_ptr<HfdigTestAudio> makeFtxTestAudio(const SynthConfig& cfg) {
    const int which = cfg.modeOpt[0];
    const int mode = which == 3 ? ftx::kFt8 : which == 4 ? ftx::kFt4 : which == 5 ? ftx::kWspr : which == 6 ? ftx::kFt2 : -1;
    if (mode < 0) return nullptr;
    return std::make_unique<FtxTestAudio>(mode, cfg);
}

} // namespace dect2
