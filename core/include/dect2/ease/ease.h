// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// First-use helpers that need no radio: when balloons fly, which geostationary satellite is highest, what a pick should leave alone.
// Pure functions, no I/O; the screens that use them are in app/ease and in each mode's UI file.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dect2::ease {

// ---- Radiosonde launch windows. Synoptic soundings are timed for 00 and 12 UTC, so most sites release near 23:15 and 11:15 UTC. A site's
// real time can differ (extra launches, other schedules), so this is only a rule of thumb, and the text says so.
struct SondeWindow {
    bool inWindow = false;     // from 30 minutes before the usual launch until the flight is normally over, 2 hours after it
    int secsToLaunch = 0;      // to the launch of this window (negative once it has passed) or of the next one
    int64_t id = 0;            // the number of the window: changes once per window
};
SondeWindow sondeWindow(int64_t unixUtc);
// Whether the optional automatic start should fire now: once per window, never over a running receiver. lastFired is the id of the last window
// that used its start (or the one in which the user turned the option on, so a stop by hand is respected).
bool sondeAutoStartDue(const SondeWindow& w, int64_t lastFired, bool optionOn, bool anythingRunning);

// ---- The highest of a list of geostationary slots from a position. -1 when none is at least minElDeg above the horizon.
struct GeoPick { int index = -1; double azDeg = 0, elDeg = 0; };
GeoPick highestGeo(double latDeg, double lonDeg, const double* slotLonDeg, int count, double minElDeg = 5.0);
const char* compass8(double azDeg);    // "north", "north-east", ... (English: pass through tr())

// What a preselection may do on this visit.
enum class Preselect {
    Wait,           // nothing yet: no position, or the receiver is running
    Pick,           // choose and tune
    LeaveAlone,     // the user already tuned elsewhere: remember that and never ask again
    Done            // decided on an earlier visit
};
Preselect preselectAction(bool prefExists, bool positionKnown, bool running, double freqMhz, double defMhz);

// ---- ISM auto band: stay where decodes arrive, keep sampling the others. One radio can still miss a device that sends rarely.
std::vector<double> ismBandsFor(const std::string& country);   // MHz, the first is tuned first
class AutoBand {
public:
    AutoBand(std::vector<double> bandsMhz, double dwellS = 30.0, double stayS = 300.0);
    // Call with a monotonic clock and the running count of decodes. Returns the index of the band to be on.
    int update(double nowS, uint64_t decodesTotal);
    bool settled() const { return settled_; }
    double bandMhz(int i) const { return bands_[(size_t)i]; }
    int count() const { return (int)bands_.size(); }
    double score(int i) const { return scores_[(size_t)i]; }
private:
    int best() const;
    std::vector<double> bands_, scores_;
    double dwell_, stay_, since_ = -1;
    uint64_t last_ = 0;
    bool haveLast_ = false, settled_ = false, sampling_ = false;
    int cur_ = 0, sampleNext_ = 0;
};

// ---- "Find ..." over a list of known frequencies: one pass, stops where a decode arrives.
class FreqHunt {
public:
    explicit FreqHunt(std::vector<double> mhz, double dwellS = 20.0) : mhz_(std::move(mhz)), dwell_(dwellS) {}
    int update(double nowS, uint64_t decodesTotal);   // the index to be on
    bool found() const { return found_; }
    bool finished() const { return finished_; }      // a whole pass with nothing heard
    double mhz(int i) const { return mhz_[(size_t)i]; }
    int size() const { return (int)mhz_.size(); }
private:
    std::vector<double> mhz_;
    double dwell_, since_ = -1;
    uint64_t base_ = 0;
    bool started_ = false, found_ = false, finished_ = false;
    int cur_ = 0;
};
// Whether a frequency-table region (GLOBAL, EU, NA, UK, or a country code) applies to a country; true when the country is unknown.
bool freqRegionApplies(const std::string& region, const std::string& country);

// ---- Find DRM: carriers the census called DRM, across the bands visited. A channel seen again keeps its best SNR; list best first.
struct DrmHit { double khz = 0; float snrDb = 0; };
void addDrmHit(std::vector<DrmHit>& hits, double khz, float snrDb);

// ---- Dish alignment: the level bar, peak hold and the tone a level maps to.
class AlignMeter {
public:
    void update(float db, bool valid);
    void resetPeak() { havePeak_ = false; }
    float level() const { return level_; }
    bool hasPeak() const { return havePeak_; }
    float peak() const { return peak_; }
    float fraction() const;                 // 0..1 of the bar (0 to 30 dB)
    float peakFraction() const;
    double toneHz() const;                  // 300 Hz at 0 dB rising to 1500 Hz at 30 dB
private:
    float level_ = 0, peak_ = 0;
    bool havePeak_ = false;
};

// ---- One callsign, one position. Fills a callsign or locator that is still empty, from the first known callsign and the station position (else a known
// grid); what the user typed is never replaced. Returns true when anything was filled.
bool stationFill(std::string& call, std::string& locator, const std::vector<std::string>& knownCalls, const std::string& knownGrid, bool havePos, double lat, double lon);

// ---- Regional modes: true when this mode's signals exist only elsewhere. False for every mode without a tag and when the country (ISO code) is unknown.
bool modeNotUsedIn(const std::string& modeName, const std::string& country);

// ---- Weather fax "Follow the schedule". A chart under way is never cut off to reach the next one; a broadcast is tuned for once.
struct WefaxSlot { std::string station; long long startUtc = 0; };
// The first broadcast still worth tuning for (late by at most lateS seconds), optionally of one station; -1 when none.
int wefaxFollowPick(const std::vector<WefaxSlot>& slots, const std::string& stationId, long long nowUtc, int lateS = 60);
bool wefaxFollowDue(long long startUtc, long long nowUtc, int marginS, long long tunedForStart, bool chartUnderWay);

// ---- DSC MF/HF: the kHz of a proposed working frequency ("8291.0 kHz"), 0 when the text is not a plausible MF/HF frequency.
double dscWorkingKhz(const std::string& text);

} // namespace dect2::ease
