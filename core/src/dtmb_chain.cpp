// DTMB channel decoder (see dtmb_chain.h).
#include "dect2/dtmb_chain.h"
#include "dect2/dtmb_map.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::dtmb {

namespace {
constexpr size_t kQueueLimit = 256;   // codewords waiting for a worker before old ones are dropped (about 50 ms of the fastest modes, 8 MB)
constexpr int kMaxIter = 60;
constexpr int kSearchIter = 20;      // alignment attempts (32QAM, 4QAM-NR) give up sooner
constexpr int kSearchEvery = 16;     // frames between alignment attempts that failed
constexpr int kThrottleAfter = 24;   // failed codewords in a row before the decoder only probes
constexpr int kProbeEvery = 8;
}

FecChain::FecChain(const Profile& p, Header h, int workers, double symRate)
    : prof_(p), code_(ldpcCode(p.rate)), perFrame_(packetsPerFrame(p)), pkCw_(payloadBits(p.rate) / kTsBits), cwPerGroup_(codewordsPerGroup(p.map)),
      bitDe_(interleaverDelay(p), true), symDe_(interleaverDelay(p), true), syncDec_(code_), nWorkers_(workers) {
    secsPerCw_ = frameSeconds(h, symRate) * framesPerGroup(p.map) / cwPerGroup_;
    // descrambler bits of the payload of one signal frame (the generator restarts with every frame)
    Scrambler s;
    descr_.resize((size_t)perFrame_ * kTsBits);
    for (auto& b : descr_) b = s.next();
    primed_ = (symDe_.totalDelay() + kDataSymbols - 1) / kDataSymbols;
    out_.reserve(16 * 188);
    for (int i = 0; i < nWorkers_; i++) threads_.emplace_back([this] { workerLoop(); });
    reset();
}

FecChain::~FecChain() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void FecChain::reset() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.clear();
        done_.clear();
        generation_++;
        nextOut_ = nextSeq_;
        pendingSecs_ = 0;
    }
    SymVar z{cf32(0, 0), 1e30f};
    symDe_.reset(z);
    bitDe_.reset(0.f);
    frames_ = 0;
    aligned_ = false;
    searchWait_ = 0; failRun_ = 0;
    llr_.clear(); llr0_ = 0;
    cwIndex_ = 0;
}

ChainStats FecChain::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

// ---------------------------------------------------------------- symbols -> LLRs of the stream
void FecChain::llrFrame(const cf32* sym, const float* var, std::vector<float>& out) {
    const Mapping m = prof_.map;
    if (m == Mapping::Qam4Nr) {
        // 4QAM LLRs, eight symbols = 16 LLRs -> NR decoder -> 8 LLRs, then the bit de-interleaver
        std::vector<float> raw(2 * (size_t)kDataSymbols), info((size_t)kDataSymbols), deint((size_t)kDataSymbols);
        demapBlock(Mapping::Qam4, sym, var, (size_t)kDataSymbols, raw.data());
        for (int g = 0; g < kDataSymbols / 8; g++) nrSoftDecode(&raw[(size_t)g * 16], &info[(size_t)g * 8]);
        bitDe_.process(info.data(), deint.data(), info.size());
        out.insert(out.end(), deint.begin(), deint.end());
        return;
    }
    std::vector<SymVar> in((size_t)kDataSymbols), de((size_t)kDataSymbols);
    for (int i = 0; i < kDataSymbols; i++) in[(size_t)i] = SymVar{sym[i], var[i]};
    symDe_.process(in.data(), de.data(), in.size());
    std::vector<cf32> xs((size_t)kDataSymbols);
    std::vector<float> vs((size_t)kDataSymbols);
    for (int i = 0; i < kDataSymbols; i++) { xs[(size_t)i] = de[(size_t)i].s; vs[(size_t)i] = de[(size_t)i].v; }
    const int bps = bitsPerSymbol(m);
    const size_t at = out.size();
    out.resize(at + (size_t)kDataSymbols * (size_t)bps);
    demapBlock(m, xs.data(), vs.data(), (size_t)kDataSymbols, &out[at]);
}

