// The "Radio settings" section under the gain controls: the radio's own settings its driver lists (DeviceInfo::settings: frequency
// correction, direct sampling, notch filters, gain modes, ...), only those the selected radio has. The values live in a.tune.radio, are
// saved per radio (app_state.cpp: syncRadioSettings / saveRadioSettings) and reach a running radio at once (a retune); one that takes
// effect when the radio is opened restarts a running receiver. A value set back to its default is dropped, so the driver does what it always did.
#include "../app.h"
#include "dect2/source/converter.h"
#include "dect2/selfcal/selfcal.h"
#include "dect2/selfcal/ppmcal.h"
#include <cmath>
#include <map>

namespace {

std::string numText(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// one setting; true when its value changed (the new value is in v, "" = back to the default)
bool settingRow(const RadioSetting& r, std::string& v, bool vertical) {
    const std::string cur = v.empty() ? r.def : v;
    bool changed = false;
    ImGui::PushID(r.key.c_str());
    const float w = (vertical ? 150 : 170) * gUi;
    switch (r.type) {
    case RadioSetting::Bool: {
        bool on = cur == "1";
        if (ImGui::Checkbox(r.label.c_str(), &on)) { v = on ? "1" : "0"; changed = true; }
        break;
    }
    case RadioSetting::Choice: {
        size_t at = 0;
        for (size_t i = 0; i < r.values.size(); i++) if (r.values[i] == cur) at = i;
        ImGui::TextDisabled("%s", r.label.c_str());
        if (ImGui::IsItemHovered() && !r.help.empty()) ImGui::SetTooltip("%s", r.help.c_str());
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##v", at < r.names.size() ? r.names[at].c_str() : cur.c_str())) {
            for (size_t i = 0; i < r.values.size(); i++)
                if (ImGui::Selectable(i < r.names.size() ? r.names[i].c_str() : r.values[i].c_str(), i == at) && i != at) { v = r.values[i]; changed = true; }
            ImGui::EndCombo();
        }
        break;
    }
    case RadioSetting::Number: {
        // typed values are taken when the field is left (not at every key); the step buttons take effect at once
        static std::map<std::string, double> editing;
        auto it = editing.find(r.key);
        double val = it != editing.end() ? it->second : atof(cur.c_str());
        ImGui::TextDisabled("%s", r.label.c_str());
        if (ImGui::IsItemHovered() && !r.help.empty()) ImGui::SetTooltip("%s", r.help.c_str());
        ImGui::SetNextItemWidth(w);
        char fmt[32];
        const int dec = r.step >= 1 ? 0 : r.step >= 0.1 ? 1 : r.step >= 0.01 ? 2 : 3;
        snprintf(fmt, sizeof fmt, "%%.%df%s%s", dec, r.unit.empty() ? "" : " ", r.unit.c_str());
        if (ImGui::InputDouble("##v", &val, r.step > 0 ? r.step : 0.0, r.step > 0 ? r.step * 10 : 0.0, fmt)) editing[r.key] = val;
        it = editing.find(r.key);
        if (it != editing.end() && !ImGui::IsItemActive()) {
            double x = it->second;
            if (r.maxV > r.minV) x = std::min(r.maxV, std::max(r.minV, x));
            if (r.step >= 1) x = std::round(x);
            editing.erase(it);
            const std::string nv = numText(x);
            if (nv != numText(atof(cur.c_str()))) { v = nv; changed = true; }
        }
        break;
    }
    }
    if (r.type == RadioSetting::Bool && ImGui::IsItemHovered() && !r.help.empty()) ImGui::SetTooltip("%s", r.help.c_str());
    ImGui::PopID();
    if (changed && (v == r.def || (r.type == RadioSetting::Number && numText(atof(v.c_str())) == numText(atof(r.def.c_str()))))) v.clear();
    return changed;
}

// The converter in front of the radio (converter.h): up-converter or LNB. The app then works with the frequency at the antenna; the radio
// is tuned to the IF. Stored in a.tune.radio under the conv.* keys, so it is saved per radio with the other settings. True when it changed.
bool converterUi(App& a, bool vertical) {
    (void)vertical;
    using namespace dect2;
    bool changed = false;
    auto& R = a.tune.radio;
    auto get = [&](const char* k, const char* def) { auto it = R.find(k); return it != R.end() ? it->second : std::string(def); };
    auto put = [&](const char* k, const std::string& v, const char* def) { if (v.empty() || v == def) R.erase(k); else R[k] = v; changed = true; };
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Converter (up-converter / LNB)"));
    const std::string id = get(convKeyPreset(), "none");
    const ConverterPreset* cur = findConverterPreset(id);
    if (!cur) cur = &converterPresets()[0];
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("##convpreset", tr(cur->name))) {
        for (const auto& p : converterPresets()) {
            if (ImGui::Selectable(tr(p.name), &p == cur)) {
                put(convKeyPreset(), p.id, "none");
                R.erase(convKeyLo()); R.erase(convKeyMode()); R.erase(convKeyPpm());
                if (p.custom) { R[convKeyLo()] = "125"; R[convKeyMode()] = "add"; }
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show and tune the frequency at the antenna while the radio is tuned to what the converter outputs."));
    if (std::string(cur->id) == "hamitdown") {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Ham It Down: 1500 MHz down-converter for about 1.5 to 3 GHz (2.4 GHz, 2.3/3.4 GHz). Connect it between the antenna and the radio; the radio sees RF - 1500 MHz. Below 1500 MHz is not supported."));
        ImGui::PopTextWrapPos();
    }
    if (cur->custom) {
        double lo = atof(get(convKeyLo(), "125").c_str());
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::InputDouble(tr("LO (MHz)"), &lo, 0, 0, "%.6f") && lo >= 0) put(convKeyLo(), numText(lo), "");
        const std::string m = get(convKeyMode(), "add");
        int mi = m == "sub" ? 1 : m == "high" ? 2 : 0;
        const char* names[3] = {tr("Up: radio = RF + LO"), tr("Down, LO below: radio = RF - LO"), tr("Down, LO above: radio = LO - RF (mirrored)")};
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::Combo("##convmode", &mi, names, 3)) put(convKeyMode(), mi == 1 ? "sub" : mi == 2 ? "high" : "add", "");
    }
    if (cur->hasPpm) {
        double ppm = atof(get(convKeyPpm(), "0").c_str());
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::InputDouble(tr("LO correction (ppm)"), &ppm, 0, 0, "%.3f")) put(convKeyPpm(), numText(ppm), "0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How far the converter's oscillator runs fast (positive) or slow. The LO used is LO x (1 + ppm / 1e6)."));
    }
    if (std::string(cur->id) != "none") {
        bool inv = get(convKeyInvert(), "0") == "1";
        if (ImGui::Checkbox(tr("Mirror the spectrum (swap I and Q)"), &inv)) put(convKeyInvert(), inv ? "1" : "", "");
        const Converter c = converterFromRadio(R);
        const double rf = a.freqMhz * 1e6;
        char b[256];
        if (c.active) {
            snprintf(b, sizeof b, tr("antenna %.6g MHz -> %s LO %.6g -> radio %.6g MHz%s"), rf / 1e6, c.mode == Converter::Add ? tr("up-converter") : tr("LNB"),
                     c.effLoHz() / 1e6, c.rfToIf(rf) / 1e6, c.inverts() ? tr("  (mirrored: I/Q swapped)") : "");
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", b);
            if (!c.reachable(rf)) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", tr("This frequency is not reachable through the converter."));
            ImGui::PopTextWrapPos();
        } else ImGui::TextDisabled("%s", tr("Set the LO above 0 MHz."));
    }
    return changed;
}

