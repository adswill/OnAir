// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/timeshift.h"
#include "dect2/tv/ts_record.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include "dect2/common/diag.h"

namespace dect2 {

// ---- PcrTimeline -------------------------------------------------------------------------------------------------------------------------------
void PcrTimeline::add(uint64_t idx, double raw) {
    double t = raw + offset_;
    if (have_) {
        double d = t - lastT_;
        if (d < -1000.0) { offset_ += 95443.7176; t = raw + offset_; d = t - lastT_; }                     // 33 bit rollover
        if (d < -0.5 || d > 10.0) { offset_ += (lastT_ + 0.04) - t; t = raw + offset_; }                     // clock jump: the timeline goes on
        if (t < lastT_) t = lastT_;
    }
    have_ = true;
    lastT_ = t;
    if (!marks_.empty() && t - marks_.back().t < 0.095 && idx > marks_.back().idx) return;
    if (!marks_.empty() && idx <= marks_.back().idx) return;
    marks_.push_back({idx, t});
}

void PcrTimeline::trimBefore(uint64_t idx) {
    while (marks_.size() > 1 && marks_[1].idx <= idx) marks_.pop_front();
}

bool PcrTimeline::timeAt(uint64_t idx, double& t) const {
    if (marks_.empty() || idx < marks_.front().idx) return false;
    if (idx >= marks_.back().idx) { t = marks_.back().t; return true; }
    size_t lo = 0, hi = marks_.size() - 1;
    while (hi - lo > 1) { const size_t m = (lo + hi) / 2; if (marks_[m].idx <= idx) lo = m; else hi = m; }
    const Mark &a = marks_[lo], &b = marks_[hi];
    t = a.t + (b.t - a.t) * (double)(idx - a.idx) / (double)(b.idx - a.idx);
    return true;
}

bool PcrTimeline::indexAt(double t, uint64_t& idx) const {
    if (marks_.empty()) return false;
    if (t <= marks_.front().t) { idx = marks_.front().idx; return true; }
    if (t >= marks_.back().t) { idx = marks_.back().idx; return true; }
    size_t lo = 0, hi = marks_.size() - 1;
    while (hi - lo > 1) { const size_t m = (lo + hi) / 2; if (marks_[m].t <= t) lo = m; else hi = m; }
    const Mark &a = marks_[lo], &b = marks_[hi];
    const double f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0;
    idx = a.idx + (uint64_t)(f * (double)(b.idx - a.idx) + 0.5);
    return true;
}

// ---- DiskTimeshift -----------------------------------------------------------------------------------------------------------------------------
static std::string segPath(const std::string& dir, uint64_t n) {
    char b[48];
    snprintf(b, sizeof b, "ts_%08llu.seg", (unsigned long long)n);
    return dir + "/" + b;
}

bool DiskTimeshift::open(const TimeshiftConfig& c) {
    close();
    std::error_code ec;
    std::filesystem::create_directories(u8file(c.dir), ec);
    if (ec) { DIAG_ERROR(309747, "The timeshift folder %s could not be created, so timeshift is not started.", c.dir.c_str()); return false; }
    // left-overs of an earlier run (a crash): only our own file pattern
    for (auto& e : std::filesystem::directory_iterator(u8file(c.dir), ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() == 15 && n.compare(0, 3, "ts_") == 0 && n.compare(11, 4, ".seg") == 0) { std::error_code e2; std::filesystem::remove(e.path(), e2); }
    }
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = c;
    if (cfg_.segmentPackets < 16) cfg_.segmentPackets = 16;
    open_ = true; failed_ = false; head_ = 0; serial_ = 0; pcrPid_ = -1;
    segs_.clear(); tl_.clear();
    return true;
}

void DiskTimeshift::removeAllLocked() {
    if (w_) { fclose(w_); w_ = nullptr; }
    if (r_) { fclose(r_); r_ = nullptr; rSeg_ = (size_t)-1; }
    std::error_code ec;
    for (auto& s : segs_) std::filesystem::remove(u8file(s.path), ec);
    segs_.clear();
}

void DiskTimeshift::close() {
    std::lock_guard<std::mutex> lk(mu_);
    removeAllLocked();
    open_ = false;
    tl_.clear();
}

void DiskTimeshift::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    removeAllLocked();
    tl_.clear();
    pcrPid_ = -1;
}

