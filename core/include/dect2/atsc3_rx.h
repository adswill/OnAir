// The ATSC 3.0 receiver as the engine uses it: samples in (any rate), a transport stream and telemetry out. Runs on its own threads, like the
// ATSC 1.0 receiver: one decodes the samples, one passes the transport stream on.
#pragma once
#include "atsc3_sync.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dect2 {

struct Atsc3Telemetry {
    uint64_t seq = 0;
    bool locked = false;                 // frames are being decoded
    double cfoHz = 0;
    long bootstraps = 0, frames = 0, framesFailed = 0, bbPackets = 0, bbBad = 0;
    long alpPackets = 0, udp = 0, llsTables = 0, routeObjects = 0;
    double secSinceFrame = 1e9;          // seconds (of wall clock) since the last decoded frame
    std::vector<atsc3::SltService> services;
    int selected = -1;
    bool serviceReady = false;
    atsc3::FrameInfo frame;
    long tsBytes = 0;
    long droppedBlocks = 0;              // sample blocks dropped because the decoder was behind
    double load = 0;                     // decoding time / signal time over the last seconds (1 = just keeping up)
};

class Atsc3Rx {
public:
    Atsc3Rx();
    ~Atsc3Rx();
    void configure(double sampleRate);
    void reset();                                       // starts again from the next samples; the chosen service is kept
    void feed(const cf32* x, size_t n);
    void setBlocking(bool b) { blocking_ = b; }         // recordings: wait when behind instead of dropping samples
    void setPacketCallback(std::function<void(const uint8_t*, size_t, double)> f) { cb_ = std::move(f); }
    void selectService(int serviceId);                  // -1: the first video service that appears
    bool telemetry(Atsc3Telemetry& t, uint64_t lastSeq);
    void stop();

private:
    void start();
    void worker();
    void pump();
    void autoSelect();

    double rate_ = 0;
    std::mutex mu_;
    std::condition_variable cv_, cvSpace_;
    std::deque<std::vector<cf32>> q_;
    size_t queued_ = 0;
    std::atomic<bool> stop_{true}, blocking_{false};
    std::thread worker_, pump_;
    std::unique_ptr<atsc3::Atsc3Receiver> rx_;
    std::unique_ptr<atsc3::Atsc3Sync> sync_;
    std::function<void(const uint8_t*, size_t, double)> cb_;
    std::atomic<int> want_{-1};
    std::atomic<long> dropped_{0}, tsBytes_{0};
    std::atomic<double> busySec_{0}, signalSec_{0};
    std::atomic<double> lastFrameWall_{0};
    std::atomic<long> lastFrames_{0};
    std::atomic<uint64_t> seq_{0};
    std::mutex tmu_;
    Atsc3Telemetry tel_;
};

} // namespace dect2
