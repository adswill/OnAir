// The auto-gain loop's glue to the app: it configures SmartAgc (core/src/source/agc.cpp) for the radio and mode in use, feeds it the ADC statistics,
// the spectrum and the decoder's SNR, applies what it decides, and offers the gear button with the "Let AGC turn the amp on/off by itself" switch.
// The amp is off by default for everyone and the AGC never touches it unless the user allowed that for this radio (docs/auto-gain.md).
#include "../ui/panel_layout.h"
#include "../app.h"
#include <cmath>

using namespace dect2;

namespace {

const char* reasonText(AgcReason r) {
    switch (r) {
    case AgcReason::Overload: case AgcReason::Reverted: return N_("cut after an overload");
    case AgcReason::TooHot: case AgcReason::Wasted: return N_("lowering the gain");
    case AgcReason::TooLow: case AgcReason::Margin: case AgcReason::Recovering: return N_("raising the gain");
    case AgcReason::Blocker: return N_("strong neighbour: front gain limited");
    case AgcReason::Locked: return N_("decoder locked: gain frozen");
    case AgcReason::LimitHit: return N_("at the end of the gain range");
    case AgcReason::TopGain: return N_("least attenuation: this radio sets its own level");
    default: return N_("holding the level");
    }
}
const char* profileText(AgcClass c) {
    switch (c) {
    case AgcClass::Narrow: return N_("narrow channel: lowest noise, neighbours kept out");
    case AgcClass::Pulsed: return N_("pulsed signals: peaks decide");
    case AgcClass::Strong: return N_("strong stations: front end protected");
    default: return N_("wide channel: ADC headroom");
    }
}

std::string ampKey(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (!d.isRadio() || !gainModelFor(d).ampSwitch) return "";
    const std::string rk = radioKeyOf(d);
    return rk.empty() ? "" : "agcamp." + rk;
}

// the amp as the radio has it now: HackRF, its switch; AirspyHF+, whether the preamp is set by hand (not "auto": then it does not follow the slider)
bool ampNow(const App& a, const GainModel& m) {
    if (!m.ampSwitch) return false;
    if (m.ampIsTopOfScale) {
        const auto it = a.tune.radio.find(m.ampSettingKey);
        return it != a.tune.radio.end() && it->second != "auto";
    }
    return a.tune.ampOn;
}

} // namespace

GainModel agcModel(const App& a) {
    return gainModelFor(a.devices[(size_t)a.devIdx], adcBits());
}

int agcMaxTotal(App& a) {
    const GainModel m = agcModel(a);
    return m.maxTotal(ampNow(a, m), agcAmpAllowed(a), 40) + (m.kind == GainModel::HackRf && !a.tune.ampOn && agcAmpAllowed(a) ? m.ampDb : 0);
}

bool agcAmpSwitchable(const App& a) { return !ampKey(a).empty(); }

bool agcAmpAllowed(App& a) {
    const std::string k = ampKey(a);
    if (k != a.agcAmpCtx) { a.agcAmpCtx = k; a.agcAmpAllow = !k.empty() && plat::prefs().getI(k.c_str(), 0) != 0; }
    return a.agcAmpAllow && !k.empty();
}

GainSetting stagesFor(App& a, int totalDb) {
    const GainModel m = agcModel(a);
    bool amp = a.tune.ampOn;
    if (!amp && agcAmpAllowed(a) && totalDb > m.maxTotal(false, false, 40)) amp = true;   // only to reach beyond the range without it, and only when allowed
    return m.split(totalDb, amp, 40);
}

void agcReset(App& a) { a.agcCtx.clear(); }

