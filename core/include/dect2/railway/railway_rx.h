#pragma once
#include "railway_tel.h"
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include <functional>
#include <memory>
namespace dect2 {
class RailwayReceiver {
public:
    explicit RailwayReceiver(railway::Protocol protocol = railway::Protocol::Klw);
    ~RailwayReceiver();
    void configure(double rate);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void clearMessages();
    void clearResults() { clearMessages(); }
    void feed(const cf32* iq, size_t n);
    bool telemetry(RailwayTelemetry& out, uint64_t lastSeq);
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    bool busy() const { return false; }
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
ModeTuning klwTuning();
ModeTuning tdcsTuning();
} // namespace dect2
