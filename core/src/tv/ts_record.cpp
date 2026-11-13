// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/tv/ts_record.h"
#include "dect2/common/u8file.h"
#include <chrono>
#include <cstring>
#include "dect2/common/diag.h"

namespace dect2 {

bool TsWriter::open(const std::string& path, size_t maxQueueBytes) {
    close();
    FILE* f = u8fopen(path, "wb");
    if (!f) { DIAG_ERROR(606687, "The transport stream recording file %s could not be created, so nothing is recorded to it.", path.c_str()); return false; }
    { std::lock_guard<std::mutex> lk(mu_); f_ = f; q_.clear(); queued_ = 0; max_ = maxQueueBytes; stop_ = false; failed_ = false; open_ = true; written_ = 0; dropped_ = 0; path_ = path; }
    th_ = dect2::namedThread("onair-tsrec", [this] { run(); });
    return true;
}
void TsWriter::close() {
    { std::lock_guard<std::mutex> lk(mu_); if (!open_) return; stop_ = true; hold_ = false; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(mu_);
    if (f_) { fclose(f_); f_ = nullptr; }
    open_ = false;
}
bool TsWriter::isOpen() const { std::lock_guard<std::mutex> lk(mu_); return open_; }
void TsWriter::write(const uint8_t* p, size_t n) {
    if (!n) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!open_ || stop_) return;
        if (queued_ + n > max_) { dropped_ += n / 188; return; }
        q_.emplace_back(p, p + n);
        queued_ += n;
    }
    cv_.notify_one();
}
uint64_t TsWriter::bytes() const { std::lock_guard<std::mutex> lk(mu_); return written_; }
uint64_t TsWriter::droppedPackets() const { std::lock_guard<std::mutex> lk(mu_); return dropped_; }
bool TsWriter::failed() const { std::lock_guard<std::mutex> lk(mu_); return failed_; }
std::string TsWriter::path() const { std::lock_guard<std::mutex> lk(mu_); return path_; }
void TsWriter::holdForTest(bool hold) { { std::lock_guard<std::mutex> lk(mu_); hold_ = hold; } cv_.notify_all(); }

void TsWriter::run() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        cv_.wait(lk, [this] { return stop_ || (!hold_ && !q_.empty()); });
        if (q_.empty()) { if (stop_) return; continue; }
        std::vector<uint8_t> c = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        const size_t w = fwrite(c.data(), 1, c.size(), f_);   // f_ is only closed after this thread has ended
        lk.lock();
        queued_ -= c.size();
        written_ += w;
        if (w != c.size()) { failed_ = true; DIAG_EVERY(848183, 30.0, "Writing the transport stream recording %s failed (the disk may be full), so the recording stops.", path_.c_str()); }
    }
}

void TsRing::setCapacityPackets(size_t n) {
    cap_ = n;
    buf_.reset(n ? new uint8_t[n * 188] : nullptr);   // not zeroed: the pages are only committed as they fill
    head_ = 0;
}
void TsRing::push(const uint8_t* pkt) {
    if (!cap_) return;
    memcpy(buf_.get() + (head_ % cap_) * 188, pkt, 188);
    head_++;
}
bool TsRing::read(uint64_t i, uint8_t* out) const {
    if (!cap_ || i >= head_ || i < oldest()) return false;
    memcpy(out, buf_.get() + (i % cap_) * 188, 188);
    return true;
}

bool tsPcrSeconds(const uint8_t* p, double& sec) {
    if (p[0] != 0x47 || !(p[3] & 0x20) || p[4] < 7 || !(p[5] & 0x10)) return false;
    const uint64_t base = ((uint64_t)p[6] << 25) | ((uint64_t)p[7] << 17) | ((uint64_t)p[8] << 9) | ((uint64_t)p[9] << 1) | (p[10] >> 7);
    const uint32_t ext = ((p[10] & 1) << 8) | p[11];
    sec = (double)base / 90000.0 + (double)ext / 27000000.0;
    return true;
}

} // namespace dect2