// ---- Calibrate frequency: the frequency self-calibration dialog (selfcal.h, ppmcal.h, docs/self-calibration.md)
struct CalState {
    std::unique_ptr<selfcal::LiveCal> live;
    selfcal::Wizard wiz{0.05, 180};   // 1-sigma 0.05 ppm = a 95 % interval under 0.1 ppm
    selfcal::Store store;
    selfcal::PpmAverager avg;         // the finished measurements of this session (several stations / repeats)
    selfcal::DriftTrack drift;        // the optional 10 minute track
    bool loaded = false, open = false, running = false, quick = false, tracking = false;
    int method = 0;                   // 0 FM station, 1 ATSC pilot, 2 time signal / AM carrier, 3 manual
    int fmIdx = 0, atscIdx = 0, timeIdx = 0;
    bool useTuned = true;             // FM: the station the receiver is tuned to
    double carrierMhz = 0;            // 2: a typed AM carrier (MHz), 0 = use the list
    double manExpMhz = 0, manObsMhz = 0;
    double t0 = 0;
    double basePpm = 0;               // the radio's ppm setting while measuring (the result is the error left on top of it)
    selfcal::Estimate result;
    selfcal::QuickResult quickRes;
    bool haveQuick = false, subCarrier = false;
    std::string serial;
};
CalState& cal() { static CalState c; return c; }

