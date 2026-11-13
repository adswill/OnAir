// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/fastscan/fwsweep.h"
#include <libhackrf/hackrf.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace dect2 {
namespace fastscan {

size_t parseSweepBuffer(const uint8_t* buf, size_t len, double offsetHz, const std::function<void(double, const cf32*, size_t)>& cb) {
    constexpr size_t kHeader = 16, kN = (kSweepBlockBytes - kHeader) / 2;
    cf32 tmp[kN];
    size_t good = 0;
    for (size_t off = 0; off + kSweepBlockBytes <= len; off += kSweepBlockBytes) {
        const uint8_t* b = buf + off;
        if (b[0] != 0x7F || b[1] != 0x7F) continue;
        uint64_t f = 0;
        for (int i = 0; i < 8; i++) f |= (uint64_t)b[2 + i] << (8 * i);
        const int8_t* p = reinterpret_cast<const int8_t*>(b + kHeader);
        for (size_t k = 0; k < kN; k++) tmp[k] = cf32(p[2 * k] * (1.0f / 128), p[2 * k + 1] * (1.0f / 128));
        cb((double)f + offsetHz, tmp, kN);
        good++;
    }
    return good;
}

FwSweepPlan planFirmwareSweep(const FastPlanConfig& wanted, const RadioProfile& r) {
    FwSweepPlan p;
    p.plan = wanted;
    double lo = 1e18, hi = 0;
    for (const FreqRange& b : wanted.bands) if (b.hiHz > b.loHz) { lo = std::min(lo, b.loHz); hi = std::max(hi, b.hiHz); }
    if (hi <= lo) return p;
    const double hop = r.rateHz * r.usableFrac;                  // 15 MHz at 20 Msps
    p.stepHz = hop; p.offsetHz = hop / 2;
    const double lo0 = std::max(1.0, std::floor(lo / 1e6));
    const double n = std::max(1.0, std::ceil((hi - lo0 * 1e6) / hop));
    p.loMhz = (uint16_t)lo0;
    p.hiMhz = (uint16_t)std::min(7250.0, lo0 + n * hop / 1e6);
    p.plan.bands = {{lo0 * 1e6, lo0 * 1e6 + n * hop - r.rateHz / 512.0}};   // one grid starting on the whole MHz; the firmware steps are the hops
    return p;
}

bool HackrfFwSweep::open(const FwSweepPlan& p, double lnaDb, double vgaDb, bool amp, const std::string& serial, std::string& err) {
    close();
    int r = hackrf_init();
    if (r != HACKRF_SUCCESS) { err = std::string("hackrf_init: ") + hackrf_error_name((hackrf_error)r); return false; }
    hackrf_device* d = nullptr;
    r = serial.empty() ? hackrf_open(&d) : hackrf_open_by_serial(serial.c_str(), &d);
    if (r != HACKRF_SUCCESS) { err = std::string("open: ") + hackrf_error_name((hackrf_error)r); hackrf_exit(); return false; }
    auto fail = [&](const char* what, int rc) { err = std::string(what) + ": " + hackrf_error_name((hackrf_error)rc); hackrf_close(d); hackrf_exit(); return false; };
    if ((r = hackrf_set_sample_rate(d, 20e6)) != HACKRF_SUCCESS) return fail("set_sample_rate", r);   // exact on the Pro (the 10-20 Msps shift quirk does not apply)
    if ((r = hackrf_set_baseband_filter_bandwidth(d, 15000000)) != HACKRF_SUCCESS) return fail("set_baseband_filter", r);
    if ((r = hackrf_set_lna_gain(d, (uint32_t)lnaDb)) != HACKRF_SUCCESS) return fail("set_lna_gain", r);
    if ((r = hackrf_set_vga_gain(d, (uint32_t)vgaDb)) != HACKRF_SUCCESS) return fail("set_vga_gain", r);
    if ((r = hackrf_set_amp_enable(d, amp ? 1 : 0)) != HACKRF_SUCCESS) return fail("set_amp_enable", r);
    uint16_t range[2] = {p.loMhz, p.hiMhz};
    r = hackrf_init_sweep(d, range, 1, kSweepBlockBytes, (uint32_t)p.stepHz, (uint32_t)p.offsetHz, LINEAR);
    if (r != HACKRF_SUCCESS) return fail("init_sweep (firmware without sweep support)", r);
    offsetHz_ = p.offsetHz;
    dev_ = d;
    open_ = true;
    r = hackrf_start_rx_sweep(d, (hackrf_sample_block_cb_fn)&HackrfFwSweep::cb, this);
    if (r != HACKRF_SUCCESS) { open_ = false; dev_ = nullptr; return fail("start_rx_sweep", r); }
    return true;
}

void HackrfFwSweep::close() {
    if (!open_) return;
    open_ = false;
    hackrf_device* d = (hackrf_device*)dev_;
    dev_ = nullptr;
    hackrf_stop_rx(d);
    hackrf_close(d);
    hackrf_exit();
    cv_.notify_all();
}

int HackrfFwSweep::cb(void* t) {
    hackrf_transfer* tr = (hackrf_transfer*)t;
    auto* self = static_cast<HackrfFwSweep*>(tr->rx_ctx);
    parseSweepBuffer(tr->buffer, (size_t)tr->valid_length, self->offsetHz_, [&](double c, const cf32* s, size_t n) { self->push(c, s, n); });
    return 0;
}

void HackrfFwSweep::push(double c, const cf32* s, size_t n) {
    std::lock_guard<std::mutex> l(m_);
    if (q_.size() >= 4096) { q_.pop_front(); dropped_++; }     // the consumer fell behind: the oldest look goes
    q_.push_back({c, std::vector<cf32>(s, s + n)});
    cv_.notify_one();
}

bool HackrfFwSweep::next(double& centerHz, std::vector<cf32>& out, const std::atomic<bool>* cancel, std::string& err) {
    std::unique_lock<std::mutex> l(m_);
    int idle = 0;
    while (q_.empty()) {
        if (!open_) { err = "the radio stopped"; return false; }
        if (cancel && cancel->load()) return false;
        if (!cv_.wait_for(l, std::chrono::milliseconds(200), [&] { return !q_.empty() || !open_; }) && ++idle > 15) { err = "no sweep data from the radio for 3 s"; return false; }
    }
    Blk b = std::move(q_.front());
    q_.pop_front();
    centerHz = b.c;
    out = std::move(b.s);
    return true;
}

}  // namespace fastscan
}  // namespace dect2
