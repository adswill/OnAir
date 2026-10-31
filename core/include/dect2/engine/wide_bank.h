// "Whole span" decoding for the narrowband modes: one single-channel receiver per carrier the finder (carrierfind.h) found in the captured span.
// Every receiver is the mode's ordinary class, fed the carrier's own baseband stream by a Channelizer; no state is shared between them.
//
//   feed(span) -> CarrierFinder -> per carrier: Channelizer -> Rx::feed  (and, while it is recorded, a second Channelizer -> IQ WAV file)
//   snapshot() -> the selected carrier's telemetry, with the log of every carrier (also the ones that have gone) merged in, the carriers
//   table and the bank's status (noise floor, CPU load, the cap)
//
// A carrier found late gets the last preRollSec of the span replayed first (the start of the transmission that made the finder notice it), so
// its decoder locks on the first burst instead of the second. A carrier that drifts is followed (the mixer moves with the finder's centroid);
// a retune of the radio that keeps a carrier inside the span moves it (CarrierFinder::shift), its decoder keeps running.
//
// CPU budget: the time the per-carrier work takes is measured against the time the block covers. Above the budget (a fraction of real time)
// the bank is "too slow": the cap drops by one (the weakest carrier goes, never the one listened to) every 2 s until the load is under the
// budget; under half the budget the cap comes back by one every 5 s (lifted once it no longer limits).
//
// Threads: the analysis thread calls feed(); the per-carrier work (Channelizer + Rx::feed, the expensive part) of the live carriers then runs in
// parallel on the shared pool (ThreadPool::shared().parallelFor, the analysis thread helps and waits). Each slot touches only its own receiver, so
// the output is the same as a sequential loop; the lines the receivers log meanwhile are caught per carrier (WideLogCapture) and passed on in
// carrier order after the join, so the log is deterministic too. DECT2_THREADS=0 runs the plain loop.
//
// Template over the receiver and its telemetry so every mode uses the same machinery; the mode supplies a few hooks (WideOps).
#pragma once
#include "dect2/dsp/carrierfind.h"
#include "dect2/common/threadpool.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

// One row of the carriers table (shared by every mode)
struct WideCarrier {
    int id = 0;
    double hz = 0;               // frequency relative to the centre of the span
    double levelDb = -200;       // dBFS-like level of the cell
    double snrDb = 0;            // above the noise floor
    double widthHz = 0;          // occupied width as measured (0: not yet)
    double ageSec = 0;           // since it was found
    double idleSec = 0;          // since it was last on the air (0 while it is)
    bool active = false;         // above the threshold now (else only held)
    bool selected = false;       // the one that is listened to and shown in detail
    bool prior = false;          // on a known channel (frequency database / band plan)
    bool recording = false;      // its IQ goes to a file (WideBank::startRecording)
    int state = 0;               // 0 searching, 1 partly locked, 2 locked (the decoder's own state)
    std::string info;            // what is known: colour code / RAN / NAC / system / talkgroup ...
};

// The bank as a whole (shown above the carriers table)
struct WideStatus {
    double noiseDb = -200;       // the span's noise floor
    double load = 0;             // the per-carrier work's share of real time (1 = as long as the signal lasts)
    double budget = 0;           // the limit for load (0: none)
    int cap = 0;                 // carriers decoded at most now
    int maxCarriers = 0;         // the cap without the CPU limit
    bool tooSlow = false;        // the CPU limit lowered the cap
    uint64_t droppedForCpu = 0;  // carriers dropped because of it
};

// While a pool job feeds one carrier's receiver, the lines that receiver logs go to this buffer (thread-local) instead of the engine's log;
// WideBank::feed replays them in carrier order. A mode's log callback must route through WideLogCapture::sink() first.
// Process-wide switch (default on) for the parallel per-carrier feed: tests and benchmarks turn it off to compare with the sequential loop.
struct WideParallel {
    static std::atomic<bool>& enabled() { static std::atomic<bool> e{true}; return e; }
};