bool DiskTimeshift::isOpen() const { std::lock_guard<std::mutex> lk(mu_); return open_; }
bool DiskTimeshift::failed() const { std::lock_guard<std::mutex> lk(mu_); return failed_; }
TimeshiftConfig DiskTimeshift::config() const { std::lock_guard<std::mutex> lk(mu_); return cfg_; }
void DiskTimeshift::setLimits(double s, uint64_t b) { std::lock_guard<std::mutex> lk(mu_); cfg_.maxSeconds = s; cfg_.maxBytes = b; trimLocked(); }
size_t DiskTimeshift::segmentCount() const { std::lock_guard<std::mutex> lk(mu_); return segs_.size(); }
uint64_t DiskTimeshift::first() const { std::lock_guard<std::mutex> lk(mu_); return segs_.empty() ? head_ : segs_.front().first; }
uint64_t DiskTimeshift::end() const { std::lock_guard<std::mutex> lk(mu_); return head_; }
uint64_t DiskTimeshift::bytes() const { std::lock_guard<std::mutex> lk(mu_); return (head_ - (segs_.empty() ? head_ : segs_.front().first)) * 188; }

double DiskTimeshift::bufferedSeconds() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (segs_.empty() || tl_.empty()) return 0;
    double a = 0;
    if (!tl_.timeAt(segs_.front().first, a)) a = tl_.firstTime();
    return std::max(0.0, tl_.lastTime() - a);
}

void DiskTimeshift::trimLocked() {
    while (segs_.size() > 1) {
        const Seg& next = segs_[1];
        bool drop = false;
        double tn = 0;
        if (cfg_.maxSeconds > 0 && tl_.timeAt(next.first, tn) && tl_.lastTime() - tn >= cfg_.maxSeconds) drop = true;
        if (cfg_.maxBytes && (head_ - segs_.front().first) * 188 > cfg_.maxBytes) drop = true;
        if (!drop) break;
        if (r_ && rSeg_ == 0) { fclose(r_); r_ = nullptr; rSeg_ = (size_t)-1; }
        else if (r_ && rSeg_ != (size_t)-1) rSeg_--;
        std::error_code ec;
        std::filesystem::remove(u8file(segs_.front().path), ec);
        segs_.pop_front();
        tl_.trimBefore(segs_.front().first);
    }
}

void DiskTimeshift::rollLocked() {
    if (w_) { fclose(w_); w_ = nullptr; }
    Seg s;
    s.first = head_;
    s.path = segPath(cfg_.dir, serial_++);
    w_ = u8fopen(s.path, "wb");
    if (!w_) { failed_ = true; DIAG_ERROR(727613, "The timeshift segment file %s could not be created, so timeshift stops.", s.path.c_str()); return; }
    segs_.push_back(s);
}

void DiskTimeshift::push(const uint8_t* p, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open_ || failed_) return;
    for (size_t i = 0; i < n; i++, p += 188) {
        if (p[0] != 0x47) continue;
        if (segs_.empty() || segs_.back().count >= cfg_.segmentPackets || !w_) {
            rollLocked();
            if (failed_) return;
            trimLocked();
        }
        if (fwrite(p, 1, 188, w_) != 188) { failed_ = true; DIAG_ERROR(700688, "Writing to the timeshift segment file failed (the disk may be full), so timeshift stops."); return; }
        double pcr;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if ((pcrPid_ < 0 || pid == pcrPid_) && tsPcrSeconds(p, pcr)) { pcrPid_ = pid; tl_.add(head_, pcr); }
        segs_.back().count++;
        head_++;
    }
}

size_t DiskTimeshift::read(uint64_t idx, uint8_t* out, size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t got = 0;
    while (got < n && idx < head_ && !segs_.empty() && idx >= segs_.front().first) {
        // find the segment holding idx
        size_t lo = 0, hi = segs_.size();
        while (hi - lo > 1) { const size_t m = (lo + hi) / 2; if (segs_[m].first <= idx) lo = m; else hi = m; }
        const Seg& s = segs_[lo];
        if (idx >= s.first + s.count) break;
        if (lo + 1 == segs_.size() && w_) fflush(w_);
        if (!r_ || rSeg_ != lo) {
            if (r_) fclose(r_);
            r_ = u8fopen(s.path, "rb");
            rSeg_ = r_ ? lo : (size_t)-1;
            if (!r_) break;
        }
        const size_t want = (size_t)std::min<uint64_t>(n - got, s.first + s.count - idx);
        if (fseek(r_, (long)((idx - s.first) * 188), SEEK_SET) != 0) break;
        const size_t r = fread(out + got * 188, 188, want, r_);
        if (r == 0) break;
        got += r;
        idx += r;
    }
    return got;
}

bool DiskTimeshift::timeAt(uint64_t idx, double& t) const { std::lock_guard<std::mutex> lk(mu_); return tl_.timeAt(idx, t); }
bool DiskTimeshift::indexAt(double t, uint64_t& idx) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!tl_.indexAt(t, idx)) return false;
    if (!segs_.empty() && idx < segs_.front().first) idx = segs_.front().first;
    return true;
}

} // namespace dect2
