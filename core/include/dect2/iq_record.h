// Recording the radio's IQ samples to a file, for bug reports and for replaying a signal later.
// The analysis thread only converts a block and puts it in a bounded queue; a separate writer thread does the disk writes, so a slow disk
// never slows the receiver down: when the queue is full the block is dropped and counted.
#pragma once
#include "dect2/source.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

struct RecordingStats {
    bool active = false;
    std::string path;
    double seconds = 0;          // length of the signal recorded so far
    uint64_t bytes = 0;          // size of the file (including what is still waiting to be written)
    uint64_t droppedSamples = 0; // samples left out because the disk could not keep up
    std::string error;           // why a recording ended by itself (disk full, ...); empty otherwise
};

// "onair_dvb_554.000MHz_10Msps_20261009-071530.cs8": guessSampleRate() and guessFormat() read the rate and the format back from it
std::string recordingFileName(const std::string& modeId, double centerHz, double rateHz, FileFormat fmt);
// The short name of an engine standard as used in recording names ("dvb", "fm", "dab", ...)
std::string recordingModeId(int stdMode);

class IqRecorder {
public:
    ~IqRecorder() { stop(); }
    // Opens the file (creating its folder) and starts the writer thread. Only CS8 and CF32 can be recorded.
    bool start(const std::string& path, FileFormat fmt, double rateHz, std::string& err);
    void stop();
    bool active() const { return active_.load(std::memory_order_relaxed); }
    // Called by the analysis thread with the radio's samples; never waits for the disk.
    void push(const cf32* x, size_t n);
    RecordingStats stats() const;

    static constexpr size_t kQueueBytes = 64u << 20;
    static constexpr uint64_t kMaxBytes = 4ull << 30;   // safety cap: a recording stops here

private:
    void writerLoop();
    std::atomic<bool> active_{false};
    FileFormat fmt_ = FileFormat::CS8;
    double rate_ = 0;
    std::string path_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> q_;
    size_t qBytes_ = 0;
    bool stopReq_ = false;
    uint64_t samples_ = 0, bytes_ = 0, dropped_ = 0;
    std::string error_;
    std::thread th_;
    void* file_ = nullptr;   // FILE*
};

} // namespace dect2
