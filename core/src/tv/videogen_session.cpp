// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Digital video generator program's engine: sources, mux and preview on their own threads (videogen_session.h, #458).
#include "dect2/tv/videogen_session.h"
#include "dect2/common/cpu_meter.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/u8file.h"
#include "dect2/tv/ts_remux.h"
#include "dect2/tv/tsout.h"
#include "dect2/tv/vglink.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace dect2::vgen {

// ---- TsQueue ----------------------------------------------------------------------------------------------------------------

void TsQueue::push(const uint8_t* packets, size_t n, bool blocking) {
    std::unique_lock<std::mutex> lk(mu_);
    for (size_t i = 0; i < n; i++) {
        if (q_.size() >= limit_) {
            if (blocking) {
                cv_.wait(lk, [&] { return q_.size() < limit_ || closed_; });
                if (closed_) return;
            } else {
                q_.pop_front();   // the oldest goes: the mux was behind (#494)
                dropped_++;
            }
        }
        std::array<uint8_t, 188> pk;
        std::copy_n(packets + i * 188, 188, pk.begin());
        q_.push_back(pk);
        pushed_++;
    }
}

bool TsQueue::pop(uint8_t* pk) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    std::copy(q_.front().begin(), q_.front().end(), pk);
    q_.pop_front();
    cv_.notify_all();
    return true;
}

size_t TsQueue::size() const { std::lock_guard<std::mutex> lk(mu_); return q_.size(); }
void TsQueue::clear() { std::lock_guard<std::mutex> lk(mu_); q_.clear(); cv_.notify_all(); }
void TsQueue::close() { std::lock_guard<std::mutex> lk(mu_); closed_ = true; cv_.notify_all(); }
void TsQueue::reopen() { std::lock_guard<std::mutex> lk(mu_); closed_ = false; dropped_ = 0; pushed_ = 0; }

double muxRateFor(const MuxConfig& m, const std::vector<double>& sourceBps) {
    double sum = 0;
    for (double b : sourceBps) sum += std::max(0.0, b);
    if (m.padding && m.channelBps > 0) return m.channelBps;
    const double need = sum * 1.02 + 20000;   // the sources, a little more so their queues stay short, and room for the PAT / SDT
    return m.channelBps > 0 ? std::min(need, m.channelBps) : need;
}

// ---- Session ----------------------------------------------------------------------------------------------------------------

namespace {
double cpuPercent(uint64_t& lastNs, std::chrono::steady_clock::time_point& lastT) {
    const auto now = std::chrono::steady_clock::now();
    const uint64_t ns = cpu::threadNs();
    const double wall = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastT).count();
    const double p = wall > 0 ? 100.0 * (double)(ns - lastNs) / wall : 0;
    lastNs = ns; lastT = now;
    return p;
}
} // namespace

struct Session::Impl {
    struct Slot {
        VideoGenerator gen;
        std::shared_ptr<TsQueue> q = std::make_shared<TsQueue>();
        bool previewOnly = false;
    };
    std::array<Slot, kMaxSources> slots;
    mutable std::mutex mu;          // the status below
    // mux
    std::thread muxTh;
    std::atomic<bool> muxStop{false};
    std::atomic<bool> muxActive{false};
    MuxConfig mcfg;
    std::vector<MuxNumbering> numbering;
    MuxStatus mst;
    std::atomic<double> channelBps{0};
    // preview
    std::thread prevTh;
    std::atomic<bool> prevStop{false};
    std::atomic<int> prevSrc{0};
    std::vector<uint8_t> prevRgba;
    int prevW = 0, prevH = 0;
    uint64_t prevSerial = 0;
    bool prevTaken = true;
    PreviewStatus pst;

    void failMux(const std::string& e) { std::lock_guard<std::mutex> lk(mu); if (mst.error.empty()) mst.error = e; }
    GenConfig sourceConfig(int k, const GenConfig& c, bool file) {
        GenConfig r = c;
        r.output = Output::Queue;
        r.queue = slots[(size_t)k].q;
        r.queueBlocking = file;
        r.previewOnly = false;
        r.threadName = "vg-src-" + std::to_string(k + 1);
        return r;
    }
    void muxRun(std::vector<double> bpsHint);
    void previewRun();
};