void calStop(App& a) {
    CalState& c = cal();
    if (c.running) a.engine.setIqTap(nullptr);
    c.running = false;
}

// the chosen reference: nominal frequency (Hz) and whether it is a sub-carrier
bool calReference(const App& a, double& hz, bool& sub) {
    CalState& c = cal();
    static const auto fm = selfcal::fmChannels();
    static const auto at = selfcal::atscChannels();
    static const auto tm = selfcal::timeCarriers();
    if (c.method == 0) {
        sub = true;
        hz = c.useTuned ? a.freqMhz * 1e6 : fm[std::max(0, std::min((int)fm.size() - 1, c.fmIdx))].nominalHz;
    } else if (c.method == 1) {
        sub = false; hz = at[std::max(0, std::min((int)at.size() - 1, c.atscIdx))].nominalHz;
    } else {
        sub = false;
        hz = c.carrierMhz > 0 ? c.carrierMhz * 1e6 : tm[std::max(0, std::min((int)tm.size() - 1, c.timeIdx))].nominalHz;
    }
    return hz > 0;
}

void calStart(App& a, const std::string& serial, bool quick, bool tracking) {
    CalState& c = cal();
    auto it = a.tune.radio.find("ppm");
    c.basePpm = it != a.tune.radio.end() ? atof(it->second.c_str()) : 0;
    c.serial = serial;
    c.result = selfcal::Estimate();
    c.quick = quick; c.tracking = tracking; c.haveQuick = false;
    double hz = 0; bool sub = false;
    if (!calReference(a, hz, sub)) return;
    c.subCarrier = sub;
    if (std::fabs(hz - a.freqMhz * 1e6) > 1.0 && !(c.method == 0 && c.useTuned)) {   // tune to the chosen reference
        a.freqMhz = hz / 1e6;
        if (a.engine.running()) { a.tune.centerHz = hz; a.engine.retune(a.tune); }
    }
    c.live.reset(new selfcal::LiveCal(c.method == 0 ? selfcal::LiveCal::FmStation : selfcal::LiveCal::Carrier, hz));
    c.wiz = (quick || tracking) ? selfcal::Wizard(0.3, 60) : selfcal::Wizard(0.05, 180);
    c.wiz.start();
    c.t0 = ImGui::GetTime();
    selfcal::LiveCal* lc = c.live.get();
    App* ap = &a;
    a.engine.setIqTap([lc, ap](const std::complex<float>* x, size_t n, double rate) {
        lc->push(x, n, rate, ap->freqMhz * 1e6 - ap->engine.offsetHz());
    });
    c.running = true;
}

