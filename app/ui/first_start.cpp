// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The first Start of a mode: FM and DAB find the stations and play one, the TV modes scan the country's plan and play a service, Airband loads the nearest
// airport. It happens once per mode (prefs "firstStart.*"), only with a real radio, an empty list and the mode's default frequency, and says what it did.
// docs/EASE-3.8.md. The decisions are in ease_logic.h.
#include "../app.h"
#include "ease_logic.h"
#include "typography.h"
#include "imgui.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/scanner.h"
#include "dect2/gnss/gnss_tel.h"
#include "dect2/prop/prop.h"
#include <ctime>
#include <cmath>
#include <string>
#include <vector>

void fmAutoScan(App& a);        // fm_ui.cpp
void dabAutoScan(App& a);       // dab_ui.cpp
void fmTune(App& a, double mhz);
bool airbandFirstLoad(App& a, std::string& what);   // airband_ui.cpp

namespace {

enum class Step { Idle, FmScan, DabScan, DabPlay, TvScan, TvPlay };
struct {
    Step step = Step::Idle;
    double t0 = 0, readyAt = 0;
    int fam = -1;
    double backMhz = 0;
    std::string msg;
    bool canUndo = false;
    double msgUntil = 0;
    double dabMhz = 0;
    bool gnssUsed = false, gnssOffered = false, gnssUndo = false, gnssOffer = false, gnssHadPos = false;
    double gnssLat = 0, gnssLon = 0, gnssBackLat = 0, gnssBackLon = 0;
} S;

const char* tvKey(const App& a) { return a.atscMode ? "atsc" : a.isdbtMode ? "isdbt" : a.family == 7 ? "dtmb" : "dvb"; }
bool tvMode(const App& a) { return !a.fmMode && !a.dabMode && !a.atsc3Mode && (a.family == 0 || a.family == 1 || a.family == 4 || a.family == 7); }

std::string keyOf(const App& a) {
    if (a.fmMode) return "firstStart.fm";
    if (a.dabMode) return "firstStart.dab";
    if (a.family == 26) return "firstStart.airband";
    if (tvMode(a)) return std::string("firstStart.tv.") + tvKey(a);
    return "";
}
void say(const std::string& m, bool undo = false) { S.msg = m; S.canUndo = undo; S.msgUntil = ImGui::GetTime() + 90; }
void finish() { S.step = Step::Idle; }

bool startTvScan(App& a) {
    std::vector<TvRange> r = tvBandRanges(a);
    if (r.empty()) return false;
    a.scanCfg.startMHz = r[0].startMhz; a.scanCfg.stopMHz = r[0].stopMhz; a.scanCfg.stepMHz = r[0].stepMhz; a.scanCfg.bwMhz = r[0].bwMhz;
    a.scanCfg.tune = a.tune;
    a.scanCfg.atsc = a.atscMode && !a.isdbtMode && !a.atsc3Mode;
    a.scanCfg.atsc3 = a.atsc3Mode;
    a.scanCfg.isdbt = a.isdbtMode;
    a.scanCfg.dtmb = a.family == 7;
    if (a.family == 7) a.scanCfg.bwMhz = a.dtmbBwMhz;
    std::string err;
    if (!Scanner::check(a.devices[a.devIdx], a.scanCfg, err)) { say(tr("Automatic scan not possible: ") + err); return false; }
    a.scanWasRunning = true;
    a.engine.stop();
    const DeviceInfo radio = a.devices[a.devIdx];
    if (!a.scanner.start(radio, a.scanCfg, err)) { say(tr("Automatic scan not possible: ") + err); a.scanWasRunning = true; return false; }
    return true;
}

void onReady(App& a) {
    const std::string key = keyOf(a);
    if (key.empty() || S.step != Step::Idle) return;
    const ModeTuning* mt = modeTuning(engineStd(a));
    const double defMhz = mt ? mt->defMhz : a.fmMode ? 100.0 : a.dabMode ? 218.640 : a.family == 1 ? 473.0 : a.family == 4 ? 473.143 : 522.0;   // app_state.cpp's defaults of the first modes
    ease::FirstIn in;
    in.done = plat::prefs().getB(key.c_str(), false);
    in.realRadio = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio();
    in.atDefaultFreq = std::fabs(a.freqMhz - defMhz) < 0.05;
    in.recording = a.engine.recordingStats().active;
    in.transmitting = txActive();
    in.busy = a.scanner.progress().running || a.fmScan.running || a.dabScan.running;
    if (a.fmMode) in.emptyList = a.fmScan.results.empty() && a.fmSaved.empty();
    else if (a.dabMode) in.emptyList = a.dabScan.results.empty();
    else if (a.family == 26) in.emptyList = receiverLocationSet();   // airbandFirstLoad checks its own list
    else in.emptyList = a.channels.empty();
    if (!ease::firstScanWanted(in)) return;
    plat::prefs().setB(key.c_str(), true); plat::prefs().flush();
    S.backMhz = a.freqMhz; S.t0 = ImGui::GetTime();
    if (a.fmMode) { fmAutoScan(a); S.step = Step::FmScan; say(tr("First start: looking for FM stations, about two minutes.")); }
    else if (a.dabMode) { dabAutoScan(a); S.step = Step::DabScan; say(tr("First start: looking for DAB stations, up to a minute.")); }
    else if (a.family == 26) {
        std::string what;
        if (airbandFirstLoad(a, what)) say(what, false);
    } else if (startTvScan(a)) { S.step = Step::TvScan; say(tr("First start: scanning your country's TV channels, this takes a few minutes.")); }
}

}  // namespace

