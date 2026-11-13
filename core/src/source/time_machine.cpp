// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/source/time_machine.h"
#include "dect2/common/u8file.h"
#include "dect2/source/iq_record.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace dect2 {

struct TimeMachine::Ring {
    std::unique_ptr<int8_t[]> data;   // not zero-filled: the pages are used as the ring fills
    size_t cap = 0;                   // stored samples (2 bytes each)
    int decim = 1;
    double srcRate = 0, rate = 0;     // the radio's, and the stored (srcRate / decim)
    std::atomic<uint64_t> head{0};    // stored samples written so far (the position of the next one)
    std::atomic<uint64_t> base{0};    // history older than this is not valid (a retune)
    std::atomic<bool> cancel{false};
    // producer only: the decimation's partial sum
    float accI = 0, accQ = 0;
    int accN = 0;
};

static inline int8_t q8(float v) { return (int8_t)std::max(-127L, std::min(127L, lrintf(v * 127.f))); }

std::shared_ptr<TimeMachine::Ring> TimeMachine::get() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ring_;
}

double TimeMachine::configure(double wantSeconds, double radioRateHz, int decimate, int memBudgetMB) {
    std::shared_ptr<Ring> old;
    std::thread w;
    {
        std::lock_guard<std::mutex> lk(mu_);
        on_ = false;
        if (ring_) ring_->cancel = true;
        old = std::move(ring_);
        w = std::move(worker_);
    }
    if (w.joinable()) w.join();   // the worker polls `cancel` every few milliseconds
    old.reset();
    if (!(wantSeconds > 0) || !(radioRateHz > 0)) { std::lock_guard<std::mutex> lk(mu_); saves_ = 0; return 0; }
    int d = decimate < 2 ? 1 : decimate < 4 ? 2 : decimate < 8 ? 4 : decimate < 16 ? 8 : 16;
    auto r = std::make_shared<Ring>();
    r->decim = d;
    r->srcRate = radioRateHz;
    r->rate = radioRateHz / d;
    const double budget = std::max(16, std::min(memBudgetMB, 64 * 1024)) * 1048576.0;
    const double want = std::min(wantSeconds, 24.0 * 3600) * r->rate * 2;
    r->cap = (size_t)(std::min(want, budget) / 2);
    if (r->cap < 1024) return 0;
    try { r->data.reset(new int8_t[r->cap * 2]); } catch (...) { return 0; }
    const double win = r->cap / r->rate;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ring_ = r;
        saves_ = 0; lastPath_.clear(); error_.clear(); lastSeconds_ = 0; lastMissing_ = 0;
    }
    on_ = true;
    return win;
}

void TimeMachine::discard() {
    if (auto r = get()) r->base.store(r->head.load(std::memory_order_acquire), std::memory_order_release);
}

double TimeMachine::storedRate() const {
    auto r = get();
    return r ? r->rate : 0;
}

void TimeMachine::pushOn(const cf32* x, size_t n) {
    Ring* r;
    std::shared_ptr<Ring> keep;
    { std::lock_guard<std::mutex> lk(mu_); keep = ring_; }   // uncontended: the UI thread only takes it to swap the pointer
    r = keep.get();
    if (!r || !n) return;
    uint64_t h = r->head.load(std::memory_order_relaxed);
    if (r->decim == 1) {
        size_t i = 0;
        while (i < n) {
            const size_t at = (size_t)(h % r->cap);
            const size_t m = std::min(n - i, r->cap - at);
            int8_t* d = r->data.get() + at * 2;
            for (size_t k = 0; k < m; k++) { d[2 * k] = q8(x[i + k].real()); d[2 * k + 1] = q8(x[i + k].imag()); }
            i += m; h += m;
            r->head.store(h, std::memory_order_release);
        }
        return;
    }
    const float inv = 1.f / r->decim;
    for (size_t i = 0; i < n; i++) {
        r->accI += x[i].real(); r->accQ += x[i].imag();
        if (++r->accN == r->decim) {
            int8_t* d = r->data.get() + (size_t)(h % r->cap) * 2;
            d[0] = q8(r->accI * inv); d[1] = q8(r->accQ * inv);
            r->accI = r->accQ = 0; r->accN = 0;
            h++;
        }
    }
    r->head.store(h, std::memory_order_release);
}

bool TimeMachine::trigger(const std::string& path, double preSeconds, double postSeconds, double centerHz, bool sigmf, std::string& err) {
    auto r = get();
    if (!on_.load() || !r) { err = "the time machine is off"; return false; }
    if (busy_.load()) { err = "a rewind save is already running"; return false; }
    preSeconds = std::max(0.0, preSeconds);
    postSeconds = std::max(0.0, postSeconds);
    const uint64_t head = r->head.load(std::memory_order_acquire), base = r->base.load(std::memory_order_acquire);
    const uint64_t oldest = std::max<uint64_t>(base, head > r->cap ? head - r->cap : 0);
    const uint64_t preN = (uint64_t)(preSeconds * r->rate);
    const uint64_t start = std::max(oldest, head > preN ? head - preN : 0);
    const uint64_t end = head + (uint64_t)(postSeconds * r->rate);
    const uint64_t bytes = fileBytes(end - start);
    if (end <= start) { err = "nothing to save yet"; return false; }
    if (bytes > kMaxFileBytes) { err = "the file would be bigger than 4 GB: choose a shorter time"; return false; }
    try {
        const std::filesystem::path p = dect2::u8file(path);
        const std::filesystem::path dir = p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
        std::filesystem::create_directories(dir);
        std::error_code ec;
        const auto sp = std::filesystem::space(dir, ec);
        if (!ec && sp.available < bytes + bytes / 8 + (64ull << 20)) { err = "not enough free disk space for this recording"; return false; }
    } catch (const std::exception& e) { err = std::string("cannot create the folder: ") + e.what(); return false; }
    std::thread old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        old = std::move(worker_);
    }
    if (old.joinable()) old.join();   // busy_ is false: it has finished
    busy_ = true;
    savePos_ = 0; saveSpan_ = end - start;
    {
        std::lock_guard<std::mutex> lk(mu_);
        error_.clear();
        worker_ = dect2::namedThread("onair-tmsave", [this, r, path, start, end, centerHz, sigmf] { saveLoop(r, path, start, end, centerHz, sigmf); });
    }
    return true;
}

