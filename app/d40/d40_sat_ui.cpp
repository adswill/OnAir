// DVB-S / S2: blind scan of the capture into a transponder list, modcods that fit the measured Es/N0, dish pointing readout. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/d40/sat_d40.h"
#include <cmath>
#include <cstdio>

namespace d4 = dect2::d40;

namespace {
struct Sat {
    bool scanning = false;
    int frames = 0;
    std::vector<double> acc;          // linear power sum per bin
    std::vector<d4::Transponder> list;
    double startHz = 0, binHz = 0;
    float minBwMhz = 1.0f;
    bool init = false;
    d4::PointingAid aid;
    bool pointing = false;
    double lastCn = 0;
    bool lnb = false; float loMhz = 9750.f; d4::LnbDrift drift; double lastDriftT = 0;
} S;
}  // namespace

void d40SatTick(App& a) {
    if (!S.init) { S.init = true; S.minBwMhz = (float)plat::prefs().getD("d40SatMinBw", 1.0); S.loMhz = (float)plat::prefs().getD("d40SatLo", 9750.0); S.lnb = plat::prefs().getI("d40SatLnb", 0) != 0; }
    if (S.lnb && a.engine.running() && a.rx.standard == 7 && a.rx.dvbs.state >= 1) {   // the carrier's offset from where the radio is tuned: the LNB's error while the user holds the tuning on the carrier
        const double now = ImGui::GetTime();
        if (now - S.lastDriftT >= 1.0) { S.lastDriftT = now; S.drift.add(now, 0.0, a.rx.dvbs.cfoHz, S.loMhz * 1e6); }
    }
    if (S.scanning && a.engine.running() && !a.spec.dbfs.empty()) {
        if (S.acc.size() != a.spec.dbfs.size()) { S.acc.assign(a.spec.dbfs.size(), 0.0); S.frames = 0; }
        for (size_t i = 0; i < a.spec.dbfs.size(); i++) S.acc[i] += std::pow(10.0, a.spec.dbfs[i] / 10.0);
        if (++S.frames >= 40) {
            std::vector<float> db(S.acc.size());
            for (size_t i = 0; i < db.size(); i++) db[i] = (float)(10.0 * std::log10(S.acc[i] / S.frames + 1e-20));
            const double fs = a.engine.sampleRate();
            S.binHz = fs / (double)db.size();
            S.startHz = a.engine.captureCentreHz() - fs / 2;
            S.list = d4::blindScan(db, S.binHz, S.startHz, S.minBwMhz * 1e6);
            S.scanning = false; S.acc.clear();
            a.engine.log("DVB-S blind scan: " + std::to_string(S.list.size()) + " carriers");
        }
    }
    if (S.pointing && a.rx.standard == 7) S.lastCn = a.rx.dvbs.snrDb;
}

void d40SatCard(App& a) {
    if (!typo::collapsingHeader(trw("Blind scan and dish pointing"), 0)) return;
    const bool run = a.engine.running();
    if (!run) ImGui::BeginDisabled();
    if (ImGui::Button(S.scanning ? tr("Scanning...") : tr("Scan the capture for carriers")) && !S.scanning) { S.scanning = true; S.acc.clear(); S.frames = 0; }
    if (!run) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Averages 40 spectrum frames of the whole capture and lists every carrier: centre, width, symbol rate (snapped to a standard rate) and C/N. Widen the capture to cover the LNB band."));
    if (!S.list.empty()) {
        if (ImGui::BeginTable("##d40bs", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, 170 * gUi))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn(tr("Width")); ImGui::TableSetupColumn(tr("Symbol rate")); ImGui::TableSetupColumn(tr("C/N")); ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            int id = 0;
            for (const auto& t : S.list) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%.2f", t.centreHz / 1e6);
                ImGui::TableNextColumn(); ImGui::Text("%.1f MHz", t.bwHz / 1e6);
                ImGui::TableNextColumn(); ImGui::Text("%.3f Msym/s%s", t.symbolRateHz / 1e6, t.snapped ? "" : " ~");
                ImGui::TableNextColumn(); ImGui::Text("%.1f dB", t.cnDb);
                ImGui::TableNextColumn(); ImGui::PushID(id++); if (ImGui::SmallButton(tr("Tune"))) spectrumTune(a, t.centreHz / 1e6); ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else if (!S.scanning) ImGui::TextDisabled("%s", tr("No scan yet."));
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputFloat(tr("Narrowest carrier (MHz)"), &S.minBwMhz, 0.5f, 1.f, "%.1f")) { if (S.minBwMhz < 0.1f) S.minBwMhz = 0.1f; plat::prefs().setD("d40SatMinBw", S.minBwMhz); }
    // modcods that fit the Es/N0 of the locked transponder
    if (a.rx.standard == 7 && a.rx.dvbs.state > 0 && a.rx.dvbs.snrDb > 0) {
        const auto fit = d4::modcodFor(a.rx.dvbs.snrDb);
        std::string t;
        for (size_t i = 0; i < fit.size() && i < 3; i++) { char b[64]; snprintf(b, sizeof b, "%s%s (+%.1f dB)", i ? ", " : "", fit[i].name.c_str(), fit[i].marginDb); t += b; }
        ImGui::TextDisabled("Es/N0 %.1f dB would carry: %s", a.rx.dvbs.snrDb, t.empty() ? "-" : t.c_str());
    }
    // dish pointing
    if (ImGui::Checkbox(tr("Dish pointing aid"), &S.pointing) && !S.pointing) S.aid.reset();
    if (S.pointing) {
        const double rate = S.aid.feed(S.lastCn);
        ImGui::SameLine(); if (ImGui::SmallButton(tr("Reset peak"))) S.aid.reset();
        const int tr_ = S.aid.trend();
        ImGui::Text("C/N %.1f dB, peak %.1f dB, %.1f dB below the peak, %s%s", S.lastCn, S.aid.peakDb(), S.aid.belowPeakDb(), tr_ > 0 ? "rising" : (tr_ < 0 ? "falling" : "steady"), S.aid.atPeak() ? " - at the peak" : "");
        (void)rate;
    }
    // LNB warm-up drift
    if (ImGui::Checkbox(tr("LNB drift watch"), &S.lnb)) { plat::prefs().setI("d40SatLnb", S.lnb ? 1 : 0); S.drift = d4::LnbDrift(); }
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Tune onto a steady beacon or carrier and leave the receiver locked: the offset of the carrier from the tuned frequency is the LNB's error. It shows the error in kHz and ppm of the LO, the drift per minute, and says when it has settled (an LNB drifts while it warms up: wait before fine pointing)."));
    if (S.lnb) {
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputFloat(tr("LNB LO (MHz)"), &S.loMhz, 0, 0, "%.0f")) plat::prefs().setD("d40SatLo", S.loMhz);
        bool ok = false;
        const double d = S.drift.driftKhzPerMin(120, &ok);
        if (!ok) ImGui::TextDisabled("%s", tr("Collecting: needs about a minute of a locked carrier."));
        else ImGui::Text("%s %+.1f kHz (%+.1f ppm), %+.1f kHz/min: %s", tr("Error"), S.drift.errorKhz(), S.drift.errorPpm(), d, S.drift.settled() ? tr("settled") : tr("still drifting"));
    }
}
