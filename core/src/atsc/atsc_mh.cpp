// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/atsc/atsc_mh.h"

namespace dect2 {

namespace { constexpr uint32_t kGroup = 118, kSlot = 156; constexpr uint64_t kConfirm = 6; }

void AtscMhDetector::endRun() {
    if (run_ == kGroup && pid_ >= 0) {
        const bool again = pid_ == candPid_ && (runStart_ - lastStart_) % kSlot == 0 && runStart_ != lastStart_;
        if (!again) { candPid_ = pid_; firstStart_ = runStart_; candGroups_ = 0; }
        lastStart_ = runStart_;
        candGroups_++;
        if (candGroups_ >= kConfirm) {
            st_.present = true; st_.pid = candPid_; st_.groups = candGroups_;
            st_.slotShare = (double)candGroups_ / (double)((lastStart_ - firstStart_) / kSlot + 1);
        }
    }
    run_ = 0;
}

void AtscMhDetector::feed(const uint8_t* p, size_t count) {
    for (size_t i = 0; i < count; i++, p += 188, pos_++) {
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        const bool err = (p[0] != 0x47) || (p[1] & 0x80);
        if (err || pid == 0x1FFF) { endRun(); pid_ = -1; continue; }
        if (pid != pid_) { endRun(); pid_ = pid; runStart_ = pos_; }
        run_++;
    }
}

} // namespace dect2
