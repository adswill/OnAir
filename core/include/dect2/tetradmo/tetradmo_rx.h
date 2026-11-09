// TETRA Direct Mode receiver (ETSI EN 300 396-2 and -3): pi/4-DQPSK at 18 ksym/s on one 25 kHz channel. The synchronisation bursts (DSB) are found on their training
// sequence; their channels name the addresses and call; the normal bursts (DNB) of the call are then read from the timing the DSB gave: signalling on SCH/F and STCH, clear speech
// through the user-installed speech codec. Calls with air interface or end-to-end encryption are reported and not decoded. Receive only.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/tetradmo/tetradmo_proto.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {
namespace tetra { class SpeechFrameSink; }

struct TetraDmoTelemetry;

class TetraDmoReceiver {
public:
    TetraDmoReceiver();
    ~TetraDmoReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void clearMessages();
    void feed(const cf32* x, size_t n);
    bool telemetry(TetraDmoTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setVoiceSink(tetra::SpeechFrameSink* sink);   // clear traffic bursts go here; encrypted or unknown calls never do
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning tetraDmoTuning();

} // namespace dect2
