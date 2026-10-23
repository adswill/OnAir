// Auto gain in the app (docs/AUTOGAIN.md): the "Auto gain" control on the gain row (Off / Auto / Optimise once / Safe start), the live meter
// strip (ADC headroom bar with a clip lamp, noise floor, plain-language warnings), the advanced popup (targets, limits, stages, metric, history
// graph, log, remembered gains, the radio's gain chain), and the glue that drives dect2::ag::AutoGainController (core/src/source/autogain.cpp)
// from the engine's spectrum, ADC statistics and the mode's telemetry.
//
// "Auto" is a.agcOn (so everything that turns the AGC off when the user touches a gain, the remote API's "agc", --agc, still works); Optimise
// once and Safe start run once and leave the result (with Undo). The amp and the antenna power are never switched unless the user allowed the
// amp in the popup ("Let auto gain switch the amp", saved per radio as before: agcamp.<radio>).
#include "../app.h"
#include "dect2/source/autogain.h"
#include <cmath>
#include <ctime>

using namespace dect2;
namespace ag = dect2::ag;

namespace {

struct AgUi {
    ag::AutoGainController c;
    ag::AgSettings set;
    ag::GainMemory mem;
    bool loaded = false;
    std::string ctx;                 // radio | mode | width | rate: a new one configures the controller afresh
    double ctxFreq = -1;
    ag::RadioProfile prof;
    ag::AgMode run = ag::AgMode::Off;    // the mode the controller runs now
    ag::AgMode oneShot = ag::AgMode::Off; // Optimise once / Safe start asked for (it ends by itself)
    // undo of the last one-shot
    bool undoable = false;
    ag::StagePlan undoPlan;
    TuneSettings undoTune;
    bool undoAgc = false;
    std::string result;
    double resultAt = -1e9;
    double lastClipAt = -1e9;
    bool popup = false;
};
AgUi& S() { static AgUi s; return s; }

const char* kModeNames[] = {"Off", "Auto", "Optimise once", "Safe start"};

bool radioOk(const App& a) { return a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[(size_t)a.devIdx].isRadio(); }

void loadSettings() {
    AgUi& s = S();
    if (s.loaded) return;
    s.loaded = true;
    auto& p = plat::prefs();
    s.set.headroomDb = p.getD("ag.headroom", -1);
    s.set.maxGainDb = p.getD("ag.maxgain", 1e9);
    s.set.stepDb = p.getD("ag.step", 0);
    s.set.loopSpeed = p.getD("ag.speed", 1);
    s.set.settleSec = p.getD("ag.settle", 0.8);
    s.set.measureSec = p.getD("ag.measure", 1.2);
    s.set.safeRiseDb = p.getD("ag.saferise", -1);
    s.set.metric = (ag::Metric)p.getI("ag.metric", 0);
    s.set.imdProbeAuto = p.getB("ag.imdprobe", false);
    s.mem.parse(p.getS("ag.mem", ""));
}
void saveSettings() {
    AgUi& s = S();
    auto& p = plat::prefs();
    p.setD("ag.headroom", s.set.headroomDb);
    p.setD("ag.maxgain", s.set.maxGainDb);
    p.setD("ag.step", s.set.stepDb);
    p.setD("ag.speed", s.set.loopSpeed);
    p.setD("ag.settle", s.set.settleSec);
    p.setD("ag.measure", s.set.measureSec);
    p.setD("ag.saferise", s.set.safeRiseDb);
    p.setI("ag.metric", (long)s.set.metric);
    p.setB("ag.imdprobe", s.set.imdProbeAuto);
    p.setS("ag.mem", s.mem.serialise());
    p.flush();
}
std::string maskKey(const App& a) { return "ag.mask." + radioKeyOf(a.devices[(size_t)a.devIdx]); }

std::string memKey(const App& a) { return ag::GainMemory::key(radioKeyOf(a.devices[(size_t)a.devIdx]), engineStd(a), a.freqMhz); }

ag::AgInput input(App& a) {
    ag::AgInput in;
    in.now = ImGui::GetTime();
    in.adc = ag::fromSignalStats(a.spec.stats);
    in.spectrum = &a.spec.dbfs;
    in.rateHz = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    in.chanBwHz = a.tune.bandwidthMhz * 1e6;
    in.offsetHz = (a.freqMhz - radioCenterMhz(a)) * 1e6;   // a mode tuned beside the DC spike listens off the centre
    in.q = ag::qualityFrom(a.rx);
    return in;
}

// the controller for the radio and mode in use (afresh when either changed)
void syncContext(App& a) {
    AgUi& s = S();
    loadSettings();
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    const double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    const std::string ctx = radioKeyOf(d) + "|" + std::to_string(engineStd(a)) + "|" + std::to_string((long)(a.tune.bandwidthMhz * 1e6)) + "|" + std::to_string((long)fs);
    s.set.ampAllowed = agcAmpAllowed(a);
    s.set.stageMask = (uint32_t)plat::prefs().getI(maskKey(a).c_str(), (long)0xffffffffL);
    if (ctx == s.ctx) { s.c.setSettings([&] { ag::AgSettings x = s.set; x.mode = s.run; return x; }()); return; }
    s.ctx = ctx;
    s.prof = ag::radioProfileFor(d);
    ag::AgSettings x = s.set;
    x.mode = s.run;
    s.c.configure(s.prof, ag::modeTargetFor(engineStd(a), a.tune.bandwidthMhz * 1e6, fs), x);
    if (s.run != ag::AgMode::Off) {
        const ag::StagePlan p = ag::planFromTune(s.prof, a.tune);
        s.c.start(ImGui::GetTime(), p.totalDb, p.ampOn, &p);
    }
    s.ctxFreq = a.freqMhz;
}

void startMode(App& a, ag::AgMode m) {
    AgUi& s = S();
    syncContext(a);
    if (s.run == ag::AgMode::Once || s.run == ag::AgMode::Safe) {   // a one-shot in progress: stopped, back to where it started
        if (s.c.active()) { ag::applyPlan(s.prof, s.c.startPlan(), a.tune); if (a.engine.running()) { a.tune.centerHz = settingsRetuneCentreHz(a); a.engine.retune(a.tune); } }
    }
    s.run = m;
    s.oneShot = (m == ag::AgMode::Once || m == ag::AgMode::Safe) ? m : ag::AgMode::Off;
    if (m == ag::AgMode::Once || m == ag::AgMode::Safe) {
        s.undoTune = a.tune; s.undoAgc = a.agcOn; s.undoable = false;
        a.agcOn = false;
    }
    ag::AgSettings x = s.set;
    x.mode = m;
    s.c.setSettings(x);
    if (m == ag::AgMode::Off) { s.c.stop(); return; }
    const ag::StagePlan p = ag::planFromTune(s.prof, a.tune);
    s.c.start(ImGui::GetTime(), p.totalDb, p.ampOn, &p);
    a.engine.log(std::string("auto gain: ") + ag::agModeName(m) + " on " + s.prof.name + " (" + ag::modeKindName(s.c.target().kind) + ")");
}

void applyCmd(App& a, const ag::AgCommand& cmd) {
    AgUi& s = S();
    const bool ampBefore = a.tune.ampOn;
    ag::applyPlan(s.prof, cmd.plan, a.tune);
    if (!s.set.ampAllowed) a.tune.ampOn = ampBefore;   // never the controller's to switch without permission (belt and braces)
}

void drawHeadroomBar(float w) {
    AgUi& s = S();
    const ag::AgMeters& m = s.c.meters();
    const float h = ImGui::GetFrameHeight() * 0.62f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##aghb", ImVec2(w, ImGui::GetFrameHeight()));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y0 = p.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w, y0 + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
    // level: full scale at the right; 0..48 dB below it
    const double tgt = s.c.headroomTarget();
    const double head = m.valid ? std::max(0.0, std::min(48.0, m.headroomDb)) : 48;
    const float fill = (float)(1.0 - head / 48.0);
    const ImVec4 col = m.clipping ? pal::badRed() : head < tgt - 3 ? pal::warnAmber() : head > tgt + 18 ? ImVec4(0.45f, 0.6f, 0.85f, 1) : pal::okGreen();
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w * fill, y0 + h), ImGui::ColorConvertFloat4ToU32(col), 3);
    // the target mark
    const float tx = p.x + w * (float)(1.0 - std::min(48.0, tgt) / 48.0);
    dl->AddLine(ImVec2(tx, y0 - 2), ImVec2(tx, y0 + h + 2), ImGui::GetColorU32(ImGuiCol_Text), 1.5f);
    // the clip lamp, held for 2 s
    if (m.clipping) s.lastClipAt = ImGui::GetTime();
    const bool lamp = ImGui::GetTime() - s.lastClipAt < 2.0;
    const float r = h * 0.45f;
    dl->AddCircleFilled(ImVec2(p.x + w + r + 4, y0 + h * 0.5f), r, lamp ? ImGui::ColorConvertFloat4ToU32(pal::badRed()) : ImGui::GetColorU32(ImGuiCol_FrameBg));
    ImGui::SameLine(0, 2 * r + 8);
    if (hov) {
        ImGui::BeginTooltip();
        ImGui::Text(tr("ADC headroom %.1f dB (target %.0f dB): the robust peak is that far below full scale"), m.headroomDb, tgt);
        ImGui::Text(tr("Peak %.1f dBFS, rms %.1f dBFS, crest %.1f dB, clipped %.3f %%"), m.peakDbfs, m.rmsDbfs, m.crestDb, m.clipFraction * 100);
        if (m.floorValid) ImGui::Text(tr("Noise floor %.1f dBFS per bin, %.1f dB over the ADC's own noise; channel SNR %.1f dB"), m.floorDbfs, m.adcMarginDb, m.chanSnrDb);
        if (m.overloads) ImGui::Text(tr("Overloads: %d"), m.overloads);
        ImGui::TextDisabled("%s", tr("Red lamp: the ADC clipped in the last 2 s"));
        ImGui::EndTooltip();
    }
}

