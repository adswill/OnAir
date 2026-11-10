// Full control over transmitting (docs/TX-CONTROL-3.8.md), the parts of the Advanced block that belong to the radio and the clock rather than to the
// signal: the analogue filter and the gain stages, the per-radio TX calibration (frequency correction, IQ balance, carrier leak, power vs gain table), the
// predicted output power, and the schedule (start at a time, repeat). The owner's TX rules hold throughout: every value is sent as typed (clamped only to the
// radio's own reported scale), a value outside the radio's range is a note, never a refusal; the operator's Start is the decision.
#include "tx_state.h"
#include "../platform/platform.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace txui;

namespace {

std::string calPrefKey(const std::string& radioKey) {
    std::string k = "tx.cal.";
    for (char c : radioKey) k += (std::isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
    return k;
}

double nowUtc() { return (double)time(nullptr); }

std::string utcText(double t) {
    const time_t tt = (time_t)t;
    struct tm g{};
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[32];
    strftime(b, sizeof b, "%H:%M:%S UTC", &g);
    return b;
}

float labelWidth() { return std::max(140.f * gUi, ImGui::GetContentRegionAvail().x * 0.42f); }

void rowLabel(const char* label, const char* tip) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(320 * gUi);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::SameLine(labelWidth());
    ImGui::SetNextItemWidth(-1);
}

TxSchedule scheduleOf(const TxUi& s) {
    TxSchedule sc;
    if (s.schedAt[0]) sc.startUtc = txParseUtcTime(s.schedAt, nowUtc());
    if (sc.startUtc < 0) sc.startUtc = 0;
    sc.everySec = std::max(0.0, s.schedEverySec);
    sc.count = std::max(0, s.schedCount);
    return sc;
}

void saveControl(const TxUi& s) {
    plat::Prefs& p = plat::basePrefs();
    p.setD("tx.filterKhz", s.filterKhz);
    p.setS("tx.stageGains", s.stageGains);
    p.setS("tx.schedAt", s.schedAt);
    p.setD("tx.schedEvery", s.schedEverySec);
    p.setI("tx.schedCount", s.schedCount);
}

} // namespace

namespace txui {

TxCalibration calibrationFor(const std::string& radioKey) {
    if (radioKey.empty()) return {};
    return txCalFromString(plat::basePrefs().getS(calPrefKey(radioKey).c_str(), ""));
}

void saveCalibration(const std::string& radioKey, const TxCalibration& c) {
    if (radioKey.empty()) return;
    plat::basePrefs().setS(calPrefKey(radioKey).c_str(), c.empty() ? std::string() : txCalToString(c));
}

void loadControlPrefs(TxUi& s) {
    if (s.radioLoaded) return;
    s.radioLoaded = true;
    plat::Prefs& p = plat::basePrefs();
    s.filterKhz = std::max(0.0, p.getD("tx.filterKhz", 0));
    snprintf(s.stageGains, sizeof s.stageGains, "%s", p.getS("tx.stageGains", "").c_str());
    snprintf(s.schedAt, sizeof s.schedAt, "%s", p.getS("tx.schedAt", "").c_str());
    s.schedEverySec = std::max(0.0, p.getD("tx.schedEvery", 0));
    s.schedCount = std::max(0, (int)p.getI("tx.schedCount", 0));
    s.targetDbm = p.getD("tx.targetDbm", 10);
}

void resetControl(TxUi& s) {
    s.filterKhz = 0;
    s.stageGains[0] = 0;
    s.schedAt[0] = 0;
    s.schedEverySec = 0;
    s.schedCount = 0;
    s.schedArmed = false;
    saveControl(s);
}

// the radio's choices and its calibration into a request. An IQ file is not a radio: its mixer needs no correction and it has no filter or gain stages.
void controlApply(const TxUi& s, TxRequest& r) {
    if (s.destKind == 1) return;
    if (s.filterKhz > 0) r.tune.bandwidthHz = s.filterKhz * 1e3;
    if (s.stageGains[0]) txParseStageGains(s.stageGains, r.tune.radio);
    txApplyCalibration(calibrationFor(s.radioKey), r);   // a test (no RF) shows what the radio would be sent, its correction included
}

void radioControlPanel(TxUi& s, const DeviceInfo* radio, bool running) {
    loadControlPrefs(s);
    bool ch = false;
    ImGui::BeginDisabled(running);
    {
        bool autoBw = s.filterKhz <= 0;
        rowLabel(tr("Radio filter"), tr("The radio's analogue filter in front of its transmit converter. Automatic follows the sample rate (about 95 percent of it; the HackRF picks its own nearest width). A width is sent to the driver as typed; the driver takes its nearest."));
        if (ImGui::Checkbox(tr("Automatic##txbw"), &autoBw)) { s.filterKhz = autoBw ? 0 : 1750; ch = true; }
        if (!autoBw) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##txbwk", &s.filterKhz, 0, 0, "%.1f kHz")) { if (!(s.filterKhz > 0)) s.filterKhz = 1; ch = true; }
        }
    }
    const bool generic = radio && radio->isGeneric();
    if (generic) {
        rowLabel(tr("Gain stages"), tr("SoapySDR radios (LimeSDR, bladeRF, USRP, XTRX, ...): set a transmit gain stage by its driver name after the overall gain, for example PAD=40, IAMP=0. Each is clamped only to the range the driver reports for it; a name the driver does not list is sent anyway and the log says so. Empty: the overall gain only."));
        if (ImGui::InputTextWithHint("##txstages", "PAD=40, IAMP=0", s.stageGains, sizeof s.stageGains)) ch = true;
        std::map<std::string, std::string> probe;
        if (s.stageGains[0] && !txParseStageGains(s.stageGains, probe)) banner(Note::Warn, tr("Write the stages as name=number, separated by commas. Until then the overall gain alone is used."));
    }
    ImGui::EndDisabled();
    if (ch) saveControl(s);
}

