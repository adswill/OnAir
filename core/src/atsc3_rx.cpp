#include <deque>
#include "dect2/atsc3_rx.h"
#include <algorithm>
#include <chrono>

namespace dect2 {

using namespace atsc3;

namespace {
double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

Atsc3Rx::Atsc3Rx() {}

Atsc3Rx::~Atsc3Rx() { stop(); }

void Atsc3Rx::configure(double rate) {
    stop();
    rate_ = rate;
    start();
}

void Atsc3Rx::start() {
    if (rate_ <= 0) return;
    rx_.reset(new Atsc3Receiver());
    sync_.reset(new Atsc3Sync(rate_, rx_.get()));
    sync_->setThreads(std::max(2, std::min(4, (int)std::thread::hardware_concurrency() / 2)));
    if (want_ >= 0) rx_->selectService(want_); else rx_->setAutoSelect(true);
    stop_ = false;
    queued_ = 0;
    q_.clear();
    busySec_ = 0; signalSec_ = 0; lastFrameWall_ = 0; lastFrames_ = 0; tsBytes_ = 0;
    { std::lock_guard<std::mutex> lk(tmu_); tel_ = Atsc3Telemetry(); }
    worker_ = std::thread([this] { worker(); });
    pump_ = std::thread([this] { pump(); });
}

void Atsc3Rx::stop() {
    if (stop_.exchange(true) && !worker_.joinable() && !pump_.joinable()) return;
    cv_.notify_all();
    cvSpace_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (sync_) sync_->flush();   // frames still being decoded on the worker threads reach the receiver before it is stopped
    if (rx_) rx_->stop();   // wakes the transport stream reader
    if (pump_.joinable()) pump_.join();
    sync_.reset();
    rx_.reset();
}

void Atsc3Rx::reset() {
    if (rate_ <= 0) return;
    stop();
    start();
}

void Atsc3Rx::feed(const cf32* x, size_t n) {
    if (stop_) return;
    std::unique_lock<std::mutex> lk(mu_);
    const size_t cap = (size_t)(rate_ * 1.5);   // a second and a half of samples may wait
    if (blocking_) {
        while (queued_ > cap && !stop_) cvSpace_.wait_for(lk, std::chrono::milliseconds(20));
        if (stop_) return;
    } else {
        while (queued_ > cap && !q_.empty()) { queued_ -= q_.front().size(); q_.pop_front(); dropped_++; }
    }
    q_.emplace_back(x, x + n);
    queued_ += n;
    cv_.notify_one();
}

void Atsc3Rx::selectService(int id) {
    want_ = id;
    if (rx_ && id >= 0) rx_->selectService(id);
    if (rx_ && id < 0) rx_->setAutoSelect(true);
}

void Atsc3Rx::autoSelect() {
    if (!rx_ || rx_->selectedService() >= 0) return;
    for (auto& s : rx_->services()) {
        if (s.category == 1 && !s.hidden && s.slsProtocol == 1) { rx_->selectService(s.serviceId); return; }
    }
}

void Atsc3Rx::worker() {
    double lastTel = 0;
    while (!stop_) {
        std::vector<cf32> blk;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return stop_ || !q_.empty(); });
            if (stop_) break;
            if (q_.empty()) continue;
            blk = std::move(q_.front());
            q_.pop_front();
            queued_ -= blk.size();
            cvSpace_.notify_all();
        }
        const double t0 = wallNow();
        sync_->push(blk.data(), blk.size());
        autoSelect();
        const double dt = wallNow() - t0;
        // the load over about the last ten seconds of signal
        busySec_ = busySec_ + dt;
        signalSec_ = signalSec_ + (double)blk.size() / rate_;
        if (signalSec_ > 10.0) { busySec_ = busySec_ * 0.5; signalSec_ = signalSec_ * 0.5; }
        const double now = wallNow();
        if (now - lastTel > 0.1) {
            lastTel = now;
            Atsc3Telemetry t;
            auto ss = sync_->stats();
            auto rs = rx_->stats();
            t.locked = ss.frames > 0 && ss.frames > lastFrames_ ? true : tel_.locked;
            if (ss.frames > lastFrames_) { lastFrames_ = ss.frames; lastFrameWall_ = now; }
            t.secSinceFrame = lastFrameWall_ > 0 ? now - lastFrameWall_ : 1e9;
            t.locked = t.secSinceFrame < 1.5;
            t.cfoHz = ss.cfoHz; t.bootstraps = ss.bootstraps; t.frames = ss.frames; t.framesFailed = rs.framesFailed;
            t.bbPackets = rs.bbPackets; t.bbBad = rs.bbBad; t.alpPackets = rs.alpPackets; t.udp = rs.udp; t.llsTables = rs.llsTables; t.routeObjects = rs.routeObjects;
            t.services = rx_->services();
            t.selected = rx_->selectedService();
            t.serviceReady = rx_->serviceReady();
            t.frame = rx_->frameInfo();
            t.tsBytes = tsBytes_;
            t.droppedBlocks = dropped_;
            t.load = signalSec_ > 0 ? busySec_ / signalSec_ : 0;
            t.seq = ++seq_;
            std::lock_guard<std::mutex> lk(tmu_);
            tel_ = std::move(t);
        }
    }
}

void Atsc3Rx::pump() {
    // The transport stream in whole packets. The remuxer delivers a segment's worth of packets at once, so the stream time handed on is the wall
    // clock time since the last call, also when nothing came: the bit rate shown is then an average, not the rate of a burst.
    std::vector<uint8_t> buf(188 * 256), acc;
    double last = wallNow();
    std::deque<std::pair<double, double>> win;
    while (!stop_) {
        Atsc3Receiver* r = rx_.get();
        if (!r) break;
        int n = r->readTsTimed(buf.data(), (int)buf.size(), 100);
        if (n < 0) break;
        if (n > 0) acc.insert(acc.end(), buf.begin(), buf.begin() + n);
        size_t start = 0;
        while (start < acc.size() && acc[start] != 0x47) start++;
        if (start) acc.erase(acc.begin(), acc.begin() + start);
        const size_t whole = acc.size() / 188 * 188;
        const double now = wallNow();
        if (whole || now - last >= 2.0) {   // empty gaps between segment bursts are not reported, so the bitrate display does not sag to zero
            // segments arrive in bursts; report the rate over the last two seconds so the display does not spike
            double secs = std::min(2.0, now - last);
            last = now;
            tsBytes_ += (long)whole;
            win.push_back({now, (double)whole});
            while (!win.empty() && now - win.front().first > 2.0) win.pop_front();
            double wb = 0;
            for (auto& w : win) wb += w.second;
            if (whole && wb > 0) secs = std::max(secs, std::min(2.0, whole / wb * 2.0));
            if (cb_) cb_(acc.data(), whole / 188, secs);
            acc.erase(acc.begin(), acc.begin() + whole);
        }
    }
}

bool Atsc3Rx::telemetry(Atsc3Telemetry& t, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(tmu_);
    if (tel_.seq <= lastSeq) return false;
    t = tel_;
    return true;
}

} // namespace dect2
