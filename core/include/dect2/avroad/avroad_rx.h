// One receiver object for the thirteen aviation, rail and road modes of 3.8 (docs/AVIA-ROAD-3.8.md): which decoder runs is set from the
// active standard, as the FengYun receiver does. Receive only. Written to the engine's contract: configure() once the sample rate is known,
// feed() from the analysis thread, telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct AvRoadTelemetry;

namespace avroad {
enum Variant : int { Fanet, Ogn, Gbas, Ldacs, Aeromacs, EotHot, Atcs, Dsrc, ItsG5, Cv2x, Ermes, MarineRadar, WeatherRadar, kNumVariants };
constexpr int kFamilyBase = 400;     // variant v: family 400 + v, active standard 401 + v, engine standard 402 + v, poll slot 300 + v
constexpr int variantOfActive(int active) { return active >= kFamilyBase + 1 && active <= kFamilyBase + (int)kNumVariants ? active - kFamilyBase - 1 : -1; }
const char* variantName(int v);
} // namespace avroad

class AvRoadReceiver {
public:
    AvRoadReceiver();
    ~AvRoadReceiver();
    void setVariant(int v);              // before configure()
    int variant() const;
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);         // what the capture is tuned to (0: unknown); some decoders use it to name the link
    bool ready() const;
    void reset();                        // the demodulators; the decoded list stays
    void feed(const cf32* x, size_t n);
    bool telemetry(AvRoadTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults(int variant) const;
    bool loadResults(const std::string& blob, int variant);
    void clearResults(int variant);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning avRoadTuning(int variant);

} // namespace dect2