bool agcStep(App& a, GainSetting& g) {
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    const bool generic = d.isGeneric();
    const double now = ImGui::GetTime();
    const int bits = adcBits();
    const GainModel m = gainModelFor(d, bits);
    const double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    const double bw = a.tune.signalBwMhz() * 1e6;
    const int std_ = engineStd(a);
    const std::string ctx = radioKeyOf(d) + "|" + std::to_string(std_) + "|" + std::to_string((long)bw) + "|" + std::to_string((long)fs) + "|" + std::to_string(bits);
    const int curTotal = generic ? (int)std::lround(a.tune.gainDb) : a.tune.lnaDb + a.tune.vgaDb + (a.tune.ampOn ? 14 : 0);
    const bool amp = ampNow(a, m);
    if (!a.agc.begun() || ctx != a.agcCtx) {   // switched on, or another radio / mode / width: its own profile and a fresh acquisition
        a.agc.configure(m, agcProfileFor(std_, bw, fs, bits));
        a.agc.begin(now, curTotal, amp);
        a.agcCtx = ctx;
        a.agcFreqMhz = a.freqMhz;
    } else if (std::fabs(a.freqMhz - a.agcFreqMhz) > 1e-9) {   // a retune: another band, another level
        a.agc.begin(now, a.agc.totalDb(), a.agc.ampOn());
        a.agcFreqMhz = a.freqMhz;
    }
    a.agc.setAmpAllowed(agcAmpAllowed(a));
    const AgcMeasure me = measureFront(a.spec.stats, a.spec.dbfs, fs, bw, (a.freqMhz - radioCenterMhz(a)) * 1e6, bits);
    AgcQuality q;
    double snr = 0;
    q.locked = q.snrValid = rxSnrDb(a.rx, snr);
    q.snrDb = snr;
    AgcCommand c;
    if (!a.agc.update(now, me, q, c)) return false;
    if (generic) { g = GainSetting(); g.vga = c.totalDb; }
    else g = c.stages;
    return true;
}

void agcAmpWarning(const App& a) {   // on a row of its own: the caller starts it
    if (!a.tune.ampOn) return;
    // a small warning mark and one short line; the whole story is in the tooltip
    panel::warnIcon();
    const bool hov1 = ImGui::IsItemHovered();
    ImGui::SameLine(0, 4 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(0);   // a narrow panel: the line wraps instead of running off the edge
    ImGui::TextColored(pal::warnAmber(), "%s", tr("can be damaged by strong signals"));
    ImGui::PopTextWrapPos();
    if (hov1 || ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The amp can fry itself on strong signals while it is on. Leave it off unless the antenna is weak and nothing strong is near."));
}

void agcGear(App& a) {
    const float sz = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool shown = ImGui::IsRectVisible(p, ImVec2(p.x + sz, p.y + sz));
    if (ImGui::InvisibleButton("##agcgear", ImVec2(sz, sz)) || (a.agcGearOpen && shown && ImGui::GetFrameCount() > 150)) { ImGui::OpenPopup("##agcset"); a.agcGearOpen = false; }
    const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    // a small gear: a ring with eight teeth
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(hov ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    const float r = sz * 0.22f, t = std::max(1.5f, sz * 0.09f);
    for (int i = 0; i < 8; i++) {
        const float an = (float)i * 0.7853982f;
        dl->AddLine(ImVec2(c.x + std::cos(an) * r, c.y + std::sin(an) * r), ImVec2(c.x + std::cos(an) * r * 1.62f, c.y + std::sin(an) * r * 1.62f), col, t * 1.3f);
    }
    dl->AddCircle(c, r, col, 16, t);
    if (hov) ImGui::SetTooltip("%s", tr("AGC settings"));
    ImGui::SetNextWindowPos(ImVec2(p.x, p.y + sz + 2 * gUi), ImGuiCond_Appearing);   // under the gear (a popup would open at the mouse)
    if (!ImGui::BeginPopup("##agcset")) return;
    ImGui::TextColored(pal::heading(), "%s", tr("Auto gain"));
    const bool on = a.agcOn && a.agc.begun();
    if (on) {
        ImGui::TextWrapped(tr("Aims for: %s"), tr(profileText(a.agc.profile().cls)));
        ImGui::TextDisabled(tr("Now: %s"), tr(reasonText(a.agc.reason())));
    }
    ImGui::Separator();
    if (agcAmpSwitchable(a)) {
        bool allow = agcAmpAllowed(a);
        if (ImGui::Checkbox(tr("Let AGC turn the amp on/off by itself"), &allow)) {
            a.agcAmpAllow = allow;
            const std::string k = ampKey(a);
            plat::prefs().setI(k.c_str(), allow ? 1 : 0);
            plat::prefs().flush();
            a.agc.setAmpAllowed(allow);
        }
        ImGui::PushTextWrapPos(300 * gUi);
        ImGui::TextDisabled("%s", tr("Off by default. With this off the AGC only moves the LNA and VGA and never touches the amp: it stays as you set it. The amp is the first stage and can be damaged by strong signals."));
        ImGui::PopTextWrapPos();
        if (allow) { ImGui::PushTextWrapPos(300 * gUi); ImGui::TextColored(pal::warnAmber(), "(!) %s", tr("The amp can fry itself on strong signals while on")); ImGui::PopTextWrapPos(); }
    } else {
        ImGui::TextDisabled("%s", tr("This radio has no amp for the AGC to switch."));
    }
    ImGui::EndPopup();
}
