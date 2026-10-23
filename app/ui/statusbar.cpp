// The status bar under the spectrum (lamps, lock state, signal readouts, level and quality gauges, advice) and the
// gain control. Split from toolbar.cpp; the entry points statusBar() and gainControl() are declared in app.h.
// Each line of the bar is one function: lamps, state and mode readouts, numbers, gauges, notes.
#include "../app.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/source/antenna_ports.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/iq_file.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <thread>
#include "toolbar_internal.h"
#include "transcribe/transcribe_app.h"

// one "label value" readout of the status line, with its icon
static void statusReadout(const App& a, const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
    static const struct { const char* k; Ic ic; } kIcons[] = {
        {"State", Ic::Pulse}, {"Mode", Ic::Layers}, {"FFT", Ic::Grid}, {"GI", Ic::Echo}, {"Pilots", Ic::Target}, {"PLP", Ic::Layers},
        {"CFO", Ic::Wave}, {"SRO", Ic::Clock}, {"SNR", Ic::Signal}, {"MER", Ic::Target}, {"Delay", Ic::Echo}, {"fs", Ic::Gauge},
        {"dropped", Ic::Warning}, {"Channel", Ic::Antenna}, {"Field", Ic::Layers}, {"TPS", Ic::Info}, {"hier", Ic::Layers}, {"Pilot", Ic::Target}};
    ImGui::AlignTextToFramePadding();
    for (auto& e : kIcons) if (!strcmp(e.k, label)) {
        const bool isState = !strcmp(label, "State");
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        iconInline(e.ic, isState ? ImGui::ColorConvertFloat4ToU32(col) : iconDim(), 0.9f);
        ImGui::PopStyleVar();
        ImGui::SameLine(0, 4 * gUi);
        break;
    }
    ImGui::TextDisabled("%s", tr(label)); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
    flowNext(15 * gUi);
}

// line 1: the lock lamps of the standard in use, then TS, video and audio, then a separator
static void statusLamps(App& a, bool run, const RxTelemetry& rx, AdcStatus adc) {
        int iq = run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0;
        lamp("IQ", iq, (int)Ic::Wave); flowNext(12 * gUi);
        const double frameS = rx.frameMs > 0 ? rx.frameMs / 1e3 : 0.5;
        const bool isT = rx.standard == 1;
        const double okFrac = (rx.blocksOk + rx.blocksBad) ? (double)rx.blocksOk / (rx.blocksOk + rx.blocksBad) : 0;
        const int fec = !run || !rx.plpValid || rx.plpFrames == 0 ? 0 : okFrac > 0.995 ? 1 : okFrac > 0.5 ? 2 : 3;
        if (rx.standard == 2) {
            const AtscTelemetry& at = rx.atsc;
            const double okF = (at.rsClean + at.rsCorrected + at.rsFailed) ? (double)(at.rsClean + at.rsCorrected) / (double)(at.rsClean + at.rsCorrected + at.rsFailed) : 0;
            lamp("Pilot", !run ? 0 : at.pilot ? 1 : 0); flowNext(12 * gUi);
            lamp("Seg", !run ? 0 : at.segSync ? 1 : 0); flowNext(12 * gUi);
            lamp("Field", !run ? 0 : at.fieldSync ? 1 : at.segSync ? 2 : 0); flowNext(12 * gUi);
            lamp("Eq", !run ? 0 : at.eqTrained ? 1 : at.fieldSync ? 2 : 0); flowNext(12 * gUi);
            lamp("Trellis", !run ? 0 : at.tsOk ? 1 : at.eqTrained ? 2 : 0); flowNext(12 * gUi);
            lamp("RS", !run || at.fields == 0 ? 0 : okF > 0.995 ? 1 : okF > 0.5 ? 2 : 3); flowNext(12 * gUi);
        } else if (isT) {
            // DVB-T: cyclic-prefix sync, guard interval, TPS signalling, channel estimate, then Viterbi and Reed-Solomon
            lamp("Sync", !run ? 0 : rx.state >= 1 ? 1 : 0); flowNext(12 * gUi);
            lamp("GI", !run ? 0 : rx.giIdx >= 0 ? (rx.state == 2 ? 1 : 2) : 0); flowNext(12 * gUi);
            lamp("TPS", !run ? 0 : rx.dvbt.tpsOk ? 1 : rx.state == 1 ? 2 : 0); flowNext(12 * gUi);
            lamp("Chan", !run ? 0 : rx.chValid && rx.dataValid ? 1 : rx.dvbt.tpsOk ? 2 : 0); flowNext(12 * gUi);
            lamp("Viterbi", !run ? 0 : rx.dvbt.fecSync ? 1 : rx.dvbt.tpsOk ? 2 : 0); flowNext(12 * gUi);
            lamp("RS", fec); flowNext(12 * gUi);
        } else {
        lamp("P1", !run || !a.rxSeen ? 0 : (rx.p1.valid && rx.secSinceP1 < std::max(1.0, 3 * frameS) ? 1 : 2)); flowNext(12 * gUi);
        lamp("GI", !run ? 0 : rx.state == 2 ? 1 : rx.state == 1 ? 2 : 0); flowNext(12 * gUi);
        lamp("L1-pre", !run ? 0 : rx.l1preOk ? 1 : rx.l1preGood > 0 ? 2 : rx.chValid ? 3 : 0); flowNext(12 * gUi);
        lamp("L1-post", !run ? 0 : rx.l1postOk ? 1 : rx.l1postGood > 0 ? 2 : rx.l1preOk ? 3 : 0); flowNext(12 * gUi);
        lamp("Frame", !run ? 0 : rx.dataValid ? 1 : rx.l1preOk ? 2 : 0); flowNext(12 * gUi);
        lamp("LDPC", fec); flowNext(12 * gUi);
        lamp("BCH", fec); flowNext(12 * gUi);
        }
        lamp("TS", !run ? 0 : a.ts.services.empty() ? 0 : (a.ts.ccErrors > 0 && a.bb.framesLost > 0.02 * (a.bb.frames + 1)) ? 2 : 1, (int)Ic::Layers); flowNext(12 * gUi);
        const PlayerStats ps = a.engine.player().stats();
        const bool pl = run && ps.active;
        lamp("Video", !pl || !ps.hasVideo ? 0 : (ps.shown > 0 && ps.videoQueue > 2) ? 1 : 2, (int)Ic::Tv); flowNext(12 * gUi);
        lamp("Audio", !pl || !ps.hasAudio ? 0 : (ps.audioBufferMs > 150) ? 1 : 2, (int)Ic::Speaker); flowNext(10 * gUi);
        ImGui::TextDisabled("|"); flowNext(10 * gUi);
}