void TimeMachine::saveLoop(std::shared_ptr<Ring> r, std::string path, uint64_t startPos, uint64_t endPos, double centerHz, bool sigmf) {
    const std::string when = sigmfUtcNow();
    FILE* f = nullptr;
    std::string err;
    try { f = fopen(dect2::u8file(path).string().c_str(), "wb"); } catch (...) {}
    if (!f) err = "cannot create " + path;
    uint64_t pos = startPos, written = 0, missing = 0;
    std::vector<int8_t> blk;
    const size_t kChunk = 1u << 20;
    auto lastMove = std::chrono::steady_clock::now();
    uint64_t lastHead = r->head.load();
    while (f && pos < endPos && !r->cancel.load()) {
        const uint64_t head = r->head.load(std::memory_order_acquire);
        if (head != lastHead) { lastHead = head; lastMove = std::chrono::steady_clock::now(); }
        const uint64_t upto = std::min(head, endPos);
        if (upto <= pos) {
            if (std::chrono::steady_clock::now() - lastMove > std::chrono::seconds(3)) break;   // the radio stopped: keep what there is
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const size_t n = (size_t)std::min<uint64_t>(upto - pos, kChunk);
        blk.resize(n * 2);
        for (size_t done = 0; done < n;) {   // copy out, around the end of the ring
            const size_t at = (size_t)((pos + done) % r->cap), m = std::min(n - done, r->cap - at);
            memcpy(blk.data() + done * 2, r->data.get() + at * 2, m * 2);
            done += m;
        }
        // overwritten while we copied? (the producer is a whole ring ahead of this chunk)
        if (r->head.load(std::memory_order_acquire) - pos > r->cap) {
            missing += n;
            if (written) { std::fill(blk.begin(), blk.end(), (int8_t)0); }   // keep the timing of what follows
            else { pos += n; continue; }                                       // nothing written yet: the file just starts later
        }
        if (fwrite(blk.data(), 1, blk.size(), f) != blk.size()) { err = "cannot write " + path + " (disk full?)"; break; }
        written += n; pos += n;
        savePos_ = pos - startPos;
    }
    if (r->cancel.load() && err.empty()) err = "cancelled";
    if (f) { if (fclose(f) != 0 && err.empty()) err = "cannot write " + path; }
    if (err.empty() && !written) err = "no samples were saved";
    if (!err.empty() && f) { std::error_code ec; std::filesystem::remove(dect2::u8file(path), ec); }
    if (err.empty() && sigmf) {
        try {
            const std::filesystem::path p = dect2::u8file(path);
            std::filesystem::path b = p; b.replace_extension("");
            SigmfInfo i;
            i.fmt = FileFormat::CS8; i.rateHz = r->rate; i.centerHz = centerHz; i.datetimeUtc = when;
            const std::string::size_type s = path.find_last_of("/\\");
            i.dataFile = path.substr(s == std::string::npos ? 0 : s + 1);
            i.sampleCount = written;
            i.description = "OnAir time machine: the samples around a trigger";
            FILE* m = fopen(dect2::u8file(b.string() + ".sigmf-meta").string().c_str(), "wb");
            if (m) { const std::string j = sigmfMetaJson(i); fwrite(j.data(), 1, j.size(), m); fclose(m); }
            std::error_code ec;
            std::filesystem::remove(b.string() + ".sigmf-data", ec);
            std::filesystem::create_hard_link(p, b.string() + ".sigmf-data", ec);
        } catch (...) {}
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (err.empty()) { saves_++; lastPath_ = path; lastSeconds_ = written / r->rate; lastMissing_ = missing; }
        else error_ = err;
    }
    busy_ = false;
}

TimeMachineStats TimeMachine::stats() const {
    TimeMachineStats s;
    std::shared_ptr<Ring> r;
    {
        std::lock_guard<std::mutex> lk(mu_);
        r = ring_;
        s.saves = saves_; s.lastPath = lastPath_; s.lastSeconds = lastSeconds_; s.lastMissing = lastMissing_; s.error = error_;
    }
    s.enabled = on_.load() && r;
    if (!r) return s;
    s.storedRateHz = r->rate;
    s.windowSeconds = r->cap / r->rate;
    const uint64_t head = r->head.load(), base = r->base.load();
    const uint64_t have = std::min<uint64_t>(head > base ? head - base : 0, r->cap);
    s.filledSeconds = have / r->rate;
    s.bytes = (uint64_t)r->cap * 2;
    s.saving = busy_.load();
    const uint64_t span = saveSpan_.load();
    s.savePct = s.saving && span ? 100.0 * std::min<uint64_t>(savePos_.load(), span) / span : 0;
    return s;
}

}  // namespace dect2