struct WideLogCapture {
    static std::vector<std::string>*& current() { static thread_local std::vector<std::string>* c = nullptr; return c; }
    // true when the line was captured (the caller must not log it itself)
    static bool sink(const std::string& line) { auto* c = current(); if (!c) return false; c->push_back(line); return true; }
};

// The nearest channel centre of the raster origin + k * step. A signal a few kHz off its channel (transmitter drift, a rig a little off frequency)
// is still that channel; one further than a quarter of the spacing from every centre sits on another raster (8.33 kHz in 25, a different offset)
// and keeps its measured frequency.
inline double snapToRaster(double hz, double origin, double step) {
    if (!(step > 0)) return hz;
    const double c = origin + std::round((hz - origin) / step) * step;
    return std::fabs(hz - c) <= 0.25 * step ? c : hz;
}

template <class Rx, class Tel>
struct WideOps {
    double outRate = 48000;                                  // the rate the receiver wants
    double occupiedHz = 9000;                                // bandwidth the channel filter keeps
    std::function<void(const std::string&)> log;             // optional: where the captured log lines go after a parallel feed (the same sink the receivers' callback uses)
    std::function<void(Rx&)> setup;                          // after configure: silent, no offset, log callback
    std::function<void(Rx&, bool play)> audio;               // play = this is the listened-to carrier
    std::function<void(Rx&)> clear;                          // clearResults
    std::function<void(Tel& t)> clearLists;                  // empty the call / message lists of t
    std::function<void(Tel& dst, const Tel& src, double hz)> append;   // append src's list entries (stamped with the carrier frequency hz) to dst
    std::function<std::string(const Tel&)> info;             // short text for the table
    double preRollSec = 0;                                   // > 0: a carrier found late gets this much of the span from before it was found (the start of the transmission)
    std::function<double(const Tel&)> freqCorrection;        // optional: Hz to add to the finder's frequency of the carrier (the decoder's own carrier-offset measurement)
    std::function<bool(const Tel&)> hasLog;                  // optional: false = this telemetry has no list entries, a carrier that goes is not kept in the log
    std::function<int(const Tel&)> rowState;                 // optional: the table's state (0 searching, 1 partly locked, 2 locked) when the telemetry's own state is not that scale
    std::function<bool(const Tel&)> voice;                   // optional: a voice call is on this carrier now (the sound goes to a call before a carrier that is only locked)
    std::function<void(Rx&, double absHz)> bind;             // optional: a receiver was made for a carrier at this absolute frequency (the span's centre set by setCenterHz plus the carrier's offset; the offset alone while no centre is known)
};

// Minimal IQ WAV writer (2 channels of 32-bit float, the header's sizes patched on close): SDR programs open these as IQ recordings
struct WideIqWav {
    FILE* f = nullptr;
    uint64_t frames = 0;
    double rate = 0;
    bool open(const std::string& path, double r) {
        close();
        f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        rate = r; frames = 0;
        header();
        return true;
    }
    void write(const cf32* x, size_t n) { if (f && n) { std::fwrite(x, sizeof(cf32), n, f); frames += n; } }
    void close() { if (!f) return; std::fseek(f, 0, SEEK_SET); header(); std::fclose(f); f = nullptr; }
    ~WideIqWav() { close(); }
private:
    void u32(uint32_t v) { unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)}; std::fwrite(b, 1, 4, f); }
    void u16(uint16_t v) { unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)}; std::fwrite(b, 1, 2, f); }
    void header() {
        const uint64_t bytes = frames * 8;
        const uint32_t data = bytes > 0xFFFFFFF0ull ? 0xFFFFFFF0u : (uint32_t)bytes;
        std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16); u16(3); u16(2); u32((uint32_t)std::lround(rate)); u32((uint32_t)std::lround(rate) * 8); u16(8); u16(32);
        std::fwrite("data", 1, 4, f); u32(data);
    }
};

template <class Rx, class Tel>
class WideBank {
public:
    WideBank(WideOps<Rx, Tel> ops) : ops_(std::move(ops)) {}