// line 1, after the lamps: lock state and the signal mode (standard, FFT, guard interval, pilots, PLP)
static void statusState(App& a, bool run, const RxTelemetry& rx) {
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) { statusReadout(a, label, val, col); };
    char b[96];
    if (!run) ro(N_("State"), a.scanner.progress().running ? tr("channel scan has the radio") : tr("stopped"), ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (a.engine.radioLost()) ro("State", tr("radio disconnected"), ImVec4(0.95f, 0.35f, 0.3f, 1));
    else if (rx.state == 2) ro("State", tr("Locked"), ImVec4(0.35f, 0.90f, 0.45f, 1));
    else if (rx.state == 1) ro("State", tr("syncing"), ImVec4(0.95f, 0.75f, 0.2f, 1));
    else ro("State", tr("searching"), ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (run && rx.standard == 2 && rx.state >= 1) {
        ro(N_("Mode"), "ATSC 8-VSB");
        ro(N_("Channel"), "6 MHz");
        snprintf(b, sizeof b, "%d", rx.atsc.fieldParity); ro("Field", b);
        ro("Pilot", tr(rx.atsc.pilot ? "locked" : "-"));
    } else if (run && rx.standard == 1 && rx.state >= 1) {
        ro("Mode", "DVB-T");
        ro("FFT", rx.fftN == 8192 ? "8K" : "2K");
        ro("GI", dvbt::guardName(rx.giIdx));
        if (rx.dvbt.tpsOk) {
            snprintf(b, sizeof b, "%s %s", dvbt::modName(rx.dvbt.mod), dvbt::rateName(rx.dvbt.crHp));
            ro("TPS", b);
            if (rx.dvbt.hier) ro("hier", tr("yes"), ImVec4(0.95f, 0.7f, 0.2f, 1));
        } else ro("TPS", tr("searching"), ImVec4(0.6f, 0.64f, 0.68f, 1));
    } else if (run && rx.state == 2) {
        ro("Mode", s1Name(rx.p1.s1));
        const FftMode* fm = fftModeFromSize(rx.fftN);
        snprintf(b, sizeof b, "%s%s", fm ? fm->name : "?", rx.extCarriers ? " ext" : ""); ro("FFT", b);
        ro("GI", guardName(rx.giIdx));
        if (rx.l1preOk) { snprintf(b, sizeof b, "PP%d", rx.l1pre.pilotPattern + 1); ro("Pilots", b); }
        if (rx.plpValid) {
            static const char* mods[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"}; static const char* rates[] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "1/3", "2/5"};
            snprintf(b, sizeof b, "%s %s%s", rx.plpFec.mod >= 0 && rx.plpFec.mod < 4 ? mods[rx.plpFec.mod] : "?", rx.plpFec.rate >= 0 && rx.plpFec.rate < 8 ? rates[rx.plpFec.rate] : "?", rx.plpFec.rotation ? " rot" : "");
            ro("PLP", b);
        }
    }
}

// line 2: frequency offset, sample-rate offset, SNR, MER, echo delay and sample rate
static void statusNumbers(App& a, bool run, const RxTelemetry& rx) {
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) { statusReadout(a, label, val, col); };
    char b[96];
    if (run && (rx.state == 2 || (rx.standard == 1 && rx.state >= 1))) {
        snprintf(b, sizeof b, "%+.1f Hz", rx.cfoHz); ro("CFO", b);
        if (rx.standard != 1) { snprintf(b, sizeof b, "%+.1f ppm", rx.sroPpm); ro("SRO", b); }
        snprintf(b, sizeof b, "%.1f dB", rx.dataValid ? rx.dataSnrDb : rx.cpSnrDb); ro("SNR", b);
        if (rx.plpMerDb > 0 && rx.plpMerDb < 90) { snprintf(b, sizeof b, "%.1f dB", rx.plpMerDb); ro("MER", b); }
        if (!a.mpd.report().echoes.empty()) { snprintf(b, sizeof b, "%.2f us", std::fabs(a.mpd.report().echoes[0].delayUs)); ro(N_("Delay"), b); }
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
}

// line 2: the ADC level gauge and the signal quality gauge
static void statusLevelQuality(App& a, bool run, const SignalStats& st, AdcStatus adc) {
    char b[96];
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("%s", tr("Level")); ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? pal::remap(IM_COL32(40, 112, 150, 255)) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("ADC level (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f"), adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    flowNext(15 * gUi);
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Signal, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("%s", tr("Quality")); ImGui::SameLine(0, 5 * gUi);
    {
        const QualityReport& q = a.quality.report();
        const float t = run && q.valid ? (float)q.percent / 100.f : 0.f;
        const ImU32 col = t < 0.25f ? IM_COL32(176, 66, 58, 255) : t < 0.5f ? IM_COL32(176, 130, 48, 255) : pal::remap(IM_COL32(40, 112, 150, 255));
        snprintf(b, sizeof b, run && q.valid ? "%.0f%%  %s" : "-", q.percent, q.label.c_str());
        gaugePill(130, t, col, b);
        if (ImGui::IsItemHovered() && run && q.valid) ImGui::SetTooltip(tr("data SNR %.1f dB, needed about %.1f dB (margin %+.1f dB)\nFEC blocks decoded %.1f%%"), q.snrDb, q.requiredDb, q.marginDb, q.fecOk * 100);
    }
    flowNext(15 * gUi);
}

// line 2, the end: dropped samples, multipath, and the ADC / gain-helper advice
static void statusNotes(App& a, bool run, AdcStatus adc) {
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) { statusReadout(a, label, val, col); };
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro(N_("dropped"), lossText(l), lossColour(l)); lossTooltip(l); }
    if (run && a.mpd.report().level != MultipathLevel::Unknown) {
        const MultipathReport& mr = a.mpd.report();
        const ImVec4 col = mr.level == MultipathLevel::None ? ImVec4(0.5f, 0.55f, 0.6f, 1) : mr.level == MultipathLevel::Mild ? ImVec4(0.95f, 0.8f, 0.3f, 1)
                         : mr.level == MultipathLevel::Likely ? ImVec4(0.95f, 0.6f, 0.2f, 1) : ImVec4(0.95f, 0.35f, 0.25f, 1);
        ImGui::AlignTextToFramePadding();
        iconInline(Ic::Echo, ImGui::ColorConvertFloat4ToU32(col), 0.9f); ImGui::SameLine(0, 4 * gUi);
        ImGui::TextColored(col, tr("multipath: %s"), multipathName(mr.level));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(mr.headline.c_str());
            for (auto& r : mr.reasons) ImGui::BulletText("%s", r.c_str());
            ImGui::EndTooltip();
        }
        flowNext(15 * gUi);
    }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (a.sweep.active()) ImGui::TextColored(pal::heading(), tr("gain helper %d/%d ..."), std::max(0, a.sweep.current()) + 1, (int)a.sweep.entries().size());
        else if (autogainStatus(a)) {}
        else if (gainOptStatus(a)) {}
        else if (adc == AdcStatus::Overload) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), tr("ADC OVERLOAD - %s"), tr(a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)"));
        else if (adc == AdcStatus::High) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "%s", tr("ADC level high"));
        else if (adc == AdcStatus::Low) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "%s", tr("ADC level low - raise the gain"));
        else if (adc == AdcStatus::NoSignal) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "%s", tr("ADC sees almost nothing - antenna / gain?"));
        else flowBreak();
    } else flowBreak();
}