void historyGraph(float w, float h) {
    AgUi& s = S();
    const auto& hs = s.c.history();
    if (hs.size() < 2) { ImGui::TextDisabled("%s", tr("(no history yet)")); return; }
    std::vector<float> g, m;
    const size_t n = std::min<size_t>(hs.size(), 600);
    for (size_t i = hs.size() - n; i < hs.size(); i++) { g.push_back((float)hs[i].gainDb); m.push_back(hs[i].metricValid ? (float)hs[i].metric : NAN); }
    float lo = 1e9f, hi = -1e9f;
    for (float v : g) { lo = std::min(lo, v); hi = std::max(hi, v); }
    char ov[64];
    snprintf(ov, sizeof ov, "gain %.1f dB", g.back());
    ImGui::PlotLines("##aggain", g.data(), (int)g.size(), 0, ov, lo - 3, hi + 3, ImVec2(w, h));
    float ml = 1e9f, mh = -1e9f;
    bool any = false;
    for (float v : m) if (std::isfinite(v)) { ml = std::min(ml, v); mh = std::max(mh, v); any = true; }
    if (any) {
        for (auto& v : m) if (!std::isfinite(v)) v = ml;
        snprintf(ov, sizeof ov, "%s %.1f", ag::metricName(s.c.usedMetric() == ag::Metric::Auto ? s.c.target().metric : s.c.usedMetric()), m.back());
        ImGui::PlotLines("##agmetric", m.data(), (int)m.size(), 0, ov, ml - 2, mh + 2, ImVec2(w, h));
    }
}

