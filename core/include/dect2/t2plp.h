// PLP decoding: time/cell de-interleaving, demapping, LDPC, BCH, BB-frame header parsing.
// Runs on worker threads so that the receive path never stalls.
#pragma once
#include "t2fec.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>

namespace dect2 {

struct PlpJob {
    uint64_t frameNo = 0;
    int t2Frame = 0;        // L1-post FRAME_IDX
    double frameSec = 0;    // duration of the T2 frame
    int plpId = 0;
    PlpFec fec;
    int numBlocks = 0;
    int tiBlocks = 0;
    std::vector<cf32> cells;  // numBlocks * cellsPerBlock equalised cells (as received, still interleaved)
    std::vector<float> n0;    // noise variance per real dimension for every cell
};

struct BbFrame {
    int blockIndex = 0;
    BbHeader header;
    std::vector<uint8_t> bits; // descrambled BB frame, kBch bits (one per byte); empty if the block failed
};

struct PlpResult {
    uint64_t frameNo = 0;
    int t2Frame = 0;
    double frameSec = 0;
    int plpId = 0;
    PlpFec fec;
    int blocks = 0, blocksOk = 0, bchFailed = 0, headerOk = 0;
    int retryRecovered = 0;     // blocks that only decoded on the second-chance pass
    int bchCorrected = 0;       // total bit errors corrected by BCH
    double avgLdpcIters = 0;
    double preBer = 0;          // hard-decision bit error rate before LDPC (measured against the decoded codewords)
    double merDb = 0;           // modulation error ratio over correctly decoded blocks
    double decodeMs = 0;
    bool usedGpu = false;       // LDPC ran on the GPU
    std::vector<BbFrame> frames;
    std::vector<cf32> constellation; // equalised cells of decoded blocks (subsampled)
    std::vector<uint8_t> constErr;   // 1 where the nearest ideal point is not the transmitted one
    std::vector<uint16_t> constTx;   // label of the transmitted (unrotated) point for each display cell
};

class PlpDecoder {
public:
    explicit PlpDecoder(int threads = 0);
    ~PlpDecoder();
    // Returns false (and drops the job) if the decoder is still busy with earlier frames.
    bool submit(PlpJob&& job);
    bool poll(PlpResult& out); // oldest finished result
    int pending() const;
    uint64_t dropped() const { return dropped_; }
    static PlpResult decodeNow(const PlpJob& job, int threads, bool useGpu = false, const std::atomic<int>* backlog = nullptr);
    // 0 = CPU, 1 = GPU, 2 = auto (CPU until it falls behind, then GPU)
    // Auto means: the GPU whenever there is one (decoding on the CPU only keeps up on easy signals, and a frame that is late is a hole in the picture)
    void setMode(int m) { mode_ = m; autoGpu_ = m == 2 && gpuOk_; switched_ = false; pathFrames_ = 0; slowRun_ = 0; }
    int mode() const { return mode_; }
    bool gpuAvailable() const { return gpuOk_; }
    bool autoOnGpu() const { return autoGpu_; }

private:
    void loop();
    int threads_;
    std::thread th_;
    mutable std::mutex mu_;
    std::condition_variable idle_;
    std::condition_variable cv_;
    std::deque<PlpJob> jobs_;
    std::deque<PlpResult> results_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<int> backlog_{0};   // jobs waiting behind the one being decoded
    bool busy_ = false;
    std::atomic<int> mode_{2};
    std::atomic<bool> autoGpu_{false};
    bool gpuOk_ = false;
    uint64_t lastDropped_ = 0;
    double emaGpuMs_ = 0, emaCpuMs_ = 0;   // smoothed decode time of each path (Auto mode)
    int slowRun_ = 0, pathFrames_ = 0;
    bool switched_ = false;                 // Auto mode has already changed path once                       // consecutive frames the current path was too slow for
};

} // namespace dect2
