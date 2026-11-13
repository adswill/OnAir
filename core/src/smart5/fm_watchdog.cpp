// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM station watchdog (see fm_watchdog.h).
#include "dect2/smart5/fm_watchdog.h"
#include "dect2/fm/fm_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace smart5 {

FmWatchInput fmWatchInput(const FmTelemetry& t) {
    FmWatchInput in;
    in.carrier = t.carrier; in.snrDb = t.snrDb; in.devKhz = t.devKhz; in.stereo = t.stereo; in.pilotPct = t.pilotPct;
    in.pi = t.piCode; in.ps = t.psName; in.radioText = t.radioText; in.clockUtc = t.rds.clockUtc; in.clockAt = t.rds.clockAt;
    return in;
}

std::string FmWatchStatus::summary() const {
    std::string s;
    auto add = [&](const std::string& x) { if (!s.empty()) s += "; "; s += x; };
    char b[96];
    if (deadAir) { snprintf(b, sizeof b, "dead air for %.0f s", deadAirFor); add(b); }
    if (pilotLost) add("stereo pilot lost");
    if (rdsStuck) add("RDS text frozen");
    if (clockWrong) { snprintf(b, sizeof b, "RDS clock %+.0f s off", clockErrS); add(b); }
    if (overDeviation) { snprintf(b, sizeof b, "over-deviation %.1f %% of the time", overDevSharePct); add(b); }
    return s;
}

void FmWatchdog::reset() { st_ = {}; ev_.clear(); pis_.clear(); last_ = -1; silentSince_ = noPilotSince_ = -1; over_.clear(); overPos_ = 0; overN_ = 0; }

void FmWatchdog::raise(bool& flag, bool now, double t, int pi, const char* kind, const std::string& text) {
    if (flag == now) return;
    flag = now;
    ev_.push_back({t, pi, kind, now ? text : std::string(kind) + " cleared", !now});
    if (ev_.size() > 500) ev_.erase(ev_.begin(), ev_.begin() + 100);
}

void FmWatchdog::update(const FmWatchInput& in, double now) {
    const double dt = last_ < 0 ? 0 : std::clamp(now - last_, 0.0, 5.0);
    last_ = now;
    const bool good = in.carrier && in.snrDb >= p_.goodSnrDb;
    if (!in.carrier) {   // nothing to judge without a station; keep the per-PI memory
        silentSince_ = noPilotSince_ = -1;
        st_.deadAirFor = 0;
        raise(st_.deadAir, false, now, in.pi, "dead air", "");
        raise(st_.pilotLost, false, now, in.pi, "pilot", "");
        return;
    }
    PiState& ps = pis_[in.pi];
    ps.heardSec += dt;
    if (in.stereo) ps.stereoSec += dt;

    // dead air: pilot (pilotPct of 75 kHz) and RDS (about 2 - 4 kHz) make up the deviation of a silent programme
    const double floorKhz = std::max(12.0, 0.75 * in.pilotPct + 8.0);
    if (good && in.devKhz < floorKhz) { if (silentSince_ < 0) silentSince_ = now; } else silentSince_ = -1;
    st_.deadAirFor = silentSince_ < 0 ? 0 : now - silentSince_;
    char b[160];
    snprintf(b, sizeof b, "dead air: peak deviation below %.0f kHz for %.0f s", floorKhz, st_.deadAirFor);
    raise(st_.deadAir, st_.deadAirFor >= p_.deadAirSec, now, in.pi, "dead air", b);

    // pilot lost: the station was stereo at least 90 % of the time over a minute before
    const bool usuallyStereo = ps.heardSec - ps.stereoSec < 0.1 * ps.heardSec && ps.heardSec > 60;
    if (good && !in.stereo && in.pilotPct < 1) { if (noPilotSince_ < 0) noPilotSince_ = now; } else noPilotSince_ = -1;
    raise(st_.pilotLost, noPilotSince_ >= 0 && now - noPilotSince_ >= p_.pilotLostSec && usuallyStereo, now, in.pi, "pilot", "stereo pilot lost on a station that is normally stereo");

    // RDS text stuck
    if (!in.radioText.empty()) {
        if (in.radioText != ps.lastRt) { if (!ps.lastRt.empty()) ps.rtChanges++; ps.lastRt = in.radioText; ps.rtSince = now; }
        raise(st_.rdsStuck, ps.rtChanges >= 1 && now - ps.rtSince >= p_.rdsStuckSec, now, in.pi, "RDS stuck", "radio text unchanged for " + std::to_string((int)((now - ps.rtSince) / 60)) + " min on a station that changes it");
    }

    // RDS clock
    if (in.clockUtc > 0 && in.clockAt > 0 && in.clockAt != ps.lastClockAt) {
        ps.lastClockAt = in.clockAt;
        const double err = (double)in.clockUtc - in.clockAt;
        ps.ct.push_back({in.clockAt, err});
        if (ps.ct.size() > 400) ps.ct.erase(ps.ct.begin());
        st_.clockErrS = err;
        st_.clockSamples = (int)ps.ct.size();
        if (ps.ct.size() >= 3) {   // least-squares slope, seconds per day
            double mt = 0, me = 0;
            for (auto& c : ps.ct) { mt += c.first; me += c.second; }
            mt /= ps.ct.size(); me /= ps.ct.size();
            double sxy = 0, sxx = 0;
            for (auto& c : ps.ct) { sxy += (c.first - mt) * (c.second - me); sxx += (c.first - mt) * (c.first - mt); }
            st_.clockDriftSPerDay = sxx > 0 && ps.ct.back().first - ps.ct.front().first > 600 ? sxy / sxx * 86400 : 0;
        }
        snprintf(b, sizeof b, "RDS clock is %+.0f s off the computer clock", err);
        raise(st_.clockWrong, std::fabs(err) > p_.clockTolSec, now, in.pi, "clock", b);
    }

    // over-deviation share
    if ((int)over_.size() != p_.overDevWindow) { over_.assign(p_.overDevWindow, 0); overPos_ = 0; overN_ = 0; }
    const uint8_t o = in.devKhz > 75.0f ? 1 : 0;
    if (over_[overPos_]) overN_--;
    over_[overPos_] = o; overN_ += o;
    overPos_ = (overPos_ + 1) % over_.size();
    st_.overDevSharePct = 100.0 * overN_ / over_.size();
    snprintf(b, sizeof b, "over-deviation: above 75 kHz in %.1f %% of reports", st_.overDevSharePct);
    raise(st_.overDeviation, st_.overDevSharePct > p_.overDevPct, now, in.pi, "over-deviation", b);
}

} // namespace smart5
} // namespace dect2