void Session::Impl::muxRun(std::vector<double> bpsHint) {
    const MuxConfig m = mcfg;
    const bool file = m.output == Output::File;
    const double rate = std::max(10000.0, muxRateFor(m, bpsHint));
    std::array<double, kMaxSources> lastRate{};
    std::vector<TsRemuxInput> in;
    for (int k = 0; k < kMaxSources; k++) {
        TsQueue* q = slots[(size_t)k].q.get();
        TsRemuxInput x;
        x.pop = [q](uint8_t* pk) { return q->pop(pk); };
        // a file's last packets still count after the source ended; a stopped live source leaves the multiplex (its slot is padded)
        x.rate = [q, k, &lastRate, file] {
            const double r = q->rate;
            if (r > 0) { lastRate[(size_t)k] = r; return r; }
            return file && q->size() > 0 ? lastRate[(size_t)k] : 0.0;
        };
        x.queued = [q] { return q->size(); };
        if (!file) x.flush = [q] { q->clear(); };   // a live source joining starts now, not with what queued before (#494)
        x.provider = m.provider;
        in.push_back(std::move(x));
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        for (int k = 0; k < kMaxSources && k < (int)numbering.size(); k++) {
            in[(size_t)k].serviceId = numbering[(size_t)k].serviceId;
            in[(size_t)k].pmtPid = numbering[(size_t)k].pmtPid;
            in[(size_t)k].esPids = numbering[(size_t)k].esPids;
        }
        mst.rateBps = rate;
    }
    TsRemuxConfig rc;
    rc.outBitrate = rate;
    rc.firstServiceId = std::max(1, m.firstServiceId);
    rc.tsid = m.tsid;
    TsRemux remux(std::move(in), rc);

    FILE* f = nullptr;
    OutputManager net;
    if (file) {
        f = u8fopen(m.tsPath, "wb");
        if (!f) { failMux("cannot write " + m.tsPath + ": " + strerror(errno)); muxActive = false; return; }
    } else if (m.output == Output::Udp || m.output == Output::Rtp) {
        OutputConfig oc;
        oc.udp = true; oc.host = m.host; oc.port = m.port; oc.ttl = m.ttl; oc.rtp = m.output == Output::Rtp;
        net.configure(oc);
        const OutputStats os = net.stats();
        if (!os.error.empty()) { failMux(os.error); muxActive = false; return; }
    } else if (m.output == Output::Transmitter && m.link) {
        m.link->startStream(rate);
    }

    std::vector<uint8_t> batch;
    batch.reserve(188 * 512);
    uint64_t packets = 0, nulls = 0;
    auto cpuT = std::chrono::steady_clock::now();
    uint64_t cpuNs = cpu::threadNs();
    auto statT = cpuT;
    bool sawSource = false, failed = false;
    auto flushBatch = [&](size_t n) {
        if (batch.empty()) return;
        if (f) { if (fwrite(batch.data(), 1, batch.size(), f) != batch.size()) { failMux("cannot write " + m.tsPath); failed = true; } }
        else if (m.output == Output::Transmitter) { if (m.link) m.link->sendTs(batch.data(), n); }   // not connected: dropped (OnAir is away)
        else { net.packets(batch.data(), n, nullptr); net.burstDone((double)n * 1504.0 / rate); }
        batch.clear();
    };
    auto emit = [&] {
        uint8_t pk[188];
        remux.next(pk);
        if (pk[1] == 0x1F && pk[2] == 0xFF) nulls++;
        batch.insert(batch.end(), pk, pk + 188);
        packets++;
    };
    auto status = [&](bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - statT < std::chrono::milliseconds(250)) return;
        statT = now;
        double need = 0;
        for (auto& s : slots) need += s.q->rate;
        std::lock_guard<std::mutex> lk(mu);
        mst.packets = packets; mst.nulls = nulls;
        mst.seconds = (double)packets * 1504.0 / rate;
        mst.sourcesBps = need;
        mst.overload = need > rate * 0.98;
        mst.programmes = remux.programmes();
        if (now - cpuT > std::chrono::seconds(1)) mst.cpuBusy = cpuPercent(cpuNs, cpuT);
    };

    if (file) {
        // as fast as the sources encode: before each packet every source in the multiplex has one waiting (or has ended), so the
        // file's packet spacing follows the mux rate exactly and nothing is padded that a source still had to send
        while (!muxStop && !failed) {
            bool waiting = false, any = false;
            for (auto& s : slots) {
                const bool live = s.q->producing || s.q->size() > 0;
                any = any || live;
                if (s.q->producing && s.q->size() == 0) waiting = true;
            }
            sawSource = sawSource || any;
            if (sawSource && !any) break;   // every source has ended and its queue is empty: the file is complete
            if (waiting || !any) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); status(false); continue; }
            emit();
            if (batch.size() >= 188 * 256) flushBatch(batch.size() / 188);
            status(false);
        }
        flushBatch(batch.size() / 188);
        if (fclose(f) != 0) failMux("cannot write " + m.tsPath);
    } else {
        // live: the mux rate in real time, a burst every 5 ms
        const auto t0 = std::chrono::steady_clock::now();
        while (!muxStop && !failed) {
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const uint64_t target = (uint64_t)(el * rate / 1504.0);
            size_t n = 0;
            while (packets < target && n < 4000) { emit(); n++; }
            if (packets < target) packets = target;   // far behind (the computer was asleep): do not send a burst of seconds
            flushBatch(n);
            status(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (m.output == Output::Transmitter && m.link) m.link->stopStream();
        if (m.output == Output::Udp || m.output == Output::Rtp) {
            for (int i = 0; i < 25; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the paced sender empties its queue
            net.close();
        }
    }
    status(true);
    std::lock_guard<std::mutex> lk(mu);
    mst.finished = mst.error.empty();
    mst.running = false;
    muxActive = false;
}

void Session::Impl::previewRun() {
    std::vector<uint8_t> tmp;
    std::array<uint64_t, kMaxSources> serial{};
    auto cpuT = std::chrono::steady_clock::now();
    uint64_t cpuNs = cpu::threadNs();
    while (!prevStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));   // about 12 pictures a second
        const int k = std::clamp(prevSrc.load(), 0, kMaxSources - 1);
        int w = 0, h = 0;
        if (slots[(size_t)k].gen.running() && slots[(size_t)k].gen.preview(tmp, w, h, serial[(size_t)k])) {
            std::lock_guard<std::mutex> lk(mu);
            if (!prevTaken) pst.dropped++;   // the window did not take the last one: the newest wins
            prevRgba.swap(tmp);
            prevW = w; prevH = h;
            prevSerial++;
            prevTaken = false;
            pst.frames++;
        }
        if (std::chrono::steady_clock::now() - cpuT > std::chrono::seconds(1)) {
            const double p = cpuPercent(cpuNs, cpuT);
            std::lock_guard<std::mutex> lk(mu);
            pst.cpuBusy = p;
        }
    }
}