void calHistoryPlot(const std::vector<selfcal::Entry>& h) {
    const float w = std::max(200.f, ImGui::GetContentRegionAvail().x), hgt = 90 * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, hgt));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + hgt), IM_COL32(20, 24, 30, 255));
    if (h.size() < 2) return;
    double lo = 1e9, hi = -1e9;
    for (const auto& e : h) { lo = std::min(lo, e.ppm - e.sigmaPpm); hi = std::max(hi, e.ppm + e.sigmaPpm); }
    if (hi - lo < 0.5) { const double m = 0.5 * (hi + lo); lo = m - 0.25; hi = m + 0.25; }
    const double t0 = h.front().utc, t1 = std::max(h.back().utc, t0 + 1);
    std::vector<ImVec2> pts;
    for (const auto& e : h) {
        const float x = p0.x + 6 + (float)((e.utc - t0) / (t1 - t0)) * (w - 12);
        const float y = p0.y + hgt - 6 - (float)((e.ppm - lo) / (hi - lo)) * (hgt - 12);
        pts.push_back(ImVec2(x, y));
        dl->AddLine(ImVec2(x, y - (float)(e.sigmaPpm / (hi - lo)) * (hgt - 12)), ImVec2(x, y + (float)(e.sigmaPpm / (hi - lo)) * (hgt - 12)), IM_COL32(120, 160, 200, 200));
    }
    dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(110, 220, 140, 255), 0, 1.5f);
    for (const auto& q : pts) dl->AddCircleFilled(q, 2.5f, IM_COL32(110, 220, 140, 255));
    char b[48];
    snprintf(b, sizeof b, "%+.2f", hi); dl->AddText(ImVec2(p0.x + 8, p0.y + 2), IM_COL32(170, 170, 170, 255), b);
    snprintf(b, sizeof b, "%+.2f", lo); dl->AddText(ImVec2(p0.x + 8, p0.y + hgt - 16), IM_COL32(170, 170, 170, 255), b);
}

// a finished full measurement: into the history of this radio (kept in the preferences, per radio key)
void calRecord(App& a, const std::string& serial, const selfcal::Estimate& e) {
    CalState& c = cal();
    selfcal::Entry en;
    en.serial = serial; en.utc = (double)utcNowOf(a); en.ppm = c.basePpm + e.ppm; en.sigmaPpm = e.sigmaPpm; en.source = e.source;
    c.store.add(en);
    plat::prefs().setS("selfcal.hist", c.store.toText());
    plat::prefs().flush();
}