void schedulePanel(TxUi& s, bool running) {
    loadControlPrefs(s);
    bool ch = false;
    ImGui::BeginDisabled(running || s.schedArmed);
    rowLabel(tr("Start at (UTC)"), tr("HH:MM or HH:MM:SS in UTC: Start waits until then (the next such time). Empty: Start sends at once."));
    if (ImGui::InputTextWithHint("##txschedat", tr("now"), s.schedAt, sizeof s.schedAt)) ch = true;
    if (s.schedAt[0] && txParseUtcTime(s.schedAt, nowUtc()) < 0) banner(Note::Warn, tr("Not a time: write HH:MM or HH:MM:SS (UTC). Until then Start sends at once."));
    rowLabel(tr("Repeat every"), tr("After a transmission has ended by itself (one pass, the timeout timer, a finite message) it is sent again this many seconds after the last start. 0: once."));
    if (ImGui::InputDouble("##txschedev", &s.schedEverySec, 0, 0, "%.0f s")) { s.schedEverySec = std::max(0.0, s.schedEverySec); ch = true; }
    if (s.schedEverySec > 0) {
        rowLabel(tr("Times in all"), tr("How many transmissions the schedule makes, the first included. 0: until Stop is pressed."));
        if (ImGui::InputInt("##txschedn", &s.schedCount, 0, 0)) { s.schedCount = std::max(0, s.schedCount); ch = true; }
        const auto tot = s.pv.find("tx.totSec");
        if (s.loop && (tot == s.pv.end() || tot->second <= 0)) ImGui::TextDisabled("%s", tr("A repeat needs an end: turn off Repeat until stopped, or set the timeout timer."));
    }
    ImGui::EndDisabled();
    if (ch) saveControl(s);
}

void scheduleStatus(TxUi& s) {
    if (s.schedArmed && !s.session.running()) {
        char b[160];
        if (s.schedNext >= 0) snprintf(b, sizeof b, tr("Scheduled: next start %s (%d sent so far)"), utcText(s.schedNext).c_str(), s.schedDone);
        else snprintf(b, sizeof b, "%s", tr("Scheduled"));
        banner(Note::Info, b);
        if (ImGui::Button(tr("Cancel the schedule"))) { s.schedArmed = false; s.schedNext = -1; }
    }
}

bool scheduleConfigured(const TxUi& s) { return scheduleOf(s).active() && !s.pttHold; }

