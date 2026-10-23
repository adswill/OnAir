// "Optimise gain": the button, the sweep that drives GainOptimiser (core/src/source/gain_opt.cpp) from the engine's telemetry and spectrum, the progress / result
// line and the undo. The AGC is paused for the sweep and stays off after it (it would move the gain the sweep chose); Undo puts the gain and the AGC back.
#include "../app.h"
#include <cmath>

using namespace dect2;

namespace {

// the radio's gain scale for the sweep, in whole dB: a generic radio's own range; the HackRF's three stages as one total (gainForTotal)
bool gainRange(const App& a, int& lo, int& hi) {
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (!d.isRadio()) return false;
    // the amp is not the sweep's to switch: it stays as the user set it (its 14 dB are then part of every total), and joins only above the range when allowed
    if (d.isGeneric()) { lo = (int)std::ceil(d.gainMinDb); hi = std::min((int)std::floor(d.gainMaxDb), agcMaxTotal(const_cast<App&>(a))); return hi > lo; }
    lo = a.tune.ampOn ? 14 : 0; hi = agcMaxTotal(const_cast<App&>(a));
    return true;
}
int currentGain(const App& a) {
    return a.devices[(size_t)a.devIdx].isGeneric() ? (int)std::lround(a.tune.gainDb) : a.tune.lnaDb + a.tune.vgaDb + (a.tune.ampOn ? 14 : 0);
}
void setGain(App& a, int g, const GainSetting* stages = nullptr) {
    if (a.devices[(size_t)a.devIdx].isGeneric()) a.tune.gainDb = g;
    else {
        const GainSetting s = stages ? *stages : stagesFor(a, g);
        a.tune.lnaDb = s.lna; a.tune.vgaDb = s.vga; a.tune.ampOn = s.amp;
    }
}
void apply(App& a) {
    if (!a.engine.running()) return;
    a.tune.centerHz = settingsRetuneCentreHz(a);
    a.engine.retune(a.tune);
}

const char* howText(GainOptimiser::Result r) {
    switch (r) {
    case GainOptimiser::Result::Snr: return N_("judged by the mode's SNR");
    case GainOptimiser::Result::Spectrum: return N_("judged by the SNR in the spectrum");
    case GainOptimiser::Result::Level: return N_("judged by the ADC level");
    default: return "";
    }
}

} // namespace