// the dialog; true when it changed the radio's ppm setting
bool calibrateDialog(App& a, const DeviceInfo& d) {
    CalState& c = cal();
    bool changed = false;
    if (!c.loaded) { c.store.fromText(plat::prefs().getS("selfcal.hist", "")); c.loaded = true; c.manExpMhz = a.freqMhz; c.manObsMhz = a.freqMhz; }
    const std::string serial = radioKeyOf(d);   // the radio's own key (its serial when it has one): history and the stored ppm are per radio
    if (c.open) { ImGui::OpenPopup(trw("Frequency calibration")); c.open = false; }
    ImGui::SetNextWindowSize(ImVec2(470 * gUi, 0), ImGuiCond_Appearing);
    bool keep = true;
    if (!ImGui::BeginPopupModal(trw("Frequency calibration"), &keep, ImGuiWindowFlags_AlwaysAutoResize)) return false;
    ImGui::PushTextWrapPos(450 * gUi);
    ImGui::TextDisabled("%s", tr("Measures how far this radio's clock is off against a signal of known frequency and sets the ppm correction for this radio. Let the radio warm up for about 10 minutes first."));
    ImGui::PopTextWrapPos();
    const bool conv = converterFromRadio(a.tune.radio).active;
    bool hasPpm = false;
    for (const auto& r : d.settings) if (r.key == "ppm") hasPpm = true;
    if (conv) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", tr("Not with a converter in front of the radio."));
    else if (!a.engine.running() && !c.running && c.method != 3) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", tr("Start the receiver first."));
    if (!hasPpm) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", tr("This radio has no frequency correction setting: the result cannot be applied."));
    ImGui::BeginDisabled(c.running);
    static const char* kMethods[] = {"FM station (stereo pilot)", "ATSC pilot (US TV channel)", "Time signal or AM carrier", "Manual: type what you see"};
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo(tr("Method"), tr(kMethods[c.method]))) {
        for (int i = 0; i < 4; i++)
            if (ImGui::Selectable(tr(kMethods[i]), c.method == i)) { c.method = i; c.wiz = selfcal::Wizard(0.05, 180); c.result = selfcal::Estimate(); c.haveQuick = false; }
        ImGui::EndCombo();
    }
    {
        static const auto fm = selfcal::fmChannels();
        static const auto at = selfcal::atscChannels();
        static const auto tm = selfcal::timeCarriers();
        if (c.method == 0) {
            ImGui::Checkbox(tr("The station the receiver is tuned to"), &c.useTuned);
            if (!c.useTuned) {
                ImGui::SetNextItemWidth(260 * gUi);
                if (ImGui::BeginCombo("##calfm", fm[c.fmIdx].label.c_str())) {
                    for (int i = 0; i < (int)fm.size(); i++) if (ImGui::Selectable(fm[i].label.c_str(), c.fmIdx == i)) c.fmIdx = i;
                    ImGui::EndCombo();
                }
            }
        } else if (c.method == 1) {
            ImGui::SetNextItemWidth(260 * gUi);
            if (ImGui::BeginCombo("##calat", at[c.atscIdx].label.c_str())) {
                for (int i = 0; i < (int)at.size(); i++) if (ImGui::Selectable(at[i].label.c_str(), c.atscIdx == i)) c.atscIdx = i;
                ImGui::EndCombo();
            }
        } else if (c.method == 2) {
            ImGui::SetNextItemWidth(260 * gUi);
            if (ImGui::BeginCombo("##caltm", c.carrierMhz > 0 ? tr("Typed carrier") : tm[c.timeIdx].label.c_str())) {
                for (int i = 0; i < (int)tm.size(); i++) if (ImGui::Selectable(tm[i].label.c_str(), c.carrierMhz <= 0 && c.timeIdx == i)) { c.timeIdx = i; c.carrierMhz = 0; }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(120 * gUi);
            ImGui::InputDouble(tr("or an AM carrier of known frequency (MHz, 0 = list)"), &c.carrierMhz, 0, 0, "%.6f");
        }
    }
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(450 * gUi);
    if (c.method == 0) ImGui::TextDisabled("%s", tr("A station's pilot is not always exact: repeat on two or three stations. Strong, steady stations are best."));
    else if (c.method == 1) ImGui::TextDisabled("%s", tr("The ATSC pilot sits 309.44 kHz above the lower channel edge; the radio is tuned to it. A main carrier, accurate to a fraction of a ppm."));
    else if (c.method == 2) ImGui::TextDisabled("%s", tr("Time stations and long/medium-wave broadcast carriers are exact. The radio must reach the frequency (direct sampling or an upconverter below 24 MHz)."));
    ImGui::PopTextWrapPos();

    char b[240];
    if (c.method == 3) {
        ImGui::SetNextItemWidth(130 * gUi); ImGui::InputDouble(tr("Expected frequency (MHz)"), &c.manExpMhz, 0, 0, "%.6f");
        ImGui::SetNextItemWidth(130 * gUi); ImGui::InputDouble(tr("Observed frequency (MHz)"), &c.manObsMhz, 0, 0, "%.6f");
        if (ImGui::Button(tr("Compute"))) {
            selfcal::Estimate e;
            e.valid = true; e.ppm = selfcal::ppmFromObserved(c.manExpMhz * 1e6, c.manObsMhz * 1e6); e.sigmaPpm = 0.1; e.source = "manual";
            auto it = a.tune.radio.find("ppm");
            c.basePpm = it != a.tune.radio.end() ? atof(it->second.c_str()) : 0;
            c.subCarrier = false; c.serial = serial; c.result = e; c.haveQuick = false; c.quick = false; c.tracking = false;
            c.wiz = selfcal::Wizard(1.0, 1.0);
            c.wiz.start();
            c.wiz.update(e, 1.0);   // a typed pair needs no integration: the wizard goes straight to Done
            calRecord(a, serial, e);
            c.avg.add(c.basePpm + e.ppm, 0.3);
        }
        ImGui::SameLine();
        ImGui::PushTextWrapPos(450 * gUi);
        ImGui::TextDisabled("%s", tr("Type the frequency a signal shows with the present setting, and the frequency it should have."));
        ImGui::PopTextWrapPos();
    }

    if (c.running) {
        const double sec = ImGui::GetTime() - c.t0;
        const auto e = c.live->estimate();
        const auto st = c.wiz.update(e, sec);
        if (!a.engine.running() || conv) { calStop(a); c.wiz.start(); c.tracking = false; }
        else if (st == selfcal::Wizard::Done || st == selfcal::Wizard::Failed) {
            calStop(a);
            if (st == selfcal::Wizard::Done) {
                c.result = c.wiz.result();
                if (c.quick) { c.quickRes = selfcal::quickCheck(c.result.ppm, c.result.sigmaPpm); c.haveQuick = true; }
                else if (c.tracking) {
                    const double now = ImGui::GetTime();
                    c.drift.add(now, c.basePpm + c.result.ppm, c.result.sigmaPpm);
                    if (!c.drift.finished(now) && a.engine.running()) calStart(a, serial, false, true);   // the next point
                    else c.tracking = false;
                } else {
                    calRecord(a, serial, c.result);
                    c.avg.add(c.basePpm + c.result.ppm, std::max(0.02, c.result.sigmaPpm));
                }
            } else c.tracking = false;
        }
        ImGui::ProgressBar((float)c.wiz.progress(), ImVec2(-1, 0));
    }
    switch (c.wiz.stage()) {
    case selfcal::Wizard::Idle: ImGui::TextUnformatted(tr("Ready.")); break;
    case selfcal::Wizard::Searching: ImGui::TextUnformatted(tr("Looking for a reference signal.")); break;
    case selfcal::Wizard::Integrating:
        snprintf(b, sizeof b, tr("Measuring: %+.2f ppm, uncertainty %.2f ppm."), c.wiz.result().ppm, c.wiz.result().sigmaPpm); ImGui::TextUnformatted(b); break;
    case selfcal::Wizard::Done:
        snprintf(b, sizeof b, tr("Done: %+.2f ppm (95%% interval +-%.2f ppm)."), c.wiz.result().ppm, 1.96 * c.wiz.result().sigmaPpm); ImGui::TextUnformatted(b); break;
    case selfcal::Wizard::Failed: ImGui::TextUnformatted(tr("No usable reference signal was found.")); break;
    }
    if (c.haveQuick) ImGui::TextColored(c.quickRes.verdict == selfcal::QuickVerdict::Good ? ImVec4(0.5f, 1, 0.5f, 1) : ImVec4(1, 0.7f, 0.3f, 1), "%s", c.quickRes.text.c_str());
    if (c.method != 3) {
        ImGui::BeginDisabled(conv || (!a.engine.running() && !c.running));
        if (!c.running) {
            if (ImGui::Button(tr("Start"))) { c.drift.reset(); calStart(a, serial, false, false); }
            ImGui::SameLine();
            if (ImGui::Button(tr("Quick check"))) { c.drift.reset(); calStart(a, serial, true, false); }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("A short measurement with the present correction: does it still hold? Nothing is stored."));
            ImGui::SameLine();
            if (ImGui::Button(tr("Track drift (10 min)"))) { c.drift.reset(); calStart(a, serial, false, true); }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Measure every minute or so for 10 minutes and draw the drift: shows when the radio has warmed up."));
        } else if (ImGui::Button(tr("Stop"))) { calStop(a); c.wiz.start(); c.tracking = false; }
        ImGui::EndDisabled();
    }
    if (c.drift.size() > 0) {
        std::vector<selfcal::Entry> dv;
        for (size_t i = 0; i < c.drift.size(); i++) { selfcal::Entry e; e.utc = c.drift.time(i); e.ppm = c.drift.ppm(i); e.sigmaPpm = c.drift.sigma(i); dv.push_back(e); }
        ImGui::TextUnformatted(tr("Drift track (ppm over time)"));
        calHistoryPlot(dv);
        ImGui::TextDisabled("%s", c.drift.note().c_str());
    }
    if (!c.running && !c.tracking && !c.quick && c.wiz.stage() == selfcal::Wizard::Done && c.result.valid) {
        const double np = c.basePpm + c.result.ppm;
        const auto san = selfcal::checkPpm(np, selfcal::Tcxo::Unknown, c.subCarrier, 1.96 * c.result.sigmaPpm);
        for (const auto& n : san.notes) { ImGui::PushTextWrapPos(450 * gUi); ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", n.c_str()); ImGui::PopTextWrapPos(); }
        if (c.avg.count() > 1) {
            snprintf(b, sizeof b, tr("%d measurements this session: %+.2f ppm, 95%% interval +-%.2f ppm."), c.avg.count(), c.avg.mean(), c.avg.ci95());
            ImGui::TextDisabled("%s", b);
        }
        ImGui::BeginDisabled(!san.accepted || !hasPpm);
        snprintf(b, sizeof b, tr("Apply %+.2f ppm to this radio"), np);
        if (ImGui::Button(b)) {
            selfcal::applyPpm(a.tune.radio, np);
            saveRadioSettings(a);   // stored with this radio's settings, which come back with the radio
            changed = true;
            c.wiz = selfcal::Wizard(0.05, 180);
            c.result = selfcal::Estimate();
        }
        ImGui::EndDisabled();
    }
    {
        auto it = a.tune.radio.find("ppm");
        snprintf(b, sizeof b, tr("Current setting: %.2f ppm"), it != a.tune.radio.end() ? atof(it->second.c_str()) : 0.0);
        ImGui::TextDisabled("%s", b);
    }
    const auto h = c.store.history(serial);
    ImGui::Separator();
    if (h.empty()) ImGui::TextDisabled("%s", tr("No measurements of this radio yet."));
    else {
        ImGui::TextUnformatted(tr("History of this radio (ppm)"));
        calHistoryPlot(h);
        double dr = 0;
        if (c.store.drift(serial, 3600, dr)) { snprintf(b, sizeof b, tr("Drift: %+.3f ppm per minute"), dr); ImGui::TextDisabled("%s", b); }
    }
    if (ImGui::Button(tr("Close"))) keep = false;
    if (!keep) { calStop(a); c.tracking = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
    return changed;
}

}   // namespace

