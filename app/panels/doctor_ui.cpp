// Reception Doctor window (View > Reception doctor): one button takes a short capture from the running receiver and tells, in plain sentences,
// why the mode may not be decoding, with a one-click fix where that is safe. The analysis is core/src/doctor/reception_doctor.cpp
// (docs/RECEPTION-DOCTOR.md); this file is the window, the capture through Engine::setIqTap() and the fixes.
#include "../app.h"
#include "dect2/doctor/reception_doctor.h"
#include <algorithm>
#include <cmath>
#include <mutex>

using namespace dect2;
namespace dr = dect2::doctor;

namespace {

struct State {
    bool capturing = false, have = false;
    std::vector<std::complex<float>> buf;
    size_t want = 0;
    double rate = 0;
    std::mutex mu;
    dr::Report rep;
    dr::Context ctx;
    std::string note;
};
State S;

void startCapture(App& a) {
    if (!a.engine.running()) { S.note = tr("Start the receiver first: the doctor looks at the live signal."); return; }
    std::lock_guard<std::mutex> l(S.mu);
    S.buf.clear(); S.have = false; S.note.clear();
    S.want = (size_t)std::clamp(a.tune.sampleRate * 0.25, 65536.0, 4.0e6);   // a quarter of a second
    S.buf.reserve(S.want);
    S.capturing = true;
    a.engine.setIqTap([](const std::complex<float>* x, size_t n, double rate) {
        std::lock_guard<std::mutex> l(S.mu);
        if (!S.capturing) return;
        S.rate = rate;
        const size_t take = std::min(n, S.want - S.buf.size());
        S.buf.insert(S.buf.end(), x, x + take);
    });
}

void analyse(App& a) {
    dr::Context c;
    {
        std::lock_guard<std::mutex> l(S.mu);
        c.rateHz = S.rate;
    }
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    c.live = d.isRadio();
    c.modeBwHz = a.tune.bandwidthMhz * 1e6;
    c.modeOffsetHz = (a.freqMhz - radioCenterMhz(a)) * 1e6;
    if (d.isGeneric()) { c.gainDb = a.tune.gainDb; c.gainMaxDb = d.gainMaxDb; }
    else { c.gainDb = a.tune.lnaDb + a.tune.vgaDb + (a.tune.ampOn ? 14 : 0); c.gainMaxDb = 104; }
    const SampleLoss l = a.engine.sampleLoss();
    c.deliveredPct = l.deliveredPct;
    c.radioDrops = l.live && l.sinceRadioSec >= 0 && l.sinceRadioSec < 10;
    c.cpuDrops = l.live && l.sinceCpuSec >= 0 && l.sinceCpuSec < 10;
    c.modeId = rateModeId(a);
    c.rfHz = a.freqMhz * 1e6;
    std::vector<std::complex<float>> x;
    { std::lock_guard<std::mutex> l2(S.mu); x.swap(S.buf); }
    S.ctx = c;
    S.rep = dr::diagnose(x.data(), x.size(), c);
    S.have = true;
}

void lowerGain(App& a, double db) {
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (d.isGeneric()) a.tune.gainDb = std::max(d.gainMinDb, a.tune.gainDb - db);
    else {   // HackRF: the VGA first (2 dB steps), then the LNA (8 dB steps), then the amp
        int left = (int)std::lround(db);
        const int v = std::min(left, a.tune.vgaDb - a.tune.vgaDb % 2); a.tune.vgaDb -= v; left -= v;
        while (left > 0 && a.tune.lnaDb >= 8) { a.tune.lnaDb -= 8; left -= 8; }
        if (left > 0 && a.tune.ampOn) a.tune.ampOn = false;
    }
}

void raiseGain(App& a, double db) {
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (d.isGeneric()) a.tune.gainDb = std::min(d.gainMaxDb, a.tune.gainDb + db);
    else { a.tune.vgaDb = std::min(62, a.tune.vgaDb + (int)std::lround(db)); a.tune.vgaDb -= a.tune.vgaDb % 2; }   // the amp is the user's own switch
}

void retuneLive(App& a) {
    if (!a.engine.running()) return;
    a.tune.centerHz = settingsRetuneCentreHz(a);
    a.engine.retune(a.tune);
}

const char* sevName(int s) { return s >= dr::SevBad ? N_("Problem") : s == dr::SevWarn ? N_("Check") : N_("OK"); }
ImVec4 sevCol(int s) { return s >= dr::SevBad ? ImVec4(1, .35f, .3f, 1) : s == dr::SevWarn ? ImVec4(1, .75f, .3f, 1) : ImVec4(.4f, .85f, .45f, 1); }

// the fix button's label and what it does; true when it was pressed
bool fixButton(App& a, const dr::Finding& f) {
    char b[160];
    switch (f.fix) {
    case dr::Fix::LowerGain:
        snprintf(b, sizeof b, tr("Lower gain by %.0f dB"), f.value);
        if (ImGui::SmallButton(b)) { lowerGain(a, f.value); retuneLive(a); return true; }
        break;
    case dr::Fix::RaiseGain:
        snprintf(b, sizeof b, tr("Raise gain by %.0f dB"), f.value);
        if (ImGui::SmallButton(b)) { raiseGain(a, f.value); retuneLive(a); return true; }
        break;
    case dr::Fix::ApplyOffset:
        snprintf(b, sizeof b, tr("Tune %+.1f kHz"), f.value / 1e3);
        if (ImGui::SmallButton(b)) { a.freqMhz += f.value / 1e6; retuneLive(a); return true; }
        break;
    case dr::Fix::WidenRate:
        snprintf(b, sizeof b, tr("Set sample rate to %.1f Msps"), f.value / 1e6);
        if (ImGui::SmallButton(b)) { chooseSampleRate(a, f.value); return true; }
        break;
    case dr::Fix::SwitchMode:
        snprintf(b, sizeof b, tr("Switch to %s"), f.modeId.c_str());
        if (ImGui::SmallButton(b)) { selectModeById(a, f.modeId); return true; }
        break;
    default: break;
    }
    return false;
}

} // namespace