Session::Session() : p_(std::make_unique<Impl>()) {}

Session::~Session() {
    stop();
    for (int k = 0; k < kMaxSources; k++) stopSourcePreview(k);
    stopPreview();
}

bool Session::start(const std::vector<SourceSpec>& sources, const MuxConfig& mux, std::string& err) {
    if (running()) { err = "already running"; return false; }
    if (p_->muxTh.joinable()) p_->muxTh.join();
    if (mux.output == Output::File && mux.tsPath.empty()) { err = "choose a .ts file to write"; return false; }
    if ((mux.output == Output::Udp || mux.output == Output::Rtp) && (mux.port <= 0 || mux.port > 65535)) { err = "the port must be 1 to 65535"; return false; }
    if (mux.output == Output::Transmitter && !mux.link) { err = "no link to OnAir"; return false; }
    if (mux.output == Output::Queue) { err = "not a mux output"; return false; }
    std::vector<double> bps;
    std::vector<MuxNumbering> num((size_t)kMaxSources);
    int enabled = 0;
    for (size_t k = 0; k < sources.size() && k < (size_t)kMaxSources; k++) {
        if (!sources[k].enabled) continue;
        enabled++;
        bps.push_back(estimate(sources[k].cfg.enc).mux * 1000.0);
        num[k] = sources[k].num;
    }
    if (enabled == 0) { err = "no source to send"; return false; }
    const bool file = mux.output == Output::File;
    // the sources first: each its own thread
    for (size_t k = 0; k < sources.size() && k < (size_t)kMaxSources; k++) {
        if (!sources[k].enabled) continue;
        Impl::Slot& s = p_->slots[k];
        if (s.gen.running()) s.gen.stop();   // a preview-only run of this source
        s.previewOnly = false;
        s.q->close();
        s.q->clear();
        s.q->reopen();
        s.q->rate = 0;
        std::string e;
        if (!s.gen.start(p_->sourceConfig((int)k, sources[k].cfg, file), e)) {
            for (auto& o : p_->slots) { o.q->close(); o.gen.stop(); o.q->clear(); }
            err = "Source " + std::to_string(k + 1) + ": " + e;
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->mst = MuxStatus();
        p_->mst.running = true;
        p_->mcfg = mux;
        p_->numbering = num;
    }
    p_->muxStop = false;
    p_->muxActive = true;
    Impl* I = p_.get();
    p_->muxTh = namedThread("vg-mux", [I, bps] { I->muxRun(bps); });
    return true;
}

void Session::stop() {
    // the sources first (a file's source waiting for room is woken by close), then the mux
    for (auto& s : p_->slots) {
        if (s.previewOnly) continue;
        s.q->close();
        s.gen.stop();
    }
    p_->muxStop = true;
    if (p_->muxTh.joinable()) p_->muxTh.join();
    for (auto& s : p_->slots) if (!s.previewOnly) s.q->clear();
}

bool Session::running() const { return p_->muxActive; }

bool Session::startSource(int k, const GenConfig& cfg, const MuxNumbering& num, std::string& err) {
    if (k < 0 || k >= kMaxSources) { err = "no such source"; return false; }
    if (!running()) { err = "the mux is not running"; return false; }
    Impl::Slot& s = p_->slots[(size_t)k];
    if (s.gen.running() && !s.previewOnly) { err = "already running"; return false; }
    if (s.gen.running()) s.gen.stop();
    s.previewOnly = false;
    s.q->clear();
    s.q->reopen();
    (void)num;   // the numbering of a source added while running is automatic until the next start of the multiplex
    bool file = false;
    { std::lock_guard<std::mutex> lk(p_->mu); file = p_->mcfg.output == Output::File; }
    return s.gen.start(p_->sourceConfig(k, cfg, file), err);
}

void Session::stopSource(int k) {
    if (k < 0 || k >= kMaxSources) return;
    Impl::Slot& s = p_->slots[(size_t)k];
    s.q->close();
    s.gen.stop();
    s.q->clear();
}

void Session::setOverlay(int k, const Layout& l) { if (k >= 0 && k < kMaxSources) p_->slots[(size_t)k].gen.setOverlay(l); }
void Session::setChannelBps(double bps) { p_->channelBps = bps; }

bool Session::startPreviewOnly(int k, const GenConfig& cfg, std::string& err) {
    if (k < 0 || k >= kMaxSources) { err = "no such source"; return false; }
    Impl::Slot& s = p_->slots[(size_t)k];
    if (s.gen.running() && !s.previewOnly) { err = "this source is streaming: its preview shows already"; return false; }
    if (s.gen.running()) s.gen.stop();
    GenConfig r = cfg;
    r.previewOnly = true;
    r.output = Output::Udp;   // not used by a preview
    r.queue.reset();
    r.threadName = "vg-src-" + std::to_string(k + 1);
    s.previewOnly = true;
    if (!s.gen.start(r, err)) { s.previewOnly = false; return false; }
    return true;
}

bool Session::sourceRunning(int k) const { return k >= 0 && k < kMaxSources && p_->slots[(size_t)k].gen.running(); }
bool Session::sourcePreviewOnly(int k) const { return sourceRunning(k) && p_->slots[(size_t)k].previewOnly; }

void Session::stopSourcePreview(int k) {
    if (k < 0 || k >= kMaxSources) return;
    Impl::Slot& s = p_->slots[(size_t)k];
    if (!s.previewOnly) return;
    s.gen.stop();
    s.previewOnly = false;
}

void Session::startPreview(int k) {
    p_->prevSrc = std::clamp(k, 0, kMaxSources - 1);
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pst.source = p_->prevSrc; }
    if (p_->prevTh.joinable()) return;
    p_->prevStop = false;
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pst.running = true; p_->pst.frames = p_->pst.dropped = 0; }
    Impl* I = p_.get();
    p_->prevTh = namedThread("vg-preview", [I] { I->previewRun(); });
}