// Tools > Calibrate frequency: opens the dialog; calibrateFrequencyDraw() (once per frame, draw_ui.cpp) draws it for the selected radio
void calibrateFrequencyOpen(App&) { cal().open = true; }
void calibrateFrequencyDraw(App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) { cal().open = false; return; }
    const DeviceInfo& d = a.devices[a.devIdx];
    if (!d.isRadio()) { cal().open = false; return; }
    syncRadioSettings(a);
    if (calibrateDialog(a, d) && a.engine.running()) { a.tune.centerHz = settingsRetuneCentreHz(a); a.engine.retune(a.tune); }
}

void radioSettingsUi(App& a, bool vertical) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return;
    const DeviceInfo& d = a.devices[a.devIdx];
    if (!d.isRadio()) return;
    syncRadioSettings(a);
    int changedN = 0;
    for (const auto& r : d.settings) if (a.tune.radio.count(r.key)) changedN++;
    if (a.tune.radio.count(dect2::convKeyPreset())) changedN++;
    if (a.tune.radio.count("decim")) changedN++;
    char title[64];
    snprintf(title, sizeof title, changedN ? tr("Radio settings (%d changed)###radioset") : tr("Radio settings###radioset"), changedN);
    bool open = false;
    if (vertical) {
        ImGui::SetNextItemOpen(false, ImGuiCond_FirstUseEver);
        open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_SpanAvailWidth);
    } else {   // a one-line bar: a button with the settings in a popup (the caller places it)
        if (ImGui::SmallButton(tr(changedN ? "Radio settings*" : "Radio settings"))) ImGui::OpenPopup("##radiosetpop");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("%s: frequency correction and the other settings of this radio"), title);
        open = ImGui::BeginPopup("##radiosetpop");
    }
    if (!open) return;
    bool restart = false, retune = false;
    for (const auto& r : d.settings) {
        auto it = a.tune.radio.find(r.key);
        std::string v = it != a.tune.radio.end() ? it->second : std::string();
        if (!settingRow(r, v, vertical)) continue;
        if (v.empty()) a.tune.radio.erase(r.key); else a.tune.radio[r.key] = v;
        saveRadioSettings(a);
        if (r.restart) restart = true; else retune = true;
    }
    if (converterUi(a, vertical)) { saveRadioSettings(a); retune = true; }
    if (d.kind == DeviceInfo::HackRF || d.kind == DeviceInfo::Soapy || d.kind == DeviceInfo::Native) {   // #233: decimation in OnAir, for radios that are poor at low sample rates
        static const int kDec[] = {1, 2, 4, 8, 16};
        const int cur = dect2::radioDecimation(a.tune);
        char lb[48];
        if (cur > 1) snprintf(lb, sizeof lb, "%d x", cur); else snprintf(lb, sizeof lb, "%s", tr("Off"));
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::BeginCombo(tr("Decimate in OnAir"), lb)) {
            for (int v : kDec) {
                char o[48];
                if (v > 1) snprintf(o, sizeof o, "%d x", v); else snprintf(o, sizeof o, "%s", tr("Off"));
                if (ImGui::Selectable(o, v == cur)) {
                    if (v > 1) a.tune.radio["decim"] = std::to_string(v); else a.tune.radio.erase("decim");
                    saveRadioSettings(a);
                    restart = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("The radio runs this many times faster than the mode asks, and OnAir filters the samples down to the mode's rate itself.\n"
                                       "For radios that are poor at low sample rates (images, a weak anti-alias filter). Costs CPU and USB bandwidth;\n"
                                       "the radio's highest rate still applies. Off: the radio runs at the mode's rate."));
    }
    {
        bool hasPpm = false;
        for (const auto& r : d.settings) if (r.key == "ppm") hasPpm = true;
        if (hasPpm) {
            if (ImGui::SmallButton(tr("Calibrate..."))) cal().open = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Measure this radio's frequency error against an FM station or a carrier of known frequency"));
        }
    }
    {   // the level offset of this radio, for the spectrum in dBm (plots.cpp): only the picture changes, the radio is not touched
        float off = dbmOffset(a);
        ImGui::SetNextItemWidth((vertical ? 150 : 170) * gUi);
        if (ImGui::InputFloat(tr("dBm offset (dB)"), &off, 0.5f, 5.f, "%+.1f")) setDbmOffset(a, std::max(-200.f, std::min(200.f, off)));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Added to a level in dBFS to give dBm for this radio: feed a known signal into the antenna port and set the difference. Used when the spectrum shows dBm (Spectrum button on the plot)."));
    }
    if (d.kind == DeviceInfo::Soapy && d.settings.size() <= 1) ImGui::TextDisabled("%s", tr("The driver's own settings show up here\nonce this radio has been started."));
    if (vertical) ImGui::TreePop(); else ImGui::EndPopup();
    if (!a.engine.running()) return;   // the next start takes them
    if (restart) { a.engine.log("radio setting changed: restarting the radio"); startReceiver(a); }
    else if (retune) { a.tune.centerHz = settingsRetuneCentreHz(a); a.engine.retune(a.tune); }
}
