// Recording the radio's IQ samples to a file, for bug reports and for replaying a signal later.
// The analysis thread only converts a block and puts it in a bounded queue; a separate writer thread does the disk writes, so a slow disk
// never slows the receiver down: when the queue is full the block is dropped and counted.
#pragma once
#include "source.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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
// The engine standard a recording's mode id names ("fm" 7, "ale" ...), -1 for "dvb" (T or T2), "iq" and unknown ids: replay restores the mode from it
int recordingStdMode(const std::string& modeId);
// The short name of an engine standard as used in recording names ("dvb", "fm", "dab", ...)
std::string recordingModeId(int stdMode);
// The same name with the BLUE extension ("...Msps_<stamp>.tmp"): the recorder writes an X-Midas BLUE file for it
std::string blueRecordingName(const std::string& name);

// SigMF (what IQEngine reads): the metadata file next to a recording. datatype: cs8 ri8, cu8 ru8_le, cs16 ri16_le, cf32 cf32_le.
struct SigmfInfo {
    FileFormat fmt = FileFormat::CS8;
    double rateHz = 0;
    double centerHz = 0;          // 0: unknown (no frequency in the capture)
    std::string datetimeUtc;      // "2026-10-13T09:15:30Z" ("" leaves it out)
    std::string dataFile;         // the data file's own name, for core:dataset ("" leaves it out)
    uint64_t sampleCount = 0;     // 0 leaves it out
    std::string description;      // "" leaves it out
};
const char* sigmfDatatype(FileFormat fmt);   // "ci8_le", "cu8", "cf32_le"
std::string sigmfMetaJson(const SigmfInfo& i);
std::string sigmfUtcNow();

class IqRecorder {
public:
    ~IqRecorder() { stop(); }
    // SigMF metadata (<name>.sigmf-meta next to the recording, and <name>.sigmf-data as a hard link to the same data so that IQEngine pairs them) for the next start()
    // description: the sidecar's description (the mode id the recording was made in, read back by the recording library)
    void setSigmf(bool on, double centerHz = 0, std::string description = "") { sigmf_ = on; centerHz_ = centerHz; sigmfDesc_ = std::move(description); }
    // Opens the file (creating its folder) and starts the writer thread. CS8, CI16 (16-bit) and CF32 can be recorded.
    // A path ending .tmp / .blue / .prm is written as an X-Midas BLUE file (bluefile.h: complex int8 / float32, rate and centre in the header
    // and keywords); no SigMF files are made for it.
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
    void* blue_ = nullptr;   // blue::Writer* when the recording is a BLUE file
    bool sigmf_ = false;
    double centerHz_ = 0;
    std::string sigmfPath_, sigmfWhen_, sigmfDesc_;
    void writeSigmf(bool final);
};

} // namespace dect2