// E9: the first time a shortwave mode opens, a band that is open now (only with a location and a real radio, and never over a frequency the mode remembers)
void firstBandPick(App& a) {
    const char* name = curModeName(a);
    const std::vector<ease::HfBand> bands = ease::hfBandsOf(name);
    if (bands.empty() || !receiverLocationSet()) return;
    if (!(a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio())) return;
    const int slot = modeSlot(a);
    if (a.family >= 6 && slot >= 0 && slot < kNumSlots && a.famFreq[slot] > 0) return;
    const std::string key = std::string("firstBand.") + name;
    plat::Prefs& p = plat::prefs();
    if (p.getB(key.c_str(), false)) return;
    p.setB(key.c_str(), true); p.flush();
    double lat, lon;
    receiverLocation(lat, lon);
    const std::time_t tt = std::time(nullptr);
    std::tm t = *std::gmtime(&tt);
    dect2::prop::Params pp;
    pp.txLat = lat; pp.txLon = lon;
    pp.year = t.tm_year + 1900; pp.month = t.tm_mon + 1; pp.day = t.tm_mday; pp.utcHour = t.tm_hour + t.tm_min / 60.0;
    pp.sfi = 130;       // a mid-cycle guess: no live solar data is used for the first pick
    const double muf = dect2::prop::mufMHz(lat, lon, 1500, pp);
    const int b = ease::pickHfBand(bands, muf);
    if (b < 0) return;
    int orig = 0;
    for (int i = 0; i < (int)bands.size(); i++) if (std::fabs(bands[i].mhz - a.freqMhz) < 0.5) orig = i;
    if (std::fabs(bands[b].mhz - a.freqMhz) < 0.01) return;
    S.backMhz = a.freqMhz;
    tuneFreq(a, bands[b].mhz);
    const std::string note = ease::hfBandSay(bands[b], bands[orig]);
    say(note.empty() ? std::string(bands[b].name) + tr(" is open now: tuned there.") : tr(note.c_str()), true);
}

void firstStartSay(const std::string& text) { say(text, false); }

void firstStartAfterStart(App&) { S.readyAt = ImGui::GetTime() + 1.5; S.fam = 0; }

// E12: a held GNSS fix: an unset station position is filled in (with Undo), a far one is offered
static void gnssPositionTick(App& a) {
    static double heldFrom = -1;
    const double now = ImGui::GetTime();
    const bool ok = a.family == 12 && a.engine.running() && a.rx.gnss.fix.valid && a.rx.gnss.fix.heightM > -500 && a.rx.gnss.fix.heightM < 12000;
    if (!ok) { heldFrom = -1; return; }
    if (heldFrom < 0) heldFrom = now;
    double lat = 0, lon = 0;
    const bool set = receiverLocationSet();
    receiverLocation(lat, lon);
    const dect2::GnssFix& f = a.rx.gnss.fix;
    double km = 0;
    if (set) { const double dLat = (f.latDeg - lat) * 111.2, dLon = (f.lonDeg - lon) * 111.2 * std::cos(lat * M_PI / 180); km = std::sqrt(dLat * dLat + dLon * dLon); }
    const ease::GnssPos act = ease::gnssPosAction(set, now - heldFrom, km);
    if (act == ease::GnssPos::UseIt && !S.gnssUsed) {
        S.gnssUsed = true; S.gnssBackLat = lat; S.gnssBackLon = lon; S.gnssHadPos = false;
        setReceiverLocation(a, f.latDeg, f.lonDeg);
        char b[120]; snprintf(b, sizeof b, tr("Station position set from this GNSS fix: %.4f, %.4f."), f.latDeg, f.lonDeg);
        say(b, false); S.gnssUndo = true;
    } else if (act == ease::GnssPos::Offer && !S.gnssOffered) {
        S.gnssOffered = true;
        char b[120]; snprintf(b, sizeof b, tr("Your GNSS fix is %.0f km from the station position."), km);
        say(b, false); S.gnssOffer = true; S.gnssLat = f.latDeg; S.gnssLon = f.lonDeg; S.gnssBackLat = lat; S.gnssBackLon = lon; S.gnssHadPos = set;
    }
}