void FecChain::pushFrame(const cf32* sym, const float* var) {
    std::vector<float> l;
    llrFrame(sym, var, l);
    frames_++;
    if (frames_ <= primed_) return;   // the de-interleaver still holds its start-up zeros
    if (llr0_ > 0 && llr0_ > llr_.size() / 2) { llr_.erase(llr_.begin(), llr_.begin() + (long)llr0_); llr0_ = 0; }
    llr_.insert(llr_.end(), l.begin(), l.end());
    if (!aligned_) trySearch();
    if (aligned_) dispatch();
}

void FecChain::skipFrame() {
    std::vector<cf32> s((size_t)kDataSymbols, cf32(0, 0));
    std::vector<float> v((size_t)kDataSymbols, 1e30f);
    // erased symbols keep the interleaver in step; the codewords that depend on them will fail
    std::vector<float> l;
    llrFrame(s.data(), v.data(), l);
    frames_++;
    if (frames_ <= primed_) return;
    llr_.insert(llr_.end(), l.begin(), l.end());
    if (aligned_) dispatch();
}

// ---------------------------------------------------------------- alignment of the codewords to the frames
// 4QAM, 16QAM, 64QAM: a codeword boundary is a frame boundary from the start. 4QAM-NR and 32QAM: a group of two frames holds whole codewords,
// and which frame starts a group is found by decoding at both positions.
void FecChain::trySearch() {
    const size_t frameBits = prof_.map == Mapping::Qam4Nr ? (size_t)kDataSymbols : (size_t)kDataSymbols * (size_t)bitsPerSymbol(prof_.map);
    if (framesPerGroup(prof_.map) == 1) { aligned_ = true; return; }
    const size_t have = llr_.size() - llr0_;
    if (have < frameBits + (size_t)kLdpcSent) return;
    if (searchWait_ > 0) {   // a failed attempt: skip a few frames, the two phases are tried again on fresh data
        searchWait_--;
        llr0_ += frameBits;
        return;
    }
    searchWait_ = kSearchEvery;
    for (size_t phase = 0; phase < 2; phase++) {
        const size_t off = llr0_ + phase * frameBits;
        if (llr_.size() < off + (size_t)kLdpcSent) continue;
        Job j{0, std::vector<float>(llr_.begin() + (long)off, llr_.begin() + (long)off + kLdpcSent), 0};
        const Result r = decode(j, syncDec_, kSearchIter);
        if (r.ok) {
            llr0_ = off;
            aligned_ = true;
            cwIndex_ = 0;
            return;
        }
    }
    // not aligned yet: slide by one frame
    llr0_ += frameBits;
}

// ---------------------------------------------------------------- codewords
void FecChain::dispatch() {
    while (llr_.size() - llr0_ >= (size_t)kLdpcSent) {
        Job j;
        const uint64_t idx = cwIndex_++;
        if (failRun_ >= kThrottleAfter && idx % kProbeEvery != 0) {
            // nothing has decoded for a while: this word is not tried
            llr0_ += kLdpcSent;
            Result r; r.skipped = true;
            std::lock_guard<std::mutex> lk(mu_);
            done_[nextSeq_++] = std::move(r);
            continue;
        }
        j.llr.assign(llr_.begin() + (long)llr0_, llr_.begin() + (long)llr0_ + kLdpcSent);
        llr0_ += kLdpcSent;
        j.groupIndex = (int)(idx % (uint64_t)cwPerGroup_);
        submit(std::move(j));
    }
}

