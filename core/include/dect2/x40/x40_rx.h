// One receiver object for the 4.0 signal modes (docs/X40-4.0.md): which decoder runs is set from the active standard, as the avroad receiver does.
// Receive only. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread,
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// every active standard of the receiver (882 - 897), for switch statements
#define DECT2_X40_ACTIVE_CASES case 882: case 883: case 884: case 885: case 886: case 887: case 888: case 889: case 890: case 891: case 892: case 893: case 894: case 895: case 896: case 897:

namespace dect2 {

struct X40Telemetry;

namespace x40 {
enum Variant : int { RcPpm, Uvb76, Graves, Jove, Beacon, Em4100, kNumVariants };
// variant v: family 881 + v, active standard 882 + v, engine standard 883 + v, poll slot 881 + v (kNumFamilies = 900 and kPollSeqSlots = 901 stay)
constexpr int kFamilyBase = 880;
constexpr int variantOfActive(int active) { return active >= kFamilyBase + 2 && active <= kFamilyBase + 1 + (int)kNumVariants ? active - kFamilyBase - 2 : -1; }
const char* variantName(int v);
} // namespace x40

class X40Receiver {
public:
    X40Receiver();
    ~X40Receiver();
    void setVariant(int v);              // before configure()
    int variant() const;
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    bool ready() const;
    void reset();                        // the demodulators; the decoded list stays
    void feed(const cf32* x, size_t n);
    bool telemetry(X40Telemetry& out, uint64_t lastSeq);
    void clearResults();
    void clearResults(int variant);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning x40Tuning(int variant);

} // namespace dect2