void scheduleArm(TxUi& s) {
    s.schedArmed = true;
    s.schedDone = 0;
    s.schedLast = 0;
    s.schedNext = txScheduleNext(scheduleOf(s), nowUtc(), 0, 0);
}

void scheduleTick(App& a, TxUi& s) {
    if (!s.schedArmed && !s.schedRunning) return;
    const bool running = s.session.running();
    if (s.schedRunning && !running) {   // a scheduled transmission has ended
        s.schedRunning = false;
        const TxSession::Stats st = s.session.stats();
        if (!s.schedArmed) return;
        if (st.failed) { s.schedArmed = false; return; }
        s.schedDone++;
        s.schedNext = txScheduleNext(scheduleOf(s), nowUtc(), s.schedLast, s.schedDone);
        if (s.schedNext < 0) { s.schedArmed = false; a.engine.log("transmit: the schedule is complete (" + std::to_string(s.schedDone) + " sent)"); }
        return;
    }
    if (!s.schedArmed || running || s.pendingStart || s.schedNext < 0 || nowUtc() < s.schedNext) return;
    int conflict = 0;
    const TxModeSpec* mode = txModeById(s.modeId);
    const DeviceInfo* radio = bindRadio(a, s, conflict);
    if (!canStart(a, s, radio, conflict, mode)) {
        s.error = tr("The scheduled transmission could not start: the radio is not free. The schedule is cancelled.");
        s.schedArmed = false;
        return;
    }
    s.schedLast = nowUtc();
    if (startTx(a, s, radio, mode, false)) s.schedRunning = true;
    else s.schedArmed = false;   // startTx has put the reason in s.error
}

void calibrationPanel(App& a, TxUi& s, const DeviceInfo* radio, bool running) {
    if (!radio || s.destKind == 1) { ImGui::TextDisabled("%s", tr("The calibration belongs to a radio: choose the transmit radio first.")); return; }
    TxCalibration c = calibrationFor(s.radioKey);
    bool ch = false;
    ImGui::TextDisabled(tr("Saved for %s. Applied to every transmission on this radio."), txRadioLabel(*radio).c_str());
    ImGui::BeginDisabled(running);
    rowLabel(tr("Frequency correction"), tr("The transmitter's clock error in parts per million, positive when it runs fast (the signal then goes out above the frequency asked for). Measure it with a known receiver or a frequency counter."));
    if (ImGui::InputDouble("##calppm", &c.ppm, 0.1, 1, "%.2f ppm")) ch = true;
    {
        const DeviceInfo* rxDev = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? &a.devices[(size_t)a.devIdx] : nullptr;
        const bool same = rxDev && deviceClaimKey(*rxDev) == s.radioKey;
        const auto it = a.tune.radio.find("ppm");
        const double rxPpm = it != a.tune.radio.end() ? atof(it->second.c_str()) : 0;
        ImGui::BeginDisabled(!same || rxPpm == 0);
        char lab[96];
        snprintf(lab, sizeof lab, tr("Use the receiver's %.2f ppm"), rxPpm);
        if (ImGui::SmallButton(lab)) { c.ppm = rxPpm; ch = true; }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("The receiver's correction of this same radio (its crystal is shared by both directions). Only offered when the receiver uses this radio and has a correction."));
    }
    rowLabel(tr("Frequency offset"), tr("A fixed number of hertz added to what the radio is told, for a radio or converter that radiates a set amount off."));
    if (ImGui::InputDouble("##calhz", &c.offsetHz, 1, 100, "%.1f Hz")) ch = true;
    rowLabel(tr("I/Q gain balance"), tr("Evens out the I and Q paths of the radio's mixer. Adjust for the smallest mirror image of a test tone (it shows on the other side of the centre)."));
    { float v = (float)c.iqGainDb; if (ImGui::SliderFloat("##caliqg", &v, -2, 2, "%.3f dB")) { c.iqGainDb = v; ch = true; } }
    rowLabel(tr("I/Q phase balance"), tr("Corrects the phase between I and Q. Adjust with the gain balance for the smallest mirror image."));
    { float v = (float)c.iqPhaseDeg; if (ImGui::SliderFloat("##caliqp", &v, -10, 10, "%.2f deg")) { c.iqPhaseDeg = v; ch = true; } }
    rowLabel(tr("Carrier leak I"), tr("A small DC added to I. With Q, cancels the carrier the radio leaks at its centre: adjust for the smallest spike at the centre frequency."));
    { float v = (float)c.dcI; if (ImGui::SliderFloat("##caldci", &v, -0.1f, 0.1f, "%.4f")) { c.dcI = v; ch = true; } }
    rowLabel(tr("Carrier leak Q"), tr("A small DC added to Q (see Carrier leak I)."));
    { float v = (float)c.dcQ; if (ImGui::SliderFloat("##caldcq", &v, -0.1f, 0.1f, "%.4f")) { c.dcQ = v; ch = true; } }
    ImGui::TextDisabled("%s", tr("Tip: use Carrier tune and a receiver (or the preview of a test) to watch the mirror image and the centre while you adjust."));

    // the power table
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Output power at the gain settings (measured)"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Write down what a power meter (or a calibrated receiver) reads at a few gain settings. OnAir then shows the predicted output power and can set the level for a power you type. It never limits the power."));
    int del = -1;
    for (size_t i = 0; i < c.power.size(); i++) {
        ImGui::PushID((int)i);
        TxPowerPoint& p = c.power[i];
        float g = (float)p.gainDb, d = (float)p.dbm;
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputFloat("##g", &g, 0, 0, "%.1f dB gain")) { p.gainDb = g; ch = true; }
        ImGui::SameLine();
        if (radio->kind == DeviceInfo::HackRF) { if (ImGui::Checkbox(tr("amp"), &p.amp)) ch = true; ImGui::SameLine(); }
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputFloat("##d", &d, 0, 0, "%.1f dBm")) { p.dbm = d; ch = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("remove"))) del = (int)i;
        ImGui::PopID();
    }
    if (del >= 0) { c.power.erase(c.power.begin() + del); ch = true; }
    if (ImGui::SmallButton(tr("Add the current level"))) {
        const double guess = txCalPowerDbm(c, s.gainDb, s.ampOn);   // the table's prediction as a starting value; the meter's reading replaces it
        c.power.push_back({s.gainDb, s.ampOn, std::isfinite(guess) ? guess : 0});
        ch = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Adds a row at the present output level; type what the meter reads next to it."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear the calibration"))) { c = TxCalibration(); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Forgets this radio's correction and power table."));
    ImGui::EndDisabled();
    if (ch) saveCalibration(s.radioKey, c);
}