    // spanRate: the sample rate of what feed() gets; the raster cfg is the mode's
    void configure(double spanRate, const CarrierCfg& cfg) {
        std::lock_guard<std::mutex> g(m_);
        spanRate_ = spanRate; cfg_ = cfg;
        if (thrSet_) { cfg_.onDb = thr_[0]; cfg_.offDb = std::min(thr_[1], thr_[0] - 0.5); cfg_.minSnrDb = thr_[2]; cfg_.debounceSec = thr_[3]; cfg_.holdSec = thr_[4]; }
        if (maxSet_ > 0) cfg_.maxCarriers = maxSet_;
        finder_.configure(spanRate, cfg_);
        applyPriors();
        hist_.clear();
        live_.clear(); retired_.clear(); sel_ = 0; seq_++;
        loadEma_ = 0; tooSlow_ = false; capChangeAt_ = clock_ = 0;
    }
    bool ready() const { return spanRate_ >= ops_.outRate; }
    void reset() { std::lock_guard<std::mutex> g(m_); finder_.reset(); hist_.clear(); retireAll(); sel_ = 0; seq_++; }   // a fresh start of the mode: the carriers are new, the logs stay
    // The radio's centre frequency (any thread; takes effect with the next feed, on the analysis thread). A retune that keeps a carrier inside the
    // span moves it (same id, same decoder); a carrier that has gone keeps its place in the log by its absolute frequency.
    void setCenterHz(double hz) { pendCenter_.store(hz); }
    // runs f on every live receiver (a setting that changes while carriers are tracked)
    void forEach(const std::function<void(Rx&)>& f) { std::lock_guard<std::mutex> g(m_); for (auto& s : live_) f(*s->rx); }
    void clearResults() {
        std::lock_guard<std::mutex> g(m_);
        retired_.clear();
        for (auto& s : live_) { if (ops_.clear) ops_.clear(*s->rx); s->tel = Tel(); s->telSeq = 0; }
        seq_++;
    }
    void setSelected(int id) { std::lock_guard<std::mutex> g(m_); sel_ = id; applyAudio(); seq_++; }
    void setMuted(bool m) { std::lock_guard<std::mutex> g(m_); muted_ = m; applyAudio(); }
    int selected() const { std::lock_guard<std::mutex> g(m_); return sel_; }
    size_t liveCount() const { std::lock_guard<std::mutex> g(m_); return live_.size(); }

    // ---- settings (any thread)
    // the finder's thresholds: on / off above the floor (hysteresis), the SNR a new carrier must reach, its dwell time, the hold time
    void setThresholds(double onDb, double offDb, double minSnrDb, double dwellSec, double holdSec) {
        std::lock_guard<std::mutex> g(m_);
        thr_[0] = onDb; thr_[1] = offDb; thr_[2] = minSnrDb; thr_[3] = dwellSec; thr_[4] = holdSec; thrSet_ = true;
        cfg_.onDb = onDb; cfg_.offDb = std::min(offDb, onDb - 0.5); cfg_.minSnrDb = minSnrDb; cfg_.debounceSec = dwellSec; cfg_.holdSec = holdSec;
        finder_.setThresholds(onDb, offDb, minSnrDb, dwellSec, holdSec);
    }
    // the hard cap on carriers decoded at once (the weakest go at the finder's next update)
    void setMaxCarriers(int n) {
        std::lock_guard<std::mutex> g(m_);
        maxSet_ = std::max(1, n);
        cfg_.maxCarriers = maxSet_; finder_.setMaxCarriers(maxSet_);
    }
    // the CPU limit as a share of real time (0: none, the default; the engine sets 0.8)
    void setCpuBudget(double frac) { std::lock_guard<std::mutex> g(m_); budget_ = std::max(0.0, frac); if (budget_ == 0) { finder_.setCap(0); tooSlow_ = false; } }
    // known channels (absolute Hz: the frequency database and the band plan of the mode); they are found at a lower threshold
    void setPriorsAbs(std::vector<double> absHz, bool only = false) { std::lock_guard<std::mutex> g(m_); priorsAbs_ = std::move(absHz); priorOnly_ = only; applyPriors(); }
    // The carriers that are not within 0.6 raster steps of one of these absolute frequencies are let go now (the finder forgets them too): a channel
    // taken off a follow list (the wide session) stops at once instead of when it falls silent.
    void releaseOutside(const std::vector<double>& absHz) {
        std::lock_guard<std::mutex> g(m_);
        std::vector<int> ids;
        for (auto& s : live_) {
            bool keep = false;
            for (double f : absHz) if (std::fabs(f - (center_ + s->car.hz)) < 0.6 * cfg_.stepHz) keep = true;
            if (!keep) ids.push_back(s->car.id);
        }
        for (int id : ids) { finder_.forget(id); retire(id); }
    }
    // test hook: the load as if measured (< 0: measure)
    void setLoadOverride(double v) { std::lock_guard<std::mutex> g(m_); loadOverride_ = v; }