void gainOptButtons(App& a, bool running) {
    GainOptSession& s = a.gopt;
    int lo = 0, hi = 0;
    const bool can = running && gainRange(a, lo, hi) && !a.sweep.active() && a.dir.state() != DirectionFinder::State::Measuring;
    // the button goes beside the row when its label fits the panel now (flowNext() used last frame's width, so it could run past the edge)
    const float btnPad = 2.f * ImGui::GetStyle().FramePadding.x;
    const char* optLbl = s.opt.active() ? tr("Stop optimising") : tr("Optimise gain");
    sameLineIf(ImGui::CalcTextSize(optLbl).x + btnPad, ImGui::GetStyle().ItemSpacing.x);
    if (s.opt.active()) {
        if (ImGui::Button(tr("Stop optimising"))) {
            int g = 0;
            if (s.opt.cancel(g)) { setGain(a, s.undoDb, &s.undoStages); apply(a); a.agcOn = s.undoAgc; agcReset(a); s.finished = false; s.undoable = false; a.engine.log("gain optimiser: stopped, gain back to " + std::to_string(s.undoDb) + " dB"); }
        }
    } else {
        ImGui::BeginDisabled(!can);
        if (ImGui::Button(tr("Optimise gain"))) {
            s.undoDb = currentGain(a);
            s.undoStages = GainSetting{a.tune.lnaDb, a.tune.vgaDb, a.tune.ampOn};
            s.undoAgc = a.agcOn;
            s.undoable = false; s.finished = false;
            a.agcOn = false;
            s.opt.start(ImGui::GetTime(), lo, hi, s.undoDb);
            a.engine.log("gain optimiser: sweeping " + std::to_string(lo) + " to " + std::to_string(hi) + " dB from " + std::to_string(s.undoDb) + " dB");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr("Optimise gain: steps through the radio's gain range and measures the reception quality at each step (the mode's SNR while it is locked, else the signal-to-noise "
                                       "ratio of the spectrum around the channel, else the ADC level), then sets the lowest gain that is within 0.5 dB of the best quality and does not "
                                       "overload the ADC. Takes about 40 s and needs a signal. The AGC is paused and stays off."));
        if (s.undoable) {
            sameLineIf(ImGui::CalcTextSize(tr("Undo gain")).x + btnPad, ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::Button(tr("Undo gain"))) {
                setGain(a, s.undoDb, &s.undoStages);
                apply(a);
                a.agcOn = s.undoAgc; agcReset(a);
                s.undoable = false; s.finished = false;
                savePrefs(a);
                a.engine.log("gain optimiser: undone, gain back to " + std::to_string(s.undoDb) + " dB");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Go back to the gain (and the AGC setting) from before the last optimisation"));
        }
    }
}

void gainOptTick(App& a) {
    GainOptSession& s = a.gopt;
    if (!s.opt.active()) return;
    const bool ok = a.engine.running() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[(size_t)a.devIdx].isRadio() && !a.sweep.active();
    if (!ok) {   // the receiver stopped, another radio was chosen or the older gain helper started: back to where it was
        int g = 0;
        s.opt.cancel(g);
        if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[(size_t)a.devIdx].isRadio()) { setGain(a, s.undoDb, &s.undoStages); apply(a); }
        a.agcOn = s.undoAgc;
        s.finished = false; s.undoable = false;
        return;
    }
    GainOptimiser::Sample sm;
    double v = 0;
    sm.snrValid = rxSnrDb(a.rx, v); sm.snrDb = v;
    const double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    // the channel the mode listens to: its width, and where it sits in the capture (a mode with a tuning offset is not on the centre)
    sm.altValid = spectrumSnrDb(a.spec.dbfs, fs, a.tune.bandwidthMhz * 1e6, v, (a.freqMhz - radioCenterMhz(a)) * 1e6); sm.altDb = v;
    sm.clip = a.spec.stats.clipFraction; sm.rms = a.spec.stats.rmsDbfs; sm.peak = a.spec.stats.peak;
    int g = 0;
    if (s.opt.update(ImGui::GetTime(), sm, g)) {
        setGain(a, g);
        apply(a);
    }
    if (!s.opt.active()) {
        s.finished = true;
        s.resultDb = s.opt.bestGain();
        s.how = s.opt.result();
        s.finishedAt = ImGui::GetTime();
        s.undoable = s.resultDb != s.undoDb;
        savePrefs(a);
        a.engine.log("gain optimiser: " + std::to_string(s.resultDb) + " dB (was " + std::to_string(s.undoDb) + " dB)" +
                     (s.how == GainOptimiser::Result::Snr ? ", by the mode's SNR" : s.how == GainOptimiser::Result::Spectrum ? ", by the spectrum SNR" : s.how == GainOptimiser::Result::Level ? ", by the ADC level" : ", every step overloaded the ADC"));
    }
}

bool gainOptStatus(App& a) {
    GainOptSession& s = a.gopt;
    if (s.opt.active()) {
        ImGui::TextColored(pal::heading(), tr("Optimising gain %d/%d: %d dB ..."), std::min(s.opt.stepsDone() + 1, s.opt.stepsTotal()), s.opt.stepsTotal(), s.opt.testing());
        return true;
    }
    if (!s.finished || ImGui::GetTime() - s.finishedAt > 25) return false;
    if (s.how == GainOptimiser::Result::AllClipped) {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "%s", tr("Every gain step overloaded the ADC: the lowest gain was kept"));
    } else {
        ImGui::TextColored(pal::okGreen(), tr("Gain optimised: %d dB (was %d dB)"), s.resultDb, s.undoDb);
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextDisabled("%s", tr(howText(s.how)));
    }
    return true;
}