void statusBar(App& a) {
    autodetStatus(a);   // "TV (auto)" / "Digital voice (auto)": what it found or is doing
    {   // live transcripts are on: always visible
        std::string t;
        if (tx::indicator(t)) { ImGui::TextColored(pal::badRed(), "\xE2\x97\x8F"); ImGui::SameLine(0, 4 * gUi); ImGui::TextColored(pal::warnAmber(), "%s", t.c_str()); }
    }
    // a radio on the network: the samples that did not arrive (the connection dropped and came back, or the receiver was too slow)
    if (a.engine.running() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].kind == DeviceInfo::Native &&
        (a.devices[a.devIdx].board == "rtltcp" || a.devices[a.devIdx].board == "spyserver" || a.devices[a.devIdx].board == "sdrpp")) {
        const uint64_t lost = a.engine.droppedSamples();
        if (lost) ImGui::TextColored(pal::warnAmber(), tr("Network radio: %llu samples lost (underruns)"), (unsigned long long)lost);
    }
    // flowEnd(): the last group of the status line wraps too
    if (const ModeUi* mu = modeUi(a.family)) {
        if (mu->status) { mu->status(a); flowEnd(); return; }
        if (mu->tabName && !mu->stream) {   // a mode that is not a TV mode and has no status line of its own: the state and its summary, never the DVB lamps (P1, L1, TS, Video)
            std::string l1, l2;
            if (mu->summary) mu->summary(a, l1, l2);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("State"); ImGui::SameLine();
            ImGui::TextUnformatted(!a.engine.running() ? "stopped" : l2.empty() ? "running" : l2.c_str());
            flowEnd(); return;
        }
    }
    if (a.dabMode) { dabStatus(a); flowEnd(); return; }
    if (a.fmMode) { fmStatus(a); flowEnd(); return; }
    if (a.atsc3Mode) { atsc3Status(a); flowEnd(); return; }
    if (a.isdbtMode) { isdbtStatus(a); flowEnd(); return; }
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const RxTelemetry& rx = a.rx;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    statusLamps(a, run, rx, adc);
    statusState(a, run, rx);
    flowBreak();
    // ---- line 2: numbers and gauges
    statusNumbers(a, run, rx);
    statusLevelQuality(a, run, st, adc);
    statusNotes(a, run, adc);
    flowEnd();
}