void firstStartTick(App& a) {
    const double now = ImGui::GetTime();
    gnssPositionTick(a);
    if (S.fam == 0 && now >= S.readyAt) { S.fam = -1; if (a.engine.running()) onReady(a); }
    switch (S.step) {
        case Step::FmScan:
            if (!a.fmScan.running) {
                std::vector<float> snr; std::vector<int> idx;
                for (int i = 0; i < (int)a.fmScan.results.size(); i++) if (a.fmScan.results[i].found) { snr.push_back(a.fmScan.results[i].snr); idx.push_back(i); }
                const int b = ease::strongestIdx(snr);
                if (b >= 0 && a.engine.running()) {
                    const auto& r = a.fmScan.results[(size_t)idx[(size_t)b]];
                    fmTune(a, r.mhz);
                    char t[128]; snprintf(t, sizeof t, tr("Tuned to %.1f MHz %s, the strongest of %d stations."), r.mhz, r.name.c_str(), (int)snr.size());
                    say(t, true);
                } else say(tr("No FM stations found. Check the antenna and press Scan on the Scan tab."));
                finish();
            }
            break;
        case Step::DabScan:
            if (!a.dabScan.running) {
                std::vector<float> snr; std::vector<int> idx;
                for (int i = 0; i < (int)a.dabScan.results.size(); i++) if (a.dabScan.results[i].found) { snr.push_back(a.dabScan.results[i].snr); idx.push_back(i); }
                const int b = ease::strongestIdx(snr);
                if (b >= 0 && a.engine.running()) { S.dabMhz = a.dabScan.results[(size_t)idx[(size_t)b]].mhz; dabTuneMhz(a, S.dabMhz); S.step = Step::DabPlay; S.t0 = now; }
                else { say(tr("No DAB ensembles found. Check the antenna and press Scan on the Scan tab.")); finish(); }
            }
            break;
        case Step::DabPlay: {
            const DabEnsemble ens = a.engine.dabEnsemble();
            bool done = false;
            if (ens.valid) for (const auto& kv : ens.services) {
                for (const auto& c : kv.second.comps) if (c.tmid == 0 && !c.ca && c.subId >= 0) {
                    if (!kv.second.label.empty() || now - S.t0 > 6) {
                        a.dabUserSub = c.subId; dabSelectStation(a, c.subId, true);
                        say(tr("Playing ") + (kv.second.label.empty() ? std::string("DAB") : kv.second.label) + ".", true);
                        done = true;
                    }
                    break;
                }
                if (done) break;
            }
            if (done || now - S.t0 > 15 || !a.engine.running()) finish();
            break;
        }
        case Step::TvScan:
            if (!a.scanner.progress().running) {
                const SavedChannel* best = nullptr;
                for (const SavedChannel& c : a.channels) if (c.nServices > 0 && (!best || c.snrDb > best->snrDb)) best = &c;
                if (best) { const SavedChannel c = *best; tuneToChannel(a, c); S.step = Step::TvPlay; S.t0 = now; }
                else { say(tr("No TV channels found. Check the antenna, or set your country in Settings.")); finish(); }
            }
            break;
        case Step::TvPlay: {
            if (a.engine.player().selected() >= 0) { finish(); break; }   // the user picked one first
            std::vector<ease::SvcPick> v;
            std::vector<const dect2::TsService*> ptr;   // the telemetry's own list
            for (const auto& sv : a.ts.services) { v.push_back({sv.id, sv.lcn, sv.type == 0x02 || sv.type == 0x0A}); ptr.push_back(&sv); }
            const int b = ease::pickFirstService(v);
            if (b >= 0) {
                a.engine.player().select(v[(size_t)b].id); a.engine.player().setVolume(a.volume);
                say(tr("Playing ") + ptr[(size_t)b]->name + ".", true);
                finish();
            } else if (now - S.t0 > 30 || !a.engine.running()) finish();
            break;
        }
        default: break;
    }
}

void firstStartHint(App& a) {
    if (S.msg.empty() || ImGui::GetTime() > S.msgUntil) return;
    ImGui::TextDisabled("%s", S.msg.c_str());
    if (S.gnssOffer) {
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton(tr("Use this position##gnssoffer"))) { setReceiverLocation(a, S.gnssLat, S.gnssLon); S.gnssOffer = false; say(tr("Station position updated from the GNSS fix."), false); S.gnssUndo = true; }
        return;
    }
    if (S.gnssUndo) {
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton(tr("Undo##gnsspos"))) {
            if (S.gnssHadPos) setReceiverLocation(a, S.gnssBackLat, S.gnssBackLon);
            else receiverLocationClear(a, S.gnssBackLat, S.gnssBackLon);
            S.gnssUndo = false; S.msg.clear();
        }
        return;
    }
    if (S.step != Step::Idle) {
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton(tr("Stop##firststart"))) {
            if (S.step == Step::FmScan) { a.fmScan.running = false; fmTune(a, S.backMhz); }
            else if (S.step == Step::DabScan) { a.dabScan.running = false; dabTuneMhz(a, S.backMhz); }
            else if (S.step == Step::TvScan) a.scanner.stop();
            finish(); S.msg.clear();
        }
    } else if (S.canUndo) {
        ImGui::SameLine(0, 8 * gUi);
        char b[64]; snprintf(b, sizeof b, "%s %.3f##firstundo", tr("Back to"), S.backMhz);
        if (ImGui::SmallButton(b)) {
            if (a.fmMode) fmTune(a, S.backMhz);
            else if (a.dabMode) dabTuneMhz(a, S.backMhz);
            else tuneFreq(a, S.backMhz);
            S.msg.clear();
        }
    }
}