void FecChain::submit(Job&& j) {
    if (nWorkers_ == 0) {
        j.seq = nextSeq_++;
        Result r = decode(j, syncDec_, kMaxIter);
        std::lock_guard<std::mutex> lk(mu_);
        done_[j.seq] = std::move(r);
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    j.seq = nextSeq_++;
    if (queue_.size() >= kQueueLimit) {
        // the decoders are behind: the oldest waiting word goes, the newest is the most useful
        Result r; r.dropped = true;
        done_[queue_.front().seq] = std::move(r);
        queue_.pop_front();
    }
    queue_.push_back(std::move(j));
    cv_.notify_one();
}

FecChain::Result FecChain::decode(const Job& j, LdpcCode::Decoder& dec, int maxIter) const {
    Result r;
    std::vector<uint8_t> info((size_t)code_.infoBits());
    const LdpcCode::Result lr = dec.decode(j.llr.data(), info.data(), maxIter);
    r.iterations = lr.iterations;
    if (!lr.ok) return r;
    const int nb = bchBlocks(prof_.rate);
    std::vector<uint8_t> bits((size_t)nb * kBchK);
    for (int b = 0; b < nb; b++) {
        const int s = bchDecode(&info[(size_t)b * kBchN]);
        if (s < 0) return r;
        r.bchCorrected += s;
        std::memcpy(&bits[(size_t)b * kBchK], &info[(size_t)b * kBchN], kBchK);
    }
    r.packets.resize((size_t)pkCw_ * 188);
    for (int k = 0; k < pkCw_; k++) {
        const int gi = j.groupIndex * pkCw_ + k;
        const size_t base = (size_t)(gi % perFrame_) * kTsBits;
        uint8_t* p = &r.packets[(size_t)k * 188];
        for (int i = 0; i < 188; i++) {
            int v = 0;
            for (int b = 0; b < 8; b++) v = (v << 1) | (bits[(size_t)(k * kTsBits + i * 8 + b)] ^ descr_[base + (size_t)(i * 8 + b)]);
            p[i] = (uint8_t)v;
        }
        if (p[0] != 0x47) return r;
    }
    r.ok = true;
    return r;
}

void FecChain::workerLoop() {
    LdpcCode::Decoder dec(code_);
    for (;;) {
        Job j;
        uint64_t gen;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            j = std::move(queue_.front());
            queue_.pop_front();
            gen = generation_;
            running_++;
        }
        Result r = decode(j, dec, kMaxIter);
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_--;
            if (gen == generation_) done_[j.seq] = std::move(r);
        }
        doneCv_.notify_all();
    }
}

void FecChain::collect(const PacketCb& cb, bool wait) {
    for (;;) {
        Result r;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (wait) doneCv_.wait(lk, [this] { return done_.count(nextOut_) || (queue_.empty() && running_ == 0 && nextOut_ >= nextSeq_); });
            auto it = done_.find(nextOut_);
            if (it == done_.end()) return;
            r = std::move(it->second);
            done_.erase(it);
            nextOut_++;
            if (r.dropped) st_.cwDropped++;
            else if (r.skipped) st_.cwSkipped++;
            else if (r.ok) { st_.cwOk++; st_.packets += (uint64_t)pkCw_; st_.iterSum += (uint64_t)r.iterations; failRun_ = 0; }
            else { st_.cwBad++; st_.iterSum += (uint64_t)r.iterations; failRun_++; }
            st_.bchCorrected += (uint64_t)r.bchCorrected;
            if (!r.dropped && !r.skipped) st_.lastIterAvg = 0.9 * st_.lastIterAvg + 0.1 * r.iterations;
        }
        pendingSecs_ += secsPerCw_;
        if (r.ok) {
            cb(r.packets.data(), r.packets.size() / 188, pendingSecs_);
            pendingSecs_ = 0;
        }
    }
}

void FecChain::poll(const PacketCb& cb) { collect(cb, false); }

void FecChain::flush(const PacketCb& cb) {
    collect(cb, true);
}

} // namespace dect2::dtmb