void receptionDoctorWindow(App& a) {
    if (!a.doctorOpen) { if (S.capturing) { a.engine.setIqTap(nullptr); S.capturing = false; } return; }
    firstWindowSize(620 * gUi, 420 * gUi);
    if (!ImGui::Begin(trw("Reception doctor"), &a.doctorOpen)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", tr("Nothing decoding? One click looks at the live signal and the radio and says what is wrong, with a button for the fix where one is safe."));
    if (S.capturing) {   // finished when the buffer is full
        bool done;
        { std::lock_guard<std::mutex> l(S.mu); done = S.buf.size() >= S.want; }
        if (done) { a.engine.setIqTap(nullptr); { std::lock_guard<std::mutex> l(S.mu); S.capturing = false; } analyse(a); }
    }
    ImGui::BeginDisabled(S.capturing);
    if (ImGui::Button(tr(S.have ? "Check again" : "Diagnose"))) startCapture(a);
    ImGui::EndDisabled();
    if (S.capturing) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Listening...")); }
    if (!S.note.empty()) ImGui::TextColored(ImVec4(1, .7f, .3f, 1), "%s", S.note.c_str());
    if (S.have) {
        if (!S.rep.valid) ImGui::TextWrapped("%s", tr("Too few samples arrived to judge. Is the receiver running?"));
        for (size_t i = 0; i < S.rep.findings.size(); i++) {
            const dr::Finding& f = S.rep.findings[i];
            ImGui::PushID((int)i);
            ImGui::TextColored(sevCol(f.severity), "[%s]", tr(sevName(f.severity)));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", tr(f.text.c_str()));   // English from the core; the numbers are in it, so it is shown as is when no translation exists
            if (f.fix != dr::Fix::None && fixButton(a, f)) { S.have = false; S.note = tr("Applied. Press Check again to see if it helped."); }
            ImGui::PopID();
            ImGui::Spacing();
        }
        if (S.rep.valid && ImGui::TreeNode(trw("Numbers"))) {
            const dr::Measure& m = S.rep.m;
            ImGui::Text("%s: %.1f dBFS   %s: %.2f   %s: %.2f %%", tr("level"), m.rmsDbfs, tr("peak"), m.peak, tr("at full scale"), m.clipPct);
            ImGui::Text("%s: %.1f dB   %s: %+.1f kHz, %.1f kHz", tr("strongest over floor"), m.strongestOverFloorDb, tr("at"), m.strongestHz / 1e3, m.strongestBwHz / 1e3);
            ImGui::Text("%s: %.1f dB   %s: %.1f dB", tr("DC spike"), m.dcOverDb, tr("image rejection"), m.imageRejDb);
            ImGui::TreePop();
        }
    }
    ImGui::End();
}
