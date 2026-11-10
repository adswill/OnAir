// Satellite TV feed hunter (see feed_hunter.h).
#include "dect2/satmisc/feed_hunter.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace satmisc {

void FeedHunter::reset() { feeds_.clear(); tryIdx_ = -1; startSec_ = -1; }

int FeedHunter::findFeed(double centreHz, double hz, double rate) const {
    for (size_t i = 0; i < feeds_.size(); i++) {
        const double abs1 = centreHz > 0 ? centreHz + hz : hz;
        const double abs2 = centreHz > 0 ? feeds_[i].mhz * 1e6 : feeds_[i].offsetHz;
        if (std::fabs(abs1 - abs2) < std::max(30e3, 0.2 * rate) && std::fabs(rate - feeds_[i].rateHz) < 0.2 * rate) return (int)i;
    }
    return -1;
}

void FeedHunter::update(double now, const std::vector<FeedCarrierIn>& in, double centre, bool lock, const std::string& service) {
    if (startSec_ < 0) startSec_ = now;
    for (auto& f : feeds_) f.present = false;
    for (const auto& c : in) {
        if (c.rateHz <= 0 || c.rateHz > maxRateHz) continue;
        int i = findFeed(centre, c.offsetHz, c.rateHz);
        if (i < 0) {
            Feed f;
            f.firstSec = now;
            f.isNew = now - startSec_ > baselineSec;
            feeds_.push_back(f);
            i = (int)feeds_.size() - 1;
        }
        Feed& f = feeds_[(size_t)i];
        f.offsetHz = c.offsetHz; f.rateHz = c.rateHz; f.snrDb = c.snrDb; f.lastSec = now; f.present = true;
        f.mhz = centre > 0 ? (centre + c.offsetHz) / 1e6 : c.offsetHz / 1e6;
    }
    if (tryIdx_ >= 0) {
        Feed& f = feeds_[(size_t)tryIdx_];
        if (lock) {
            f.state = FeedState::Locked; if (!service.empty()) f.service = service;
            // wait a little longer for a programme name, then go on
            if (now - tryStart_ > dwellSec || !service.empty()) tryIdx_ = -1;
        } else if (now - tryStart_ > dwellSec || !f.present) { f.state = FeedState::NoLock; tryIdx_ = -1; }
    }
    if (tryIdx_ < 0)
        for (size_t i = 0; i < feeds_.size(); i++)
            if (feeds_[i].state == FeedState::Untried && feeds_[i].present) { tryIdx_ = (int)i; feeds_[i].state = FeedState::Trying; tryStart_ = now; break; }
}

bool FeedHunter::finishedRound() const {
    for (const auto& f : feeds_) if (f.state == FeedState::Untried && f.present) return false;
    return tryIdx_ < 0;
}

void FeedHunter::retry() { for (auto& f : feeds_) if (f.state != FeedState::Locked) f.state = FeedState::Untried; tryIdx_ = -1; }

} // namespace satmisc
} // namespace dect2
