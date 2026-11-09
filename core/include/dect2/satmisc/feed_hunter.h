// Satellite TV feed hunter: watches the carrier list of the DVB-S receiver (the "carriers in the spectrum" survey) and tries every narrow carrier in turn.
// A contribution feed is a small carrier (typically a few Msym/s) that appears and goes; the hunter records when each was first and last seen, flags those
// that were not there when the hunt began, and leaves what the DVB-S receiver reports as the carrier's verdict. Finding a carrier does not make it DVB-S:
// one that never locks is shown as "no lock" (it can be another standard, for example ABS-S, or encrypted at a lower layer), never guessed at.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace satmisc {

struct FeedCarrierIn { double offsetHz = 0, rateHz = 0, snrDb = 0; };

enum class FeedState { Untried, Trying, Locked, NoLock };

struct Feed {
    double mhz = 0;            // absolute when the centre is known, else offset / 1e6
    double offsetHz = 0;
    double rateHz = 0;
    double snrDb = 0;
    double firstSec = 0, lastSec = 0;
    bool isNew = false;        // appeared after the hunt began
    bool present = false;
    FeedState state = FeedState::Untried;
    std::string service;       // the first programme name, when the receiver locked and reported one
};

class FeedHunter {
public:
    void reset();
    double maxRateHz = 12e6;   // wider carriers are programmes, not feeds (Advanced can raise it)
    double dwellSec = 20;
    double baselineSec = 30;   // carriers seen in this first part of the hunt are not "new"
    // One update per telemetry report: the carriers now, the radio's centre, and whether the receiver has transport stream lock on what it was asked to try
    void update(double nowSec, const std::vector<FeedCarrierIn>& carriers, double centreHz, bool tsLock, const std::string& service);
    bool hasRequest() const { return tryIdx_ >= 0; }
    double requestOffsetHz() const { return tryIdx_ >= 0 ? feeds_[(size_t)tryIdx_].offsetHz : 0.0; }
    const std::vector<Feed>& feeds() const { return feeds_; }
    bool finishedRound() const;
    void retry();              // mark all not locked as untried again
private:
    int findFeed(double centreHz, double hz, double rate) const;
    std::vector<Feed> feeds_;
    int tryIdx_ = -1;
    double startSec_ = -1, tryStart_ = 0;
};

} // namespace satmisc
} // namespace dect2
