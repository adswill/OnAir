#pragma once
#include "dect2/audio/videoin.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

struct VideoIn::Impl {
    void* backend = nullptr;
    std::mutex mu;
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;          // packed RGB24, latest frame
    std::atomic<uint64_t> seq{0};
    std::atomic<bool> stop{false};
    std::thread fileTh;
    // a file: the frame on show follows the clock of whoever consumes it (setClock); until one is given, the wall clock
    std::atomic<double> clock{-1};
    std::chrono::steady_clock::time_point wallStart;
    std::condition_variable cv;
    double pubPts = -1, pubDur = 0;    // the frame published last (under mu)
    bool fileDone = false;             // (under mu) the decoding thread has ended
    double clockNow() const {
        const double c = clock.load();
        return c >= 0 ? c : std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
    }
    void setRgb(int nw, int nh, const uint8_t* packed);
};

bool videoInCameraStart(VideoIn::Impl* impl, const std::string& deviceId, std::string& err);
void videoInCameraStop(VideoIn::Impl* impl);
std::vector<VideoInputDevice> videoInListCameras();

} // namespace dect2
