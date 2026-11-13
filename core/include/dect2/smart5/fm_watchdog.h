// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM station watchdog (3.9, docs/SMART5-broadcast-3.9.md item 1): flags a broken station before the listener notices.
// Fed once per receiver report (a few times a second) with what the FM receiver already measures; no audio is needed.
//  - dead air: the carrier is clean but the peak deviation stays at the level of the pilot and RDS alone for deadAirSec
//  - pilot lost: a station that was stereo nearly all the time it was heard loses its pilot while the signal stays good
//  - RDS stuck: the radio text of a station that has changed it before has not changed for rdsStuckSec
//  - clock wrong: the RDS clock time (4A) is more than clockTolSec off the computer's clock; the drift per day per PI is fitted
//  - over-deviation: the peak deviation is above 75 kHz in more than overDevPct % of the recent reports
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
struct FmTelemetry;
namespace smart5 {

struct FmWatchInput {
    bool carrier = false;
    float snrDb = 0;
    float devKhz = 0;          // peak deviation of the last report
    bool stereo = false;
    float pilotPct = 0;        // pilot injection, % of 75 kHz
    int pi = 0;
    std::string ps, radioText;
    int64_t clockUtc = 0;      // RDS CT, 0 = none yet
    double clockAt = 0;        // computer time the CT arrived
};
FmWatchInput fmWatchInput(const FmTelemetry& t);

struct FmWatchParams {
    double deadAirSec = 12;
    double rdsStuckSec = 3 * 3600;
    double clockTolSec = 90;
    double pilotLostSec = 10;
    double overDevPct = 1.0;
    double goodSnrDb = 20;
    int overDevWindow = 600;   // reports
};

struct FmWatchEvent { double t = 0; int pi = 0; std::string kind, text; bool cleared = false; };

struct FmWatchStatus {
    bool deadAir = false, pilotLost = false, rdsStuck = false, clockWrong = false, overDeviation = false;
    double clockErrS = 0;          // last CT minus the computer clock (0 when none)
    double clockDriftSPerDay = 0;  // fitted over the CTs of this PI (0 with fewer than 3)
    int clockSamples = 0;
    double overDevSharePct = 0;
    double deadAirFor = 0;         // seconds the current silence has lasted
    std::string summary() const;   // "" when nothing is wrong
};

class FmWatchdog {
public:
    explicit FmWatchdog(FmWatchParams p = {}) : p_(p) {}
    void update(const FmWatchInput& in, double now);
    const FmWatchStatus& status() const { return st_; }
    const std::vector<FmWatchEvent>& events() const { return ev_; }
    void reset();
private:
    struct PiState {
        double heardSec = 0, stereoSec = 0;
        std::string lastRt; double rtSince = 0; int rtChanges = 0;
        std::vector<std::pair<double, double>> ct;   // (computer time, error)
        double lastClockAt = 0;
    };
    void raise(bool& flag, bool now, double t, int pi, const char* kind, const std::string& text);
    FmWatchParams p_;
    FmWatchStatus st_;
    std::vector<FmWatchEvent> ev_;
    std::map<int, PiState> pis_;
    double last_ = -1, silentSince_ = -1, noPilotSince_ = -1;
    std::vector<uint8_t> over_; size_t overPos_ = 0; int overN_ = 0;
};

} // namespace smart5
} // namespace dect2
