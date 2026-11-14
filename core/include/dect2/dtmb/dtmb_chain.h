// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DTMB channel decoder: the data symbols of every signal frame in, transport stream packets out.
// de-interleaver (symbols, or bits for 4QAM-NR) -> soft demapper (-> NR decoder) -> codeword alignment -> LDPC -> BCH -> descrambler.
// The LDPC decoding runs on worker threads; results come back in order through poll().
#pragma once
#include "dtmb_defs.h"
#include "dtmb_ldpc.h"
#include "dect2/common/ring.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace dect2::dvbs { class ComputeChoice; }

namespace dect2::dtmb {

struct ChainStats {
    uint64_t cwOk = 0, cwBad = 0, cwDropped = 0;   // codewords: decoded, failed (LDPC, BCH or sync bytes), thrown away because the workers were behind
    uint64_t cwSkipped = 0;                        // not tried: after a long run of failures only every eighth word is decoded (keeps the cost of a bad signal low)
    uint64_t packets = 0;                          // transport packets delivered
    uint64_t iterSum = 0;                          // LDPC iterations of the codewords counted in cwOk + cwBad
    uint64_t bchCorrected = 0;                     // BCH blocks with one bit corrected
    double lastIterAvg = 0;
    uint64_t okIterMax = 0, badIterSum = 0;        // slowest word that decoded, iterations spent on words that did not (DTMB_PROFILE)
    double ldpcSec = 0, tsSec = 0;                 // time spent in the LDPC iterations and in BCH, descrambling and packet assembly (DTMB_PROFILE)
    uint64_t gpuWords = 0;                         // codewords whose LDPC ran on the GPU
    bool onGpu = false;                            // the last codewords were decoded on the GPU
};

class FecChain {
public:
    // workers: threads for the LDPC decoder; 0 decodes inside pushFrame (tests)
    FecChain(const Profile& p, Header h, int workers, double symRate = kSymbolRate);
    ~FecChain();
    // Where the LDPC decoding of the worker threads runs: 0 CPU, 1 GPU, 2 auto (the GPU when there is one and it is faster, measured while it runs).
    // The GPU takes several codewords at once. Without workers (tests) the decoding is always on the CPU. The decoded stream is the same on either.
    void setComputeMode(int mode);
    static bool gpuUsable();    // the GPU has a kernel for these codes
    FecChain(const FecChain&) = delete;
    FecChain& operator=(const FecChain&) = delete;
    // Forget the stream: the interleaver, the alignment and the queue (jobs already running finish and are discarded)
    void reset();
    // The 3744 data symbols of a signal frame in transmit order: unit power constellation scale, with the noise variance E|n|^2 of every symbol
    void pushFrame(const cf32* sym, const float* var);
    // A frame went missing: pass erasures through the de-interleaver
    void skipFrame();
    // Delivers finished codewords in order. cb(packets, count, seconds of signal they stand for); only good packets are passed on.
    using PacketCb = std::function<void(const uint8_t* packets, size_t count, double seconds)>;
    void poll(const PacketCb& cb);
    // Waits for the queue to drain (tools and tests)
    void flush(const PacketCb& cb);
    bool aligned() const { return aligned_; }
    int framesUntilPrimed() const { return primed_ > frames_ ? (int)(primed_ - frames_) : 0; }
    ChainStats stats() const;
    // soft output of the last decoded codeword, for the display: mean LLR magnitude
    const Profile& profile() const { return prof_; }

private:
    struct Job { uint64_t seq; std::vector<float> llr; int groupIndex; };
    struct Result { bool ok = false, dropped = false, skipped = false; int iterations = 0, bchCorrected = 0; double ldpcSec = 0, tsSec = 0; std::vector<uint8_t> packets; };
    void llrFrame(const cf32* sym, const float* var, std::vector<float>& out);
    void trySearch();
    void dispatch();
    void submit(Job&& j);
    Result decode(const Job& j, LdpcCode::Decoder& dec, int maxIter) const;
    Result afterLdpc(const Job& j, uint8_t* info, Result r) const;   // BCH, descrambler and packet assembly of a word whose LDPC decoding succeeded
    void decodeBatch(std::vector<Job>& jobs, std::vector<Result>& out, LdpcCode::Decoder& dec, bool gpu);
    void workerLoop();
    void collect(const PacketCb& cb, bool wait);

    Profile prof_;
    const LdpcCode& code_;
    int perFrame_, pkCw_, cwPerGroup_;
    double secsPerCw_;
    std::vector<uint8_t> descr_;                 // descrambler bits of one signal frame's payload
    uint64_t frames_ = 0, primed_ = 0;
    bool aligned_ = false;
    int searchWait_ = 0;                         // frames until the next alignment attempt
    int failRun_ = 0;                            // consecutive failed codewords (receiver thread only)
    std::vector<float> llr_;                     // LLRs of the stream, valid from llr0_
    size_t llr0_ = 0;
    uint64_t cwIndex_ = 0;
    ConvInterleaver<float> bitDe_;               // 4QAM-NR bit de-interleaver
    struct SymVar { cf32 s; float v; };
    ConvInterleaver<SymVar> symDe_;
    LdpcCode::Decoder syncDec_;                  // used by the alignment search and the synchronous mode

    // worker pool
    std::unique_ptr<dvbs::ComputeChoice> compute_;
    std::atomic<bool> gpuBad_{false};            // a GPU call failed: the CPU decodes from then on
    std::atomic<uint64_t> gpuWords_{0};
    int nWorkers_;
    std::vector<std::thread> threads_;
    mutable std::mutex mu_;
    std::condition_variable cv_, doneCv_;
    std::deque<Job> queue_;
    std::map<uint64_t, Result> done_;
    uint64_t nextSeq_ = 0, nextOut_ = 0, generation_ = 0;
    int running_ = 0;
    bool stop_ = false;
    ChainStats st_;
    double pendingSecs_ = 0;
    std::vector<uint8_t> out_;
};

} // namespace dect2::dtmb