void gainControl(App& a) {
    gainOptTick(a);
    if (!a.engine.running() || a.devices[a.devIdx].kind == DeviceInfo::File) { a.sweep = GainSweep(); return; }
    const DeviceInfo& cd = a.devices[a.devIdx];
    const bool generic = cd.isGeneric();
    const int gmax = generic ? std::max(1, (int)cd.gainMaxDb) : 0;
    GainSetting g = generic ? genericGain((int)std::lround(a.tune.gainDb), gmax) : GainSetting{a.tune.lnaDb, a.tune.vgaDb, a.tune.ampOn};
    bool changed = false;
    const double now = ImGui::GetTime();
    if (a.sweep.active()) {
        GainSweep::Sample sm;
        sm.locked = a.rx.dataValid; sm.snrDb = a.rx.dataSnrDb;
        sm.clip = a.spec.stats.clipFraction; sm.rms = a.spec.stats.rmsDbfs; sm.peak = a.spec.stats.peak;
        changed = a.sweep.update(now, sm, g);
        if (changed && a.sweep.rf() >= 0) a.tune.radio["rfgr"] = std::to_string(a.sweep.rf());
        if (!a.sweep.active()) a.engine.log("gain helper: " + a.sweep.summary() + (a.sweep.rf() >= 0 ? ", RF state " + std::to_string(a.sweep.rf()) : std::string()));
    } else {
        // auto gain (autogain_ui.cpp): Auto, Optimise once, Safe start write a.tune themselves; Off keeps only the meters going
        if (autogainStep(a)) {
            a.tune.centerHz = settingsRetuneCentreHz(a);
            a.engine.retune(a.tune);
            savePrefs(a);
        }
        autogainMeterOnly(a);
    }
    if (changed) {
        if (generic) a.tune.gainDb = g.vga; else { a.tune.lnaDb = g.lna; a.tune.vgaDb = g.vga; a.tune.ampOn = g.amp; }
        a.tune.centerHz = settingsRetuneCentreHz(a);
        a.engine.retune(a.tune);
        if (!a.sweep.active() || true) savePrefs(a);
        if (a.agcOn && !a.sweep.active() && generic) a.engine.log("AGC: gain " + std::to_string(g.vga) + " dB");
        else if (a.agcOn && !a.sweep.active()) a.engine.log("AGC: LNA " + std::to_string(g.lna) + " dB, VGA " + std::to_string(g.vga) + " dB, amp " + (g.amp ? "on" : "off"));
    }
}