void advancedPopup(App& a) {
    AgUi& s = S();
    if (!ImGui::BeginPopup("##agadv")) return;
    bool changed = false;
    const ag::RadioProfile& r = s.prof;
    ImGui::TextColored(pal::heading(), "%s", tr("Auto gain"));
    ImGui::SameLine();
    ImGui::TextDisabled("%s, %s", r.name.c_str(), ag::modeKindName(s.c.target().kind));
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", s.c.target().why);
    ImGui::TextDisabled(tr("Now: %s, %s, %.1f dB"), ag::agModeName(s.run), ag::phaseName(s.c.phase()), s.c.current().totalDb);
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    // targets and limits
    bool own = s.set.headroomDb >= 0;
    if (ImGui::Checkbox(tr("My own headroom target"), &own)) { s.set.headroomDb = own ? s.c.target().headroomDb : -1; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Off: the mode's own (%.0f dB). How far the signal's peaks stay below the ADC's full scale."), s.c.target().headroomDb);
    if (own) { ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi); float v = (float)s.set.headroomDb; if (ImGui::SliderFloat("##aghr", &v, 1, 30, "%.0f dB")) { s.set.headroomDb = v; changed = true; } }
    {
        const float lo = (float)r.minTotal(false), hi = (float)r.maxTotal(a.tune.ampOn);
        float v = (float)std::min(s.set.maxGainDb, (double)hi);
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::SliderFloat(tr("Max gain"), &v, lo, hi, "%.0f dB")) { s.set.maxGainDb = v >= hi - 0.01f ? 1e9 : v; changed = true; }
        float st = (float)s.set.stepDb;
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::SliderFloat(tr("Step size"), &st, 0, 12, st <= 0 ? "auto" : "%.0f dB")) { s.set.stepDb = st < 0.5f ? 0 : std::round(st); changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Optimise once: the spacing of the sweep (auto: about 10 steps over the range). Safe start: the ramp's step (auto: 3 dB)."));
        float sp = (float)s.set.loopSpeed;
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::SliderFloat(tr("Loop speed"), &sp, 0.25f, 4.f, "%.2fx", ImGuiSliderFlags_Logarithmic)) { s.set.loopSpeed = sp; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How quickly Auto reacts (hold and settle times are divided by it). Overloads are always acted on at once."));
        float se = (float)s.set.settleSec, me = (float)s.set.measureSec;
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::SliderFloat(tr("Settle time"), &se, 0.2f, 4.f, "%.1f s")) { s.set.settleSec = se; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Wait after each change before measuring, on top of the radio's own %.2f s (a decoder needs time to re-lock)."), r.settleSec);
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::SliderFloat(tr("Measure time"), &me, 0.3f, 6.f, "%.1f s")) { s.set.measureSec = me; changed = true; }
        bool ownRise = s.set.safeRiseDb >= 0;
        if (ImGui::Checkbox(tr("My own Safe start rise"), &ownRise)) { s.set.safeRiseDb = ownRise ? s.c.target().floorRiseDb : -1; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Safe start ramps up from the lowest gain until the noise floor has risen this much (the mode's: %.0f dB): the antenna's noise is then above the radio's."), s.c.target().floorRiseDb);
        if (ownRise) { ImGui::SameLine(); ImGui::SetNextItemWidth(140 * gUi); float v = (float)s.set.safeRiseDb; if (ImGui::SliderFloat("##agrise", &v, 1, 15, "%.0f dB")) { s.set.safeRiseDb = v; changed = true; } }
    }
    {
        static const char* kMetrics[] = {"Auto (the mode's)", "MER", "SNR", "BER", "Lock", "Channel SNR", "Sensitivity", "Headroom"};
        int m = (int)s.set.metric;
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::Combo(tr("Quality metric"), &m, kMetrics, 8)) { s.set.metric = (ag::Metric)m; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("What Optimise once maximises. The mode's: %s. A decoder number (MER / SNR / BER / lock) is used only while it locks; else the channel SNR from the spectrum."), ag::metricName(s.c.target().metric));
    }
    if (ImGui::Checkbox(tr("Auto also checks for intermodulation (a short +3 dB step now and then)"), &s.set.imdProbeAuto)) changed = true;
    ImGui::Separator();
    // stages
    ImGui::TextDisabled("%s", tr("Stages it may adjust"));
    uint32_t mask = s.set.stageMask;
    for (size_t i = 0; i < r.stages.size(); i++) {
        const ag::GainStage& st = r.stages[i];
        char lbl[96];
        const double now = i < s.c.current().db.size() ? s.c.current().db[i] : 0;
        if (st.userOnly) {
            if (agcAmpSwitchable(a)) {
                bool allow = agcAmpAllowed(a);
                if (ImGui::Checkbox(tr("Let auto gain switch the amp (default off)"), &allow)) {
                    a.agcAmpAllow = allow;
                    plat::prefs().setI(("agcamp." + radioKeyOf(a.devices[(size_t)a.devIdx])).c_str(), allow ? 1 : 0);
                    plat::prefs().flush();
                    a.agc.setAmpAllowed(allow);
                    changed = true;
                }
                if (allow) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "(!) %s", tr("can be damaged by strong signals")); }
            } else ImGui::TextDisabled(tr("%s: only you switch it"), st.label.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", (st.note + (r.ampNote.empty() ? "" : "\n" + r.ampNote)).c_str());
            continue;
        }
        if (!st.settable) {
            ImGui::BulletText(tr("%s (set by the driver)"), st.label.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.note.c_str());
            continue;
        }
        snprintf(lbl, sizeof lbl, "%s  %.1f dB (%g..%g)##agst%zu", st.label.c_str(), now, st.minDb(), st.maxDb(), i);
        bool on = (mask & (1u << i)) != 0;
        if (ImGui::Checkbox(lbl, &on)) {
            mask = on ? (mask | (1u << i)) : (mask & ~(1u << i));
            if (mask == 0) mask = 1u << i;   // something must stay adjustable
            s.set.stageMask = mask;
            plat::prefs().setI(maskKey(a).c_str(), (long)mask);
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", st.note.c_str(), st.reduction ? "\n(the driver counts it as a reduction; shown as gain)" : "");
    }
    if (!r.notes.empty()) { ImGui::PushTextWrapPos(460 * gUi); ImGui::TextDisabled("%s", r.notes.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("History (gain, quality)"));
    historyGraph(440 * gUi, 46 * gUi);
    if (ImGui::TreeNode(tr("Changes"))) {
        ImGui::BeginChild("##aglog", ImVec2(440 * gUi, 140 * gUi), true);
        const auto& lg = s.c.log();
        const double t = ImGui::GetTime();
        for (size_t i = lg.size(); i-- > 0 && lg.size() - i <= 60;) ImGui::Text("-%5.0fs  %5.1f > %5.1f dB  %s", t - lg[i].t, lg[i].fromDb, lg[i].toDb, lg[i].why.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }
    if (ImGui::TreeNode(tr("Remembered gains"))) {
        const std::string rk = radioKeyOf(a.devices[(size_t)a.devIdx]) + "|";
        std::string forget;
        for (const auto& kv : s.mem.entries()) {
            if (kv.first.compare(0, rk.size(), rk) != 0) continue;
            ImGui::Text(tr("mode %s, from %s MHz: %.1f dB (%s %.1f)"), kv.first.substr(rk.size(), kv.first.find('|', rk.size()) - rk.size()).c_str(),
                        kv.first.substr(kv.first.rfind('|') + 1).c_str(), kv.second.plan.totalDb, kv.second.metricName.c_str(), kv.second.metric);
            ImGui::SameLine();
            ImGui::PushID(kv.first.c_str());
            if (ImGui::SmallButton(tr("Forget"))) forget = kv.first;
            ImGui::PopID();
        }
        if (!forget.empty()) { s.mem.forget(forget); changed = true; }
        if (const ag::GainMemoryEntry* e = s.mem.find(memKey(a))) {
            char b[96];
            snprintf(b, sizeof b, "%s (%.1f dB)", tr("Use the remembered gain"), e->plan.totalDb);
            if (ImGui::Button(b)) {
                ag::AgCommand cmd; cmd.plan = e->plan;
                if (cmd.plan.db.size() != s.prof.stages.size()) cmd.plan = ag::planStages(s.prof, e->plan.totalDb, s.c.target().policy, s.set.stageMask, {}, a.tune.ampOn);
                cmd.plan.ampOn = a.tune.ampOn;
                a.agcOn = false;
                applyCmd(a, cmd);
                if (a.engine.running()) { a.tune.centerHz = settingsRetuneCentreHz(a); a.engine.retune(a.tune); }
                savePrefs(a);
            }
        }
        ImGui::TreePop();
    }
    if (changed) { saveSettings(); ag::AgSettings x = s.set; x.mode = s.run; s.c.setSettings(x); }
    ImGui::EndPopup();
}

void gear() {
    const float sz = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##aggear", ImVec2(sz, sz))) ImGui::OpenPopup("##agadv");
    const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(hov ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    const float r = sz * 0.22f, t = std::max(1.5f, sz * 0.09f);
    for (int i = 0; i < 8; i++) {
        const float an = (float)i * 0.7853982f;
        dl->AddLine(ImVec2(c.x + std::cos(an) * r, c.y + std::sin(an) * r), ImVec2(c.x + std::cos(an) * r * 1.62f, c.y + std::sin(an) * r * 1.62f), col, t * 1.3f);
    }
    dl->AddCircle(c, r, col, 16, t);
    if (hov) ImGui::SetTooltip("%s", tr("Auto gain settings: targets, limits, stages, history, remembered gains"));
    ImGui::SetNextWindowPos(ImVec2(p.x, p.y + sz + 2 * gUi), ImGuiCond_Appearing);
}

} // namespace

// ------------------------------------------------------------------------------------------------------------------------------ the API
bool autogainStep(App& a) {
    AgUi& s = S();
    if (!radioOk(a) || !a.engine.running()) {
        if (s.oneShot != ag::AgMode::Off) { s.oneShot = ag::AgMode::Off; s.run = ag::AgMode::Off; s.c.stop(); }
        return false;
    }
    // what should run: Auto follows a.agcOn (turned off by a manual gain change); a one-shot until it ends
    ag::AgMode want = s.oneShot != ag::AgMode::Off ? s.oneShot : a.agcOn ? ag::AgMode::Auto : ag::AgMode::Off;
    if (a.dir.state() == DirectionFinder::State::Measuring || a.sweep.active() || a.gopt.opt.active()) want = ag::AgMode::Off;   // the gain must stay put
    syncContext(a);
    if (want != s.run) {
        if (want == ag::AgMode::Off) { s.run = ag::AgMode::Off; s.c.stop(); }
        else startMode(a, want);
    }
    if (std::fabs(a.freqMhz - s.ctxFreq) > 1e-9) {   // a retune: Auto acquires afresh; a one-shot is cancelled (it measured another channel)
        s.ctxFreq = a.freqMhz;
        if (s.run == ag::AgMode::Auto) { const ag::StagePlan p = ag::planFromTune(s.prof, a.tune); s.c.start(ImGui::GetTime(), p.totalDb, p.ampOn, &p); }
        else if (s.oneShot != ag::AgMode::Off) { startMode(a, ag::AgMode::Off); a.agcOn = s.undoAgc; s.result = "stopped: the frequency changed"; s.resultAt = ImGui::GetTime(); }
    }
    ag::AgCommand cmd;
    const bool ch = s.c.tick(input(a), cmd);
    if (ch) applyCmd(a, cmd);
    // a one-shot that finished: the result, undo, memory
    if (s.oneShot != ag::AgMode::Off && !s.c.active()) {
        s.result = s.c.resultText();
        s.resultAt = ImGui::GetTime();
        s.undoable = s.c.phase() == ag::Phase::Done && !(s.c.current() == s.c.startPlan());
        if (s.c.phase() == ag::Phase::Done && s.c.usedMetric() != ag::Metric::Auto) {
            ag::GainMemoryEntry e;
            e.plan = s.c.current(); e.metric = s.c.bestMetric(); e.metricName = ag::metricName(s.c.usedMetric()); e.when = (int64_t)time(nullptr);
            s.mem.put(memKey(a), e);
            saveSettings();
        }
        a.engine.log("auto gain: " + std::string(ag::agModeName(s.oneShot)) + ": " + s.result);
        s.oneShot = ag::AgMode::Off;
        s.run = ag::AgMode::Off;
        a.agcOn = s.undoAgc;   // Auto goes on again if it was on (it starts from the result)
    }
    if (ch && s.run == ag::AgMode::Auto) a.engine.log("auto gain: " + cmd.why);
    return ch;
}

void autogainMeterOnly(App& a) {   // Off: the meters still run
    AgUi& s = S();
    if (!radioOk(a) || !a.engine.running() || s.run != ag::AgMode::Off) return;
    syncContext(a);
    ag::AgCommand cmd;
    s.c.tick(input(a), cmd);
}

void autogainControl(App& a, bool vertical, bool running) {
    AgUi& s = S();
    loadSettings();
    int cur = s.oneShot == ag::AgMode::Once ? 2 : s.oneShot == ag::AgMode::Safe ? 3 : a.agcOn ? 1 : 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Auto gain"));
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth((vertical ? 150 : 120) * gUi);
    ImGui::BeginDisabled(!radioOk(a));
    if (ImGui::BeginCombo("##agmode", tr(kModeNames[cur]))) {
        for (int i = 0; i < 4; i++) {
            const bool needRun = i >= 2;
            ImGui::BeginDisabled(needRun && !running);
            if (ImGui::Selectable(tr(kModeNames[i]), i == cur)) {
                if (i == 0) { if (s.oneShot != ag::AgMode::Off) startMode(a, ag::AgMode::Off); a.agcOn = false; agcReset(a); }
                else if (i == 1) { if (s.oneShot != ag::AgMode::Off) startMode(a, ag::AgMode::Off); a.agcOn = true; agcReset(a); }
                else startMode(a, i == 2 ? ag::AgMode::Once : ag::AgMode::Safe);
                savePrefs(a);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                static const char* tips[] = {
                    "The gain stays where you set it. The meters keep running.",
                    "A slow continuous loop: keeps the ADC headroom the mode wants, cuts at once on an overload, raises the gain only while it buys sensitivity, "
                    "holds still while a decoder is locked, undoes a step that made the decoder worse. Touching a gain control turns it off.",
                    "Tries the gain range (and the stage splits) and keeps the gain with the best quality for this mode (MER / SNR while it locks, else the "
                    "SNR in the channel) that does not overload or create intermodulation. About 15-40 s; Undo puts the old gain back.",
                    "Starts at the lowest gain and ramps up until the noise floor rises (the antenna's noise is then above the radio's), stopping before "
                    "the ADC clips or intermodulation appears. The safest first setting for an unknown antenna."};
                ImGui::SetTooltip("%s", tr(tips[i]));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 4 * gUi);
    gear();
    if (radioOk(a)) { syncContext(a); advancedPopup(a); }
    if (s.oneShot != ag::AgMode::Off && s.c.active()) {
        ImGui::SameLine(0, 6 * gUi);
        char ov[48];
        snprintf(ov, sizeof ov, "%.0f dB", s.c.current().totalDb);
        ImGui::ProgressBar((float)s.c.progress(), ImVec2(80 * gUi, 0), ov);
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button(tr("Stop"))) { startMode(a, ag::AgMode::Off); a.agcOn = s.undoAgc; s.result = "stopped: the gain was put back"; s.resultAt = ImGui::GetTime(); }
    } else if (s.undoable) {
        sameLineIf(ImGui::CalcTextSize(tr("Undo gain")).x + 2.f * ImGui::GetStyle().FramePadding.x, 6 * gUi);
        if (ImGui::Button(tr("Undo gain"))) {
            a.tune.lnaDb = s.undoTune.lnaDb; a.tune.vgaDb = s.undoTune.vgaDb; a.tune.gainDb = s.undoTune.gainDb; a.tune.radio = s.undoTune.radio;
            a.agcOn = s.undoAgc; agcReset(a);
            if (a.engine.running()) { a.tune.centerHz = settingsRetuneCentreHz(a); a.engine.retune(a.tune); }
            s.undoable = false;
            savePrefs(a);
            a.engine.log("auto gain: undone");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Back to the gain from before the last Optimise once / Safe start"));
    }
}

void autogainMeters(App& a) {
    AgUi& s = S();
    if (!radioOk(a) || !a.engine.running()) return;
    const ag::AgMeters& m = s.c.meters();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("ADC"));
    ImGui::SameLine(0, 4 * gUi);
    drawHeadroomBar(110 * gUi);
    if (m.valid) {
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextDisabled(tr("%.0f dB headroom"), m.headroomDb);
        if (m.floorValid) {
            ImGui::SameLine(0, 8 * gUi);
            ImGui::TextDisabled(tr("floor %+.0f dB over ADC"), m.adcMarginDb);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("The noise floor is %.1f dB above the ADC's own noise (%.1f dBFS per bin). Below about %.0f dB more gain buys sensitivity; far above it, gain only costs headroom."),
                                                          m.adcMarginDb, m.floorDbfs, s.c.target().marginGoalDb);
        }
    }
    const auto adv = s.c.advice();
    if (!adv.empty()) {
        ImGui::SameLine(0, 8 * gUi);
        const bool bad = adv.front().kind == ag::Advice::Overload || adv.front().kind == ag::Advice::Intermod || adv.front().kind == ag::Advice::Compressed;
        ImGui::TextColored(bad ? pal::badRed() : pal::warnAmber(), "(!) %s", tr(adv.front().text.c_str()));
        if (ImGui::IsItemHovered() && adv.size() > 1) {
            ImGui::BeginTooltip();
            for (const auto& x : adv) ImGui::BulletText("%s", tr(x.text.c_str()));
            ImGui::EndTooltip();
        }
    }
}

bool autogainStatus(App& a) {
    AgUi& s = S();
    if (s.oneShot != ag::AgMode::Off && s.c.active()) {
        ImGui::TextColored(pal::heading(), tr("Auto gain: %s %.0f %% (%.1f dB) ..."), tr(s.oneShot == ag::AgMode::Once ? "optimising" : "safe start"), s.c.progress() * 100, s.c.current().totalDb);
        return true;
    }
    if (s.result.empty() || ImGui::GetTime() - s.resultAt > 25) return false;
    ImGui::TextColored(pal::okGreen(), tr("Auto gain: %s"), s.result.c_str());
    (void)a;
    return true;
}

bool autogainRun(App& a, int how) {
    if (!radioOk(a) || !a.engine.running()) return false;
    startMode(a, how == 3 ? ag::AgMode::Safe : how == 1 ? ag::AgMode::Auto : ag::AgMode::Once);
    if (how == 1) a.agcOn = true;
    return true;
}
bool autogainBusy(const App&) { return S().oneShot != ag::AgMode::Off && S().c.active(); }
std::string autogainResult(const App&) { return S().result; }
std::string autogainStatusJson(App& a) {
    (void)a;
    return ag::autogainJson(S().c, ImGui::GetTime());
}