void Session::stopPreview() {
    p_->prevStop = true;
    if (p_->prevTh.joinable()) p_->prevTh.join();
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pst.running = false;
}

bool Session::previewRunning() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->pst.running; }

bool Session::previewFrame(std::vector<uint8_t>& rgba, int& w, int& h, uint64_t& serial) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (serial == p_->prevSerial || p_->prevRgba.empty()) return false;
    rgba = p_->prevRgba;
    w = p_->prevW; h = p_->prevH;
    serial = p_->prevSerial;
    p_->prevTaken = true;
    return true;
}

SourceStatus Session::sourceStatus(int k) const {
    SourceStatus st;
    if (k < 0 || k >= kMaxSources) return st;
    const Impl::Slot& s = p_->slots[(size_t)k];
    st.running = s.gen.running();
    st.previewOnly = st.running && s.previewOnly;
    st.gen = s.gen.stats();
    st.queued = s.q->size();
    st.dropped = s.q->dropped();
    st.cpuBusy = st.gen.cpuBusy;
    return st;
}

MuxStatus Session::muxStatus() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->mst; }
PreviewStatus Session::previewStatus() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->pst; }
TsQueue& Session::queue(int k) { return *p_->slots[(size_t)std::clamp(k, 0, kMaxSources - 1)].q; }

} // namespace dect2::vgen