    // ---- recording: the carrier's IQ (a second channel filter, 4 samples per raster step) to an IQ WAV file in dir; returns the path ("" on failure)
    std::string startRecording(int id, const std::string& dir) {
        std::lock_guard<std::mutex> g(m_);
        Slot* s = find(id);
        if (!s || spanRate_ <= 0) return std::string();
        const double rr = std::min(spanRate_, 4.0 * cfg_.stepHz);
        if (!s->recCh.configure(spanRate_, rr, s->car.hz, 0.9 * cfg_.stepHz)) return std::string();
        char name[160];
        const std::time_t now = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        snprintf(name, sizeof name, "carrier_%.5fMHz_%dsps_%04d%02d%02d-%02d%02d%02d.wav", (center_ + s->car.hz) / 1e6, (int)std::lround(rr),
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        std::string path = dir.empty() ? std::string(name) : dir + ((dir.back() == '/' || dir.back() == '\\') ? "" : "/") + name;
        if (!s->rec.open(path, rr)) return std::string();
        s->recPath = path; seq_++;
        return path;
    }
    void stopRecording(int id) { std::lock_guard<std::mutex> g(m_); if (Slot* s = find(id)) { s->rec.close(); s->recPath.clear(); seq_++; } }
    std::string recordingPath(int id) const { std::lock_guard<std::mutex> g(m_); for (auto& s : live_) if (s->car.id == id) return s->recPath; return std::string(); }

    void feed(const cf32* x, size_t n) {
        std::lock_guard<std::mutex> g(m_);
        if (spanRate_ <= 0) return;
        const double pc = pendCenter_.load();
        if (pc != center_) {
            const double d = pc - center_;
            if (centerSet_ && std::fabs(d) < 0.45 * spanRate_ && !live_.empty()) {   // the carriers still inside the span move with the retune
                finder_.shift(d);
                for (int id : finder_.takeRemoved()) retire(id);
                for (auto& s : live_) { s->car.hz -= d; s->ch.setShift(s->car.hz); s->recCh.setShift(s->car.hz); }
            } else { retireAll(); finder_.reset(); sel_ = 0; }
            hist_.clear(); center_ = pc; centerSet_ = true; applyPriors(); seq_++;
        }
        finder_.setKeep(effSel());
        finder_.feed(x, n);
        for (int id : finder_.takeRemoved()) retire(id);
        const double follow = 0.06 * cfg_.stepHz;   // a drifting carrier: the mixer follows it in steps (the decoder's own AFC takes the rest)
        for (const Carrier& c : finder_.carriers()) {
            Slot* s = find(c.id);
            if (!s) s = add(c);
            if (s) {
                s->car = c;
                if (std::fabs(c.hz - s->ch.shiftHz()) > follow) { s->ch.setShift(c.hz); s->recCh.setShift(c.hz); }
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        feedCarriers(x, n);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        budgetStep(el, (double)n / spanRate_);
        if (ops_.preRollSec > 0) {   // keep the last preRollSec of the span (before this block) for a carrier found later
            const size_t cap = histCap();
            hist_.insert(hist_.end(), x, x + n);
            if (hist_.size() > 2 * cap) hist_.erase(hist_.begin(), hist_.end() - (long)cap);   // trimmed in steps: a wide span would otherwise move the whole history on every block
        }
    }

    // The selected carrier's telemetry (the strongest locked one when none is chosen), the logs of all merged in, and the table.
    // Returns false when nothing changed since lastSeq.
    bool snapshot(Tel& primary, std::vector<WideCarrier>& rows, uint64_t lastSeq, uint64_t* seqOut, WideStatus* st = nullptr) {
        std::lock_guard<std::mutex> g(m_);
        bool changed = false;
        for (auto& s : live_) { Tel t; if (s->rx->telemetry(t, s->telSeq)) { s->telSeq = t.seq; s->tel = std::move(t); changed = true; } }
        if (changed) seq_++;
        if (st) {
            st->noiseDb = finder_.noiseDb(); st->load = loadEma_; st->budget = budget_; st->cap = finder_.cap(); st->maxCarriers = cfg_.maxCarriers;
            st->tooSlow = tooSlow_; st->droppedForCpu = droppedCpu_;
        }
        if (seq_ == lastSeq) return false;
        *seqOut = seq_;
        rows.clear();
        Slot* best = nullptr;
        for (auto& s : live_) {
            WideCarrier r; r.id = s->car.id; r.hz = hzOf(*s); r.levelDb = s->car.levelDb; r.snrDb = s->car.snrDb; r.active = s->car.active;
            r.widthHz = s->car.widthHz; r.ageSec = s->car.ageSec; r.idleSec = s->car.idleSec; r.prior = s->car.prior; r.recording = s->rec.f != nullptr;
            r.state = ops_.rowState ? ops_.rowState(s->tel) : s->tel.state; r.info = ops_.info ? ops_.info(s->tel) : std::string();
            rows.push_back(r);
            if (s->car.id == sel_) best = s.get();
        }
        if (!best) {   // none chosen: the best locked one, else the strongest
            for (auto& s : live_) {
                auto rank = [](const Slot& q) { return q.tel.state * 1000.0 + q.car.levelDb; };
                if (!best || rank(*s) > rank(*best)) best = s.get();
            }
        }
        if (sel_ == 0 && ops_.audio) {   // none chosen: the sound follows the best locked carrier too (it used to stay silent until a row was clicked); it stays on its carrier while that is as good as the best
            Slot* cur = nullptr;
            for (auto& s : live_) if (s->car.id == autoPlay_) cur = s.get();
            // a call wins over a carrier that is only locked (a control channel is locked all the time and would keep the sound for ever); the call
            // being played keeps it until it ends, then the strongest other call takes it
            auto talking = [&](const Slot& q) { return ops_.voice && ops_.voice(q.tel); };
            Slot* call = cur && talking(*cur) ? cur : nullptr;
            if (!call) for (auto& s : live_) if (talking(*s) && (!call || s->car.levelDb > call->car.levelDb)) call = s.get();
            const int want = call ? call->car.id : (cur && best && cur->tel.state >= best->tel.state) ? autoPlay_ : (best ? best->car.id : 0);
            if (want != autoPlay_) { autoPlay_ = want; applyAudio(); }
        } else if (autoPlay_ != 0) autoPlay_ = 0;
        Tel p = best ? best->tel : Tel();
        // the logs: carriers that have gone first, then the live ones (each list keeps its own order)
        if (ops_.append) {
            if (ops_.clearLists) ops_.clearLists(p);
            for (auto& r : retired_) ops_.append(p, r.tel, r.hz - center_);
            for (auto& s : live_) ops_.append(p, s->tel, hzOf(*s));
        }
        p.seq = seq_;
        primary = std::move(p);
        for (auto& r : rows) r.selected = best && r.id == best->car.id;
        return true;
    }

    const CarrierFinder& finder() const { return finder_; }
    // test hooks: the receivers' frequencies in the span, the ids of the live carriers, and the status
    std::vector<double> liveHz() const { std::lock_guard<std::mutex> g(m_); std::vector<double> v; for (auto& s : live_) v.push_back(s->car.hz); return v; }
    std::vector<int> liveIds() const { std::lock_guard<std::mutex> g(m_); std::vector<int> v; for (auto& s : live_) v.push_back(s->car.id); return v; }
    WideStatus status() const {
        std::lock_guard<std::mutex> g(m_);
        WideStatus st; st.noiseDb = finder_.noiseDb(); st.load = loadEma_; st.budget = budget_; st.cap = finder_.cap(); st.maxCarriers = cfg_.maxCarriers;
        st.tooSlow = tooSlow_; st.droppedForCpu = droppedCpu_;
        return st;
    }
    // test hook: the channel mixer frequency of a carrier's receiver (NaN when it is not live)
    double mixerHz(int id) const { std::lock_guard<std::mutex> g(m_); for (auto& s : live_) if (s->car.id == id) return s->ch.shiftHz(); return std::nan(""); }

private:
    struct Slot {
        Carrier car; std::unique_ptr<Rx> rx; Channelizer ch; Tel tel; uint64_t telSeq = 0; std::vector<cf32> scratch; std::vector<std::string> log;
        Channelizer recCh; WideIqWav rec; std::vector<cf32> recBuf; std::string recPath;
    };
    struct Retired { int id; double hz; Tel tel; };   // hz: relative to the span centre plus the centre of that time (absolute)
    size_t histCap() const { return (size_t)std::min(ops_.preRollSec * spanRate_, (double)(1u << 21)); }   // at most 2 M samples (16 MB)
    void feedSlot(Slot& s, const cf32* x, size_t n, std::vector<cf32>& out) {
        out.clear();
        s.ch.process(x, n, out);
        if (!out.empty()) s.rx->feed(out.data(), out.size());
        if (s.rec.f) { s.recBuf.clear(); s.recCh.process(x, n, s.recBuf); s.rec.write(s.recBuf.data(), s.recBuf.size()); }
    }
    // Channelizer + receiver of every live carrier. Two or more carriers: in parallel on the shared pool, each slot with its own scratch and log buffer.
    void feedCarriers(const cf32* x, size_t n) {
        ThreadPool& pool = ThreadPool::shared();
        if (live_.size() < 2 || pool.workers() == 0 || n < 4096 || !WideParallel::enabled().load(std::memory_order_relaxed)) {
            std::vector<cf32> out;
            for (auto& s : live_) feedSlot(*s, x, n, out);
            return;
        }
        pool.parallelFor(live_.size(), [&](size_t i) {
            Slot& s = *live_[i];
            s.log.clear();
            WideLogCapture::current() = &s.log;
            feedSlot(s, x, n, s.scratch);
            WideLogCapture::current() = nullptr;
        });
        if (ops_.log) for (auto& s : live_) for (const std::string& l : s->log) ops_.log(l);
    }
    // the CPU budget: el seconds of work for blk seconds of signal
    void budgetStep(double el, double blk) {
        if (blk <= 0) return;
        clock_ += blk;
        const double inst = loadOverride_ >= 0 ? loadOverride_ : (live_.empty() ? 0.0 : el / blk);
        const double a = std::min(1.0, blk / 1.0);
        loadEma_ += a * (inst - loadEma_);
        if (budget_ <= 0) return;
        if (loadEma_ > budget_ && live_.size() > 1 && clock_ - capChangeAt_ >= 2.0) {
            const int cap = std::max(1, (int)live_.size() - 1);
            finder_.setCap(cap); tooSlow_ = true; droppedCpu_++; capChangeAt_ = clock_;
            char b[120]; snprintf(b, sizeof b, "whole span: too slow (load %.0f %% of real time), decoding at most %d carriers", loadEma_ * 100, cap);
            if (ops_.log) ops_.log(b);
        } else if (tooSlow_ && loadEma_ < 0.5 * budget_ && clock_ - capChangeAt_ >= 5.0) {
            const int cap = finder_.cap() + 1;
            capChangeAt_ = clock_;
            if (cap >= cfg_.maxCarriers || cap > (int)live_.size()) { finder_.setCap(0); tooSlow_ = false; } else finder_.setCap(cap);   // the cap no longer limits: lifted
        }
    }
    void applyPriors() {
        std::vector<double> rel; rel.reserve(priorsAbs_.size());
        for (double f : priorsAbs_) rel.push_back(f - center_);
        finder_.setPriors(rel, priorOnly_);
    }
    Slot* find(int id) { for (auto& s : live_) if (s->car.id == id) return s.get(); return nullptr; }
    Slot* add(const Carrier& c) {
        auto s = std::make_unique<Slot>();
        s->car = c;
        s->rx.reset(new Rx());
        s->rx->configure(ops_.outRate);
        if (ops_.setup) ops_.setup(*s->rx);
        if (ops_.bind) ops_.bind(*s->rx, center_ + c.hz);
        if (!s->ch.configure(spanRate_, ops_.outRate, c.hz, ops_.occupiedHz)) return nullptr;
        if (ops_.audio) ops_.audio(*s->rx, !muted_ && c.id == effSel());
        if (!hist_.empty()) {   // the start of the transmission that made the finder notice the carrier
            const size_t k = std::min(hist_.size(), histCap());
            std::vector<cf32> out;
            s->ch.process(hist_.data() + (hist_.size() - k), k, out);
            if (!out.empty()) s->rx->feed(out.data(), out.size());
        }
        live_.push_back(std::move(s));
        seq_++;
        return live_.back().get();
    }
    void retire(int id) {
        for (size_t i = 0; i < live_.size(); i++) if (live_[i]->car.id == id) {
            Tel t; if (live_[i]->rx->telemetry(t, 0)) live_[i]->tel = std::move(t);
            live_[i]->rec.close();
            if (!ops_.hasLog || ops_.hasLog(live_[i]->tel)) retired_.push_back(Retired{id, hzOf(*live_[i]) + center_, std::move(live_[i]->tel)});
            if (retired_.size() > 64) retired_.pop_front();
            if (sel_ == id) sel_ = 0;
            live_.erase(live_.begin() + (long)i); seq_++; return;
        }
    }
    double hzOf(const Slot& s) const {   // the finder's frequency, refined by what the decoder measured on the carrier
        const double c = ops_.freqCorrection ? ops_.freqCorrection(s.tel) : 0.0;
        const double hz = s.car.hz + (std::fabs(c) < 0.3 * cfg_.stepHz ? c : 0.0);
        return cfg_.snapReported ? snapToRaster(hz, cfg_.gridOriginHz, cfg_.stepHz) : hz;
    }
    void retireAll() { while (!live_.empty()) retire(live_.back()->car.id); }
    void applyAudio() { if (!ops_.audio) return; for (auto& s : live_) ops_.audio(*s->rx, !muted_ && s->car.id == effSel()); }
    int effSel() const { return sel_ ? sel_ : autoPlay_; }   // the carrier whose sound plays: the chosen one, else the best locked one
    WideOps<Rx, Tel> ops_;
    mutable std::mutex m_;
    double spanRate_ = 0;
    CarrierCfg cfg_;
    CarrierFinder finder_;
    std::vector<std::unique_ptr<Slot>> live_;
    std::deque<Retired> retired_;
    int sel_ = 0;
    int autoPlay_ = 0;   // the carrier that plays while none is chosen (sel_ == 0)
    bool muted_ = false;
    uint64_t seq_ = 1;
    std::vector<cf32> hist_;
    double center_ = 0;
    bool centerSet_ = false;
    std::atomic<double> pendCenter_{0};
    // settings kept over a configure
    double thr_[5] = {9, 5, 0, 0.3, 8};
    bool thrSet_ = false;
    int maxSet_ = 0;
    std::vector<double> priorsAbs_;
    bool priorOnly_ = false;
    // CPU budget
    double budget_ = 0, loadEma_ = 0, loadOverride_ = -1, clock_ = 0, capChangeAt_ = 0;
    bool tooSlow_ = false;
    uint64_t droppedCpu_ = 0;
};

} // namespace dect2
