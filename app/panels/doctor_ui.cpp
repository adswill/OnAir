// Reception Doctor window (View > Reception doctor): one button takes a short capture from the running receiver and tells, in plain sentences,
// why the mode may not be decoding, with a one-click fix where that is safe. The analysis is core/src/doctor/reception_doctor.cpp
// (docs/RECEPTION-DOCTOR.md); this file is the window, the capture through Engine::setIqTap() and the fixes.
#include "../app.h"
#include "dect2/doctor/decode_progress.h"
#include "dect2/doctor/reception_doctor.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <mutex>

using namespace dect2;
namespace dr = dect2::doctor;

namespace {

struct State {
    bool capturing = false, have = false;
    std::vector<std::complex<float>> buf;
    std::vector<std::complex<float>> clip;   // the samples of the last diagnosis, kept for the "send this failed signal" clip until the next check
    dr::AutoOpen autoOpen;
    bool autoCheck = false;      // the window opened by itself: run the check without being asked
    bool autoLoaded = false, autoOn = true;
    bool clipOpen = false, clipConsent = false;
    std::string clipNote;
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
    c.modeBwHz = a.tune.signalBwMhz() * 1e6;
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
    S.clip = std::move(x);
    S.clipOpen = false; S.clipConsent = false; S.clipNote.clear();
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

// How far the current decoder got, from what every receiver reports in common: its state (0 searching, 1 partly locked, 2 locked) and its good / bad frame counts
template <class T> void readStage(const T& t, dr::StageInput& in) {
    if constexpr (requires { t.state; }) in.syncState = (int)t.state;
    if constexpr (requires { t.blocksOk; }) in.blocksOk = (int64_t)t.blocksOk;
    if constexpr (requires { t.blocksBad; }) in.blocksBad = (int64_t)t.blocksBad;
}

dr::StageInput stageInput(const App& a) {
    dr::StageInput in;
    const RxTelemetry& t = a.rx;
    const SampleLoss l = a.engine.sampleLoss();
    in.samples = a.engine.running() && (!l.live || l.deliveredPct < 0 || l.deliveredPct > 1);
#define STAGE(num, member) case num: readStage(t.member, in); break;
    switch (t.standard) {
    case 0: case 1:
        in.syncState = t.state;
        if (t.standard == 1) in.syncState = t.dvbt.fecSync ? 2 : t.dvbt.tpsOk ? 1 : 0;
        in.blocksOk = t.dataValid ? 1 : 0;
        break;
    STAGE(2, atsc) STAGE(3, dab) STAGE(6, fm) STAGE(7, dvbs) STAGE(8, dtmb) STAGE(10, dmr) STAGE(11, drm) STAGE(12, adsb) STAGE(13, gnss) STAGE(14, sonde) STAGE(15, ais) STAGE(16, marine) STAGE(17, acars)
    STAGE(18, inmc) STAGE(19, aero) STAGE(20, iridium) STAGE(21, mesh) STAGE(22, hdr) STAGE(23, cdr) STAGE(24, pager) STAGE(25, packet) STAGE(26, hfdig) STAGE(27, airband) STAGE(28, vdl2) STAGE(83, vdl4)
    STAGE(29, ism) STAGE(32, m17) STAGE(36, hfdl) STAGE(41, dvbc) STAGE(45, orbcomm) STAGE(50, ale) STAGE(51, p25) STAGE(52, ble) STAGE(62, cube) STAGE(64, vdes) STAGE(66, zigbee) STAGE(67, flarm) STAGE(69, dji) STAGE(72, loraraw)
    default: break;
    }
#undef STAGE
    return in;
}

const char* stageWord(dr::StageState s) {
    switch (s) {
    case dr::StageState::Yes: return tr("yes");
    case dr::StageState::Partial: return tr("partly");
    case dr::StageState::No: return tr("no");
    default: return tr("not reported");
    }
}

ImVec4 stageCol(dr::StageState s) {
    switch (s) {
    case dr::StageState::Yes: return ImVec4(.45f, .85f, .5f, 1);
    case dr::StageState::Partial: return ImVec4(1, .75f, .3f, 1);
    case dr::StageState::No: return ImVec4(1, .4f, .35f, 1);
    default: return ImVec4(.6f, .6f, .6f, 1);
    }
}

void stageLadder(const App& a) {
    const dr::Progress p = dr::progress(S.have ? S.rep : dr::Report(), stageInput(a));
    const struct { const char* name; dr::StageState st; } rows[4] = {{tr("Signal"), p.signal}, {tr("Carrier"), p.carrier}, {tr("Sync"), p.sync}, {tr("Error check"), p.fec}};
    ImGui::TextUnformatted(tr("How far the decoder got:"));
    for (const auto& r : rows) {
        ImGui::SameLine(0, 14 * gUi);
        ImGui::TextColored(stageCol(r.st), "%s: %s", r.name, stageWord(r.st));
    }
}

void coachPanel(const App& a) {
    if (!S.have || !S.rep.valid) return;
    bool known = false, forBand = false;
    stationAntennaFor(a, known, forBand);
    const dr::Coaching c = dr::coach(S.rep, S.ctx, known, forBand);
    bool any = false;
    for (dr::Advice ad : c.advice) {
        const char* text = nullptr;
        char b[120];
        switch (ad) {
        case dr::Advice::GainDown: snprintf(b, sizeof b, tr("Lower the gain by about %.0f dB: the signal is overloading the radio."), c.gainDeltaDb); text = b; break;
        case dr::Advice::GainUp: snprintf(b, sizeof b, tr("Raise the gain by about %.0f dB: the level is low."), c.gainDeltaDb); text = b; break;
        case dr::Advice::GainGood: text = tr("The gain is about right."); break;
        case dr::Advice::LnaHelps: text = tr("The gain is at its maximum: an LNA at the antenna would help."); break;
        case dr::Advice::NeedsAntennaForBand: text = tr("Your station lists no antenna for this frequency. An antenna made for this band will do more than any setting."); break;
        case dr::Advice::AntennaOk: text = tr("Your antenna suits this frequency."); break;
        case dr::Advice::MoveAntenna: text = tr("Try the antenna higher up, near a window or outdoors, away from computers and chargers."); break;
        case dr::Advice::NoAdvice: break;
        }
        if (!text) continue;
        if (!any) { ImGui::Spacing(); ImGui::TextUnformatted(tr("Antenna and gain")); ImGui::Separator(); any = true; }
        ImGui::BulletText("%s", text);
    }
}

// One clip at a time, and only with this clip's own tick: nothing leaves the computer here, the user attaches the saved files themselves.
void clipPanel(App& a) {
    if (!S.have || S.clip.empty()) return;
    if (!S.clipOpen) { if (ImGui::Button(tr("Send this failed signal..."))) { S.clipOpen = true; S.clipConsent = false; S.clipNote.clear(); } return; }
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Send this failed signal"));
    ImGui::Separator();
    const double secs = S.ctx.rateHz > 0 ? (double)S.clip.size() / S.ctx.rateHz : 0;
    ImGui::TextWrapped(tr("This clip is %.2f s of what the radio received at %.4f MHz, %.1f MB, mode %s. It holds everything that was on the air in that moment, including any voice. It contains no location and nothing about you."),
                       secs, a.freqMhz, (double)dr::clipBytes(S.clip.size()) / 1e6, S.ctx.modeId.c_str());
    ImGui::Checkbox(tr("I agree to share this clip"), &S.clipConsent);
    ImGui::BeginDisabled(!S.clipConsent);
    if (ImGui::Button(tr("Save clip"))) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::path(plat::dataDir()) / "failed_signals";
        fs::create_directories(dir, ec);
        char name[80];
        snprintf(name, sizeof name, "clip_%lld", (long long)std::time(nullptr));
        const std::string base = (dir / name).string();
        dr::ClipInfo info; info.rateHz = S.ctx.rateHz; info.freqHz = S.ctx.rfHz; info.modeId = S.ctx.modeId;
        std::string err;
        if (dr::writeClip(base, S.clip.data(), S.clip.size(), info, &err)) {
            S.clipNote = std::string(tr("Saved. Attach both files to a GitHub issue or the signal sample library:")) + " " + base + ".sigmf-*";
            S.clipConsent = false;
        } else S.clipNote = err;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) { S.clipOpen = false; S.clipConsent = false; }
    if (!S.clipNote.empty()) ImGui::TextWrapped("%s", S.clipNote.c_str());
}

void autoTick(App& a) {
    if (!S.autoLoaded) { S.autoLoaded = true; S.autoOn = plat::prefs().getB("doctorAutoOpen", true); }
    if (!S.autoOn || a.doctorOpen) return;
    const bool radio = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[(size_t)a.devIdx].isRadio();
    const dr::StageInput in = stageInput(a);
    const bool decoding = in.syncState >= 2 || in.blocksOk > 0;
    const bool knowsState = in.syncState >= 0 || in.blocksOk >= 0;   // a mode that reports nothing cannot be said to have failed
    if (S.autoOpen.update(ImGui::GetTime(), radio && a.engine.running() && knowsState, decoding, rateModeId(a))) { a.doctorOpen = true; S.autoCheck = true; }
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
    autoTick(a);
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
    if (S.autoCheck && !S.capturing) { S.autoCheck = false; startCapture(a); }
    stageLadder(a);
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
        coachPanel(a);
        ImGui::Spacing();
        clipPanel(a);
    }
    if (ImGui::CollapsingHeader(trw("Advanced"))) {
        if (ImGui::Checkbox(tr("Open by itself when nothing decodes"), &S.autoOn)) plat::prefs().setB("doctorAutoOpen", S.autoOn);
    }
    ImGui::End();
}