void powerReadout(TxUi& s, const DeviceInfo* radio) {
    if (!radio || s.destKind == 1) return;
    loadControlPrefs(s);
    const TxCalibration c = calibrationFor(s.radioKey);
    const double p = txCalPowerDbm(c, s.gainDb, s.ampOn);
    if (!std::isfinite(p)) return;
    char b[160];
    const double mw = std::pow(10.0, p / 10.0);
    if (mw >= 1000) snprintf(b, sizeof b, tr("about %.1f dBm (%.2f W) out, from this radio's power table"), p, mw / 1000);
    else snprintf(b, sizeof b, tr("about %.1f dBm (%.3g mW) out, from this radio's power table"), p, mw);
    ImGui::TextDisabled("%s", b);
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputDouble("##txtarget", &s.targetDbm, 0, 0, "%.1f dBm")) plat::basePrefs().setD("tx.targetDbm", s.targetDbm);
    ImGui::SameLine();
    if (ImGui::Button(tr("Set the level for this power"))) {
        const double g = txCalGainForDbm(c, s.targetDbm, s.ampOn);
        if (std::isfinite(g)) {
            s.gainDb = (float)g;
            s.gainSet = true;
            const bool hasRange = radio->txGainMaxDb > radio->txGainMinDb;
            if (hasRange && (g < radio->txGainMinDb || g > radio->txGainMaxDb)) {
                char w[200];
                snprintf(w, sizeof w, tr("%.1f dB is outside this radio's gain scale (%.1f to %.1f dB); the radio will hold it at its own limit."), g, radio->txGainMinDb, radio->txGainMaxDb);
                s.info = w;
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Works out the gain from the measured table. It is a prediction: check it with a meter."));
}

} // namespace txui
