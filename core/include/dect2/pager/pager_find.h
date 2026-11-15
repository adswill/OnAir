// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pager finder (issue #269): scans a wide capture for narrowband pager carriers. It reuses the whole-span pager bank (CarrierFinder on a 25 kHz
// raster, one POCSAG / FLEX receiver per carrier, every speed tried at once): a carrier counts as a pager when its receiver decodes pages, and the
// speed with the most pages is the one reported (POCSAG 512 / 1200 / 2400 baud, FLEX 1600 / 3200 / 6400). Carriers that were seen but have decoded
// nothing yet are listed too, with no speed. The scan stops by itself after maxSec seconds of signal (the receivers cost CPU).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/pager/pager_tel.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct PagerCandidate {
    double hz = 0;                 // relative to the capture centre
    double absHz = 0;              // absolute (0 when the centre is unknown)
    double snrDb = 0;
    int speed = -1;                // PagerSpeed with the most pages, -1 when nothing was decoded
    uint64_t pages = 0;            // pages decoded on this carrier (all speeds)
    uint64_t speedPages[kPagerSpeeds] = {};
    uint32_t lastCapcode = 0;      // address of the newest page (0 when none)
};

class PagerFinder {
public:
    PagerFinder();
    ~PagerFinder();
    bool configure(double rate, double centreHz, double maxSec = 90);   // false when the capture is too narrow for a carrier search (< 400 kHz)
    void reset();                                  // forget the candidates and start again
    void feed(const cf32* x, size_t n);            // the analysis thread
    std::vector<PagerCandidate> rows() const;      // decoded ones first (most pages), then by SNR; any thread
    double seconds() const;                        // signal time scanned
    bool done() const;                             // maxSec reached: feed() does nothing more
    void setLimit(double maxSec);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
