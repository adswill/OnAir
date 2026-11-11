// Screens of the ham science modes (families 460 - 465, 500 - 502, 750 JT6M and the 3.9 event monitors 820 - 835, one receiver class, docs/HAM-SCIENCE-3.8.md and docs/W1B-3.9.md): QRSS and DFCW (a spectrogram of the
// keyed tone and the text), FSK441, ISCAT and PI4 (a list of decodes), Throb (running text), SuperSID (VLF station levels and the sudden changes of a solar
// flare), the riometer (absorption against a sidereal quiet-day curve) and pulsar folding (profile, S/N, and what antenna it takes).
// Auto is on for every mode; the manual settings sit under the decoder options and turn Auto off when changed.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/common/time_fmt.h"
#include "dect2/hamsci/hamsci_gen.h"
#include "dect2/hamsci/hamsci_rx.h"
#include "platform/platform.h"
#include "ui/kit/kit.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/mode_panel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace dect2;

namespace {

struct State {
    bool loaded = false, wasRunning = false;
    bool autoOn = true;
    float centreHz = 1000.f, dotSec = 3.f, sidRise = 1.5f, rioLon = 20.f, rioThr = 0.3f;
    int variant = 0;                 // Throb: 0 = automatic, then 1 .. 6; ISCAT: 0 = automatic, 1 = A, 2 = B
    bool dfcwRev = false;
    int sidSel = 0;
    int psrSel = 0;
    float psrFlux = 1.5f, psrTsys = 150.f, psrHours = 1.f, psrBwMHz = 2.4f, psrDuty = 3.f;
    float evtThr = 0.f;              // event monitors: detection threshold, 0 = automatic
    std::string saved;               // last CSV saved (path or error)
    bool pushed = false;
    uint64_t qdcSeen = 0;
    std::string qdcSaved;
};
State gS[kHamsciKinds];

bool live(const App& a, int K) { return a.engine.running() && a.rx.standard == hamsciFamily(K) + 1; }
void notRunning(const App& a) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); }

void load(int K) {
    State& s = gS[K];
    if (s.loaded) return;
    s.loaded = true;
    const std::string id = hamsciId(K);
    s.autoOn = plat::prefs().getB(("hsAuto_" + id).c_str(), true);
    s.centreHz = (float)plat::prefs().getD(("hsCentre_" + id).c_str(), 1000.0);
    s.dotSec = (float)plat::prefs().getD(("hsDot_" + id).c_str(), 3.0);
    s.variant = (int)plat::prefs().getI(("hsVariant_" + id).c_str(), 0);
    s.dfcwRev = plat::prefs().getB(("hsRev_" + id).c_str(), false);
    s.sidRise = (float)plat::prefs().getD("hsSidRise", 1.5);
    s.rioLon = (float)plat::prefs().getD("hsRioLon", 20.0);
    s.rioThr = (float)plat::prefs().getD("hsRioThr", 0.3);
    s.psrFlux = (float)plat::prefs().getD("hsPsrFlux", 1.5);
    s.psrTsys = (float)plat::prefs().getD("hsPsrTsys", 150.0);
    s.psrHours = (float)plat::prefs().getD("hsPsrHours", 1.0);
    s.qdcSaved = plat::prefs().getS("hsRioQdc", "");
    s.evtThr = (float)plat::prefs().getD(("hsEvtThr_" + id).c_str(), 0.0);
}

void save(const char* key, double v) { plat::prefs().setD(key, v); plat::prefs().flush(); }

std::string ago(double s) {
    char b[48];
    if (s < 0) return "-";
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

void kv(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(150 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void intro(int K) {
    static const char* txt[kHamsciKinds] = {
        "QRSS: Morse so slow (dots of 3 to 120 s) that a narrow spectrogram shows it. Tune the audio tone into the window or leave the centre on Auto; the dot length is found from the keying. Receive only.",
        "DFCW: slow Morse where all elements are the same length: dots on one frequency, dashes a little higher, a gap between letters. The spectrogram shows both. Receive only.",
        "FSK441: meteor scatter pings, 441 baud 4-FSK. The receiver finds the bursts above the noise and reads the call signs in them. Receive only.",
        "ISCAT: weak-signal 42-tone scatter mode (ISCAT-A and B), found and decoded automatically. Receive only.",
        "PI4: 4-FSK beacon mode of the microwave bands, 27 s frames with an 8 character message. Receive only.",
        "Throb and ThrobX: one or two tones at a time, one symbol per second or faster. Variant and tone comb are found automatically. Receive only.",
        "SuperSID: the strength of VLF navy transmitters (15 to 30 kHz) follows the ionosphere. A solar flare shows as a sudden step in a signal. It needs a VLF-capable radio and a loop or whip antenna, and the step must be checked against the GOES X-ray flux before you call it a flare.",
        "Riometer: the cosmic radio noise (about 30 MHz) shrinks when the ionosphere absorbs it during a geomagnetic or solar particle event. The receiver measures a rejected-interference noise power and compares it with a quiet-day curve per sidereal hour. The curve needs one to two days and a receiver gain that never changes.",
        "Pulsar folding: weak periodic pulses are found by dedispersing and folding the record at the pulsar's period. This is a radio astronomy job: it needs a dish or array, a low-noise amplifier and an hour or more; the antenna line below says how big.",
        "JT6M: 6 m meteor and ionospheric scatter, 44-tone FSK at 21.5 baud with a sync tone every third symbol. The receiver lists the pings and folds a long signal into its repeating message. Receive only.",
        "Schumann resonances: the Earth-ionosphere cavity rings near 7.83, 14.1, 20.3, 26.3 and 32.5 Hz, driven by lightning around the world. Needs an ELF input (a magnetic coil and a sound card or ADC): a radio cannot hear 8 Hz. The peaks need a few minutes of averaging. Receive only. Tested on simulated signals only.",
        "Whistlers and tweeks: lightning-launched tones in a VLF audio recording. A whistler is a tone that falls over a second or two (t = t0 + D / sqrt(f)); a tweek is a short ridge near 1.8 kHz after a sferic. Needs a VLF antenna and receiver into a sound card. Receive only. Tested on simulated signals only.",
        "Moon echo test: send a keyed carrier of a second or more (your own transmitter, not controlled here; pulses 10 s apart or more) and the monitor looks 2.3 to 2.85 s later for the echo. Receive only. Tested on simulated signals only.",
        "Radio Jove: Jupiter, the Sun and the galactic background near 20.1 MHz in a spectrogram; broadband bursts are listed and narrowband interference is ignored. Needs a radio that tunes 20 MHz and a suitable antenna. Receive only. Tested on simulated signals only.",
        "e-CALLISTO style solar bursts: a spectrogram of one capture and the bursts in it with their drift rate. One capture covers a few MHz; the CALLISTO spectrometer sweeps the whole band and this mode does not. Receive only. Tested on simulated signals only.",
        "Beacon events: a VHF beacon carrier is followed and each opening is logged with time, S/N, Doppler width and flutter. The class (aurora-like, TEP-like, sporadic-E-like) comes from the signal's character only and does not prove the path. Receive only. Tested on simulated signals only."};
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(txt[K]));
    ImGui::PopTextWrapPos();
}

// ---- digital modes: the list of decodes
void decodeList(App& a, const HamsciTelemetry& t) {
    if (!ImGui::BeginTable("##hslist", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("dB")); ImGui::TableSetupColumn(tr("Hz")); ImGui::TableSetupColumn(tr("Message"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const HamsciLine& l : t.lines) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(l.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.0f", l.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", l.freqHz);
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", l.text.c_str()); if (!l.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", l.note.c_str()); }
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (t.lines.empty()) ImGui::TextDisabled("%s", tr("nothing decoded yet"));
}

void textBox(App& a, const HamsciTelemetry& t, float h) {
    ImGui::BeginChild("##hstext", ImVec2(0, h), true);
    ImGui::PushFont(a.mono, 0);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t.text.empty() ? tr("no text yet") : t.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

// ---- the spectrogram of the grabbers
void spectrogram(App& a, const HamsciTelemetry& t, float h) {
    const auto& rows = t.qrss.rows;
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(10, 12, 18, 255));
    if (!rows.empty() && !rows[0].empty()) {
        const size_t R = rows.size(), B = rows[0].size();
        const float cw = w / (float)R, ch = h / (float)B;
        for (size_t r = 0; r < R; r++)
            for (size_t b = 0; b < B; b++) {
                const float v = std::clamp((rows[r][b] - 2.f) / 22.f, 0.f, 1.f);
                if (v < 0.05f) continue;
                const int g = (int)(40 + 215 * v);
                dl->AddRectFilled(ImVec2(p.x + (float)r * cw, p.y + h - ((float)b + 1) * ch), ImVec2(p.x + (float)(r + 1) * cw + 0.5f, p.y + h - (float)b * ch), IM_COL32(g, (int)(g * 0.85f), (int)(60 * v + 30), 255));
            }
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::TextDisabled("%.1f s per column, %.2f Hz per row, window %.0f Hz wide, centre %.1f Hz", t.qrss.rowSec, t.qrss.binHz, t.qrss.spanHz, t.qrss.centreHz);
    (void)a;
}

// ---- SuperSID
void sidTab(App& a, const HamsciTelemetry& t, State& s) {
    const auto& st = t.sid;
    if (ImGui::BeginTable("##sidt", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn("kHz"); ImGui::TableSetupColumn(tr("Level dB")); ImGui::TableSetupColumn(tr("Change dB"));
        ImGui::TableSetupColumn(tr("Heard")); ImGui::TableSetupColumn(tr("Sudden change")); ImGui::TableSetupColumn(tr("Site"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int n = 0;
        const hamsci::SidStation* tab = hamsci::sidStations(n);
        ImGui::PushFont(a.mono, 0);
        for (size_t i = 0; i < st.stations.size(); i++) {
            const auto& x = st.stations[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable((x.call + "##sid" + std::to_string(i)).c_str(), s.sidSel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) s.sidSel = (int)i;
            ImGui::TableNextColumn(); ImGui::Text("%.2f", x.kHz);
            ImGui::TableNextColumn(); if (x.inSpan && x.levelDb > -150) ImGui::Text("%.1f", (double)x.levelDb); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (x.inSpan && x.baselineDb > -150) ImGui::Text("%+.1f", (double)x.riseDb); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(!x.inSpan ? tr("out of span") : x.present ? tr("yes") : tr("no"));
            ImGui::TableNextColumn(); if (x.event) ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1), "%+.1f dB", x.eventPeakDb); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", i < (size_t)n ? tab[i].where : "");
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (!st.stations.empty()) {
        const auto& x = st.stations[(size_t)std::clamp(s.sidSel, 0, (int)st.stations.size() - 1)];
        ImGui::TextDisabled("%s %s", x.call.c_str(), tr("level, one value per minute (right = now)"));
        if (x.history.size() > 1) {
            float lo = 1e9f, hi = -1e9f;
            for (float v : x.history) { lo = std::min(lo, v); hi = std::max(hi, v); }
            ImGui::PlotLines("##sidh", x.history.data(), (int)x.history.size(), 0, nullptr, lo - 1, hi + 1, ImVec2(ImGui::GetContentRegionAvail().x, std::max(80.f, 110 * gUi)));
        } else ImGui::TextDisabled("%s", tr("the history starts after the first minute"));
    }
    ImGui::Spacing();
    kv(a, tr("sudden changes"), "%d", st.eventsTotal);
    if (st.eventsTotal) kv(a, tr("last"), "%s %+.1f dB, %s ago", st.lastEventCall.c_str(), st.lastEventRiseDb, ago(st.lastEventAgoSec).c_str());
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("A step of more than the threshold within ten minutes on a station that is clearly present is marked. Sunrise and sunset along the path also change a signal (slowly); check a mark against the GOES X-ray flux before calling it a flare."));
    ImGui::PopTextWrapPos();
}

// ---- riometer
void rioTab(App& a, const HamsciTelemetry& t) {
    const auto& r = t.rio;
    kv(a, tr("noise power"), "%.2f dB (re full scale)", (double)r.powerDb);
    kv(a, tr("sidereal time"), "%.2f h", r.lstHours);
    if (r.haveQdc) kv(a, tr("absorption"), "%.2f dB%s", (double)r.absorptionDb, r.event ? "  (event)" : "");
    else kv(a, tr("quiet-day curve"), "%d of 288 bins, not usable yet", r.qdcFill);
    kv(a, tr("interference rejected"), "%.0f %%", r.interferedFrac * 100.0);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Absorption, one value per minute (right = now)"));
    if (r.absHistory.size() > 1) ImGui::PlotLines("##rioa", r.absHistory.data(), (int)r.absHistory.size(), 0, nullptr, -0.5f, 2.f, ImVec2(ImGui::GetContentRegionAvail().x, std::max(80.f, 110 * gUi)));
    ImGui::TextDisabled("%s", tr("Noise power, one value per minute"));
    if (r.powerHistory.size() > 1) ImGui::PlotLines("##riop", r.powerHistory.data(), (int)r.powerHistory.size(), 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, std::max(80.f, 110 * gUi)));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Needs a receiver whose gain does not change (turn off automatic gain), an antenna that looks straight up, and a clean frequency near 30 MHz. A gain change looks like absorption."));
    ImGui::PopTextWrapPos();
}

// ---- pulsar
void pulsarTab(App& a, const HamsciTelemetry& t, State& s) {
    const auto& p = t.pulsar;
    kv(a, tr("observed"), "%.0f s%s", t.pulsarObservedSec, t.pulsarSearching ? "  (searching)" : "");
    kv(a, tr("best S/N"), "%.1f  (%s)", p.snr, p.ok ? tr("detected") : tr("not detected: needs 7"));
    if (p.snr > 0) kv(a, tr("period, DM"), "%.6f s, %.2f pc/cm3", p.periodS, p.dm);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Folded profile (one pulse period)"));
    if (!p.profile.empty()) ImGui::PlotLines("##psrprof", p.profile.data(), (int)p.profile.size(), 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, std::max(90.f, 120 * gUi)));
    if (!p.dmCurve.empty()) { ImGui::TextDisabled("%s", tr("S/N against dispersion measure")); ImGui::PlotLines("##psrdm", p.dmCurve.data(), (int)p.dmCurve.size(), 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, 60 * gUi)); }
    if (!p.periodCurve.empty()) { ImGui::TextDisabled("%s", tr("S/N against period")); ImGui::PlotLines("##psrper", p.periodCurve.data(), (int)p.periodCurve.size(), 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, 60 * gUi)); }
    ImGui::Spacing();
    // the antenna statement and the radiometer equation
    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.4f, 1), "%s", tr("What antenna this needs"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("A pulsar is thousands of times weaker than the noise in a single sample. It shows only after minutes to hours of folding, and only if the antenna collects enough: roughly one square metre of effective area with a low-noise amplifier at the feed (a dish of 1.5 to 2 m, or an array of several long Yagis) for the brightest northern pulsar B0329+54 at 408 MHz. A whip, a discone or a wideband dipole (0.1 m2) will not show any pulsar. The numbers come from the radiometer equation below and from simulated data; this mode has not been tested on a real pulsar."));
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(90 * gUi); ImGui::InputFloat(tr("flux (Jy)"), &s.psrFlux, 0.f, 0.f, "%.2f");
    ImGui::SetNextItemWidth(90 * gUi); ImGui::InputFloat(tr("system temperature (K)"), &s.psrTsys, 0.f, 0.f, "%.0f");
    ImGui::SetNextItemWidth(90 * gUi); ImGui::InputFloat(tr("hours"), &s.psrHours, 0.f, 0.f, "%.1f");
    s.psrFlux = std::max(0.01f, s.psrFlux); s.psrTsys = std::max(10.f, s.psrTsys); s.psrHours = std::max(0.05f, s.psrHours);
    const double bw = t.inputRate > 0 ? std::min(t.inputRate, 4e6) : 2.4e6;
    const double area = hamsci::pulsarRequiredAreaM2(s.psrFlux, bw, s.psrTsys, s.psrHours * 3600.0, 0.03, 5.0, 2);
    kv(a, tr("effective area for S/N 5"), "%.1f m2 (3 %% pulse width, %.1f MHz, both polarisations)", area, bw / 1e6);
}

// ---- the event monitors (kinds 10 .. 15): spectrogram, trend, events, exports
std::string evtFreqLabel(int K, const HamsciTelemetry& t, int idx) {
    const auto& e = t.evt;
    char b[48];
    switch (K) {
    case kHsSchumann: snprintf(b, sizeof b, "%.1f Hz", e.f0Hz + idx * e.binHz * 4); break;
    case kHsWhistler: snprintf(b, sizeof b, "%.1f kHz", (e.f0Hz + idx * e.binHz) / 1e3); break;
    case kHsEme: snprintf(b, sizeof b, "%+.0f Hz", e.f0Hz + idx * e.binHz); break;
    case kHsJove: case kHsCallisto: snprintf(b, sizeof b, "%.3f MHz", (t.centerHz + e.f0Hz + idx * e.binHz) / 1e6); break;
    default: snprintf(b, sizeof b, "%+.0f Hz", e.f0Hz + idx * e.binHz); break;
    }
    return b;
}

void evtSpectrogram(App&, const HamsciTelemetry& t, int K, float h) {
    const auto& rows = t.evt.rows;
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(10, 12, 18, 255));
    const float range = K == kHsJove || K == kHsCallisto ? 8.f : K == kHsSchumann ? 12.f : 24.f;
    int B = 0;
    if (!rows.empty() && !rows[0].empty()) {
        const size_t R = rows.size();
        B = (int)rows[0].size();
        const float cw = w / 150.f, ch = h / (float)B;
        for (size_t r = 0; r < R; r++)
            for (int b = 0; b < B && (size_t)b < rows[r].size(); b++) {
                const float v = std::clamp(rows[r][(size_t)b] / range, 0.f, 1.f);
                if (v < 0.05f) continue;
                const int g = (int)(40 + 215 * v);
                const float x0 = p.x + w - (float)(R - r) * cw;
                dl->AddRectFilled(ImVec2(x0, p.y + h - ((float)b + 1) * ch), ImVec2(x0 + cw + 0.5f, p.y + h - (float)b * ch), IM_COL32(g, (int)(g * 0.85f), (int)(60 * v + 30), 255));
            }
    }
    ImGui::Dummy(ImVec2(w, h));
    if (B > 0) ImGui::TextDisabled("%s %s  -  %s %s,  %.2f s %s", tr("bottom"), evtFreqLabel(K, t, 0).c_str(), tr("top"), evtFreqLabel(K, t, B).c_str(), t.evt.rowSec, tr("per column, newest on the right"));
}

void evtPlot(const char* id, const std::vector<float>& v, const std::string& name) {
    if (v.size() < 2) return;
    ImGui::TextDisabled("%s", tr(name.c_str()));
    ImGui::PlotLines(id, v.data(), (int)v.size(), 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, std::max(70.f, 90 * gUi)));
}

void evtTab(App& a, const HamsciTelemetry& t, State& s, int K) {
    const auto& e = t.evt;
    evtSpectrogram(a, t, K, std::max(150.f, 200 * gUi));
    ImGui::Spacing();
    if (K == kHsSchumann) {
        static const double kNom[5] = {7.83, 14.1, 20.3, 26.3, 32.5};
        if (ImGui::BeginTable("##schpk", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn(tr("Mode")); ImGui::TableSetupColumn(tr("Published Hz")); ImGui::TableSetupColumn(tr("Found Hz")); ImGui::TableSetupColumn(tr("dB over baseline")); ImGui::TableSetupColumn("Q");
            ImGui::TableHeadersRow();
            ImGui::PushFont(a.mono, 0);
            for (int m = 0; m < 5; m++) {
                const bool ok = (size_t)m < e.peakHz.size() && e.peakHz[(size_t)m] > 0;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", m + 1);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", kNom[m]);
                ImGui::TableNextColumn(); if (ok) ImGui::Text("%.2f", (double)e.peakHz[(size_t)m]); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); if (ok) ImGui::Text("%.1f", (double)e.peakDb[(size_t)m]); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); if (ok) ImGui::Text("%.1f", (double)e.peakQ[(size_t)m]); else ImGui::TextDisabled("-");
            }
            ImGui::PopFont();
            ImGui::EndTable();
        }
        if (e.avgSpec.size() > 2) { ImGui::TextDisabled("%s", tr("Five minute average spectrum, dB over the baseline")); ImGui::PlotLines("##schavg", e.avgSpec.data(), (int)e.avgSpec.size(), 0, nullptr, -2.f, 14.f, ImVec2(ImGui::GetContentRegionAvail().x, std::max(80.f, 100 * gUi))); }
    }
    kv(a, tr("events"), "%d", e.eventsTotal);
    evtPlot("##evtt1", e.trend, e.trendName);
    evtPlot("##evtt2", e.trend2, e.trend2Name);
    if (e.hourly.size() == 24) {
        std::vector<float> h(24);
        for (size_t i = 0; i < 24; i++) h[i] = (float)e.hourly[i];
        ImGui::TextDisabled("%s", tr("Events per UTC hour of the day"));
        ImGui::PlotHistogram("##evth", h.data(), 24, 0, nullptr, 0.f, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, std::max(60.f, 70 * gUi)));
    }
    ImGui::Spacing();
    HamsciReceiver& r = a.engine.hamsci(K);
    if (ImGui::SmallButton((std::string(tr("Copy events (CSV)")) + "##evcp").c_str())) ImGui::SetClipboardText(r.evtExportCsv().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton((std::string(tr("Copy trend (CSV)")) + "##evtr").c_str())) ImGui::SetClipboardText(r.evtExportTrendCsv().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton((std::string(tr("Save CSV files")) + "##evsv").c_str())) {
        const std::string base = plat::dataDir() + "/" + hamsciId(K);
        std::ofstream f1(base + "_events.csv", std::ios::binary), f2(base + "_trend.csv", std::ios::binary);
        if (f1 && f2) { f1 << r.evtExportCsv(); f2 << r.evtExportTrendCsv(); s.saved = base + "_events.csv, " + base + "_trend.csv"; }
        else s.saved = tr("could not write to the data folder");
    }
    if (!s.saved.empty()) ImGui::TextDisabled("%s", s.saved.c_str());
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(trw("Event list"), ImGuiTreeNodeFlags_DefaultOpen)) decodeList(a, t);
}

// ---- the tab
void tab(App& a, int K) {
    load(K);
    const HamsciTelemetry& t = a.rx.hamsci;
    kit::modeHeader({(uint16_t)hamsciFamily(K), hamsciName(K), live(a, K) ? (t.state >= 2 ? 1 : 2) : a.engine.running() ? 2 : 0, live(a, K) && t.snrDb > 0 ? t.snrDb : -1000.f, a.freqMhz * 1e6});
    intro(K);
    ImGui::Spacing();
    if (!live(a, K)) { notRunning(a); return; }
    State& s = gS[K];
    if (!t.found.empty()) kv(a, tr("found"), "%s", t.found.c_str());
    switch (K) {
    case kHsQrss: case kHsDfcw: spectrogram(a, t, std::max(160.f, 200 * gUi)); textBox(a, t, std::max(70.f, 90 * gUi)); break;
    case kHsThrob: textBox(a, t, std::max(160.f, ImGui::GetContentRegionAvail().y)); break;
    case kHsSid: sidTab(a, t, s); break;
    case kHsRiometer: rioTab(a, t); break;
    case kHsPulsar: pulsarTab(a, t, s); break;
    case kHsJt6m: if (!t.text.empty()) kv(a, tr("averaged message"), "%s", t.text.c_str()); decodeList(a, t); break;
    case kHsSchumann: case kHsWhistler: case kHsEme: case kHsJove: case kHsCallisto: case kHsAurora: evtTab(a, t, s, K); break;
    default: decodeList(a, t); break;
    }
}

void receiver(App& a, int K) {
    if (!live(a, K)) { notRunning(a); return; }
    const HamsciTelemetry& t = a.rx.hamsci;
    kv(a, tr("state"), "%s", hamsciSummary(t).c_str());
    kv(a, tr("automatic settings"), "%s", t.autoParams ? tr("on") : tr("off (manual)"));
    kv(a, tr("level"), "%.1f dBFS", (double)t.levelDb);
    kv(a, tr("input"), "%.3f Msps", t.inputRate / 1e6);
    kv(a, tr("signal time"), "%.1f s", t.timeSec);
    if (hamsciAudioKind(K)) kv(a, tr("decoded"), "%llu", (unsigned long long)t.blocksOk);
    if (hamsciEvtKind(K)) { for (const auto& l : t.evt.info) kv(a, tr("status"), "%s", l.c_str()); kv(a, tr("events"), "%d", t.evt.eventsTotal); }
    smart5SidCard(a);   // round-5 smart checks (app/smart5, docs/SMART5-UI-3.9.md)
}

void list(App&, int) {}

void decoder(App& a, bool&, int K) {
    load(K);
    State& s = gS[K];
    HamsciReceiver& r = a.engine.hamsci(K);
    if (ImGui::Checkbox((std::string(tr("Auto")) + "##hsauto").c_str(), &s.autoOn)) {
        plat::prefs().setB(("hsAuto_" + std::string(hamsciId(K))).c_str(), s.autoOn); plat::prefs().flush();
        r.setAuto(s.autoOn);
        if (!s.autoOn) s.pushed = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Automatic settings: the receiver finds the variant, the tone, the element length or the search range itself. Changing a manual value turns Auto off for this mode."));
    ImGui::TextDisabled("%s", tr("ADVANCED"));
    auto manual = [&] { if (s.autoOn) { s.autoOn = false; plat::prefs().setB(("hsAuto_" + std::string(hamsciId(K))).c_str(), false); } };
    switch (K) {
    case kHsQrss: case kHsDfcw:
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputFloat(tr("Tone (Hz)"), &s.centreHz, 0.f, 0.f, "%.1f")) { manual(); save(("hsCentre_" + std::string(hamsciId(K))).c_str(), s.centreHz); r.setQrssCentreHz(s.centreHz); }
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputFloat(tr("Element length (s)"), &s.dotSec, 0.f, 0.f, "%.1f")) { manual(); save(("hsDot_" + std::string(hamsciId(K))).c_str(), s.dotSec); r.setQrssDotSec(s.dotSec); }
        if (K == kHsDfcw && ImGui::Checkbox(tr("Dash below the dot frequency"), &s.dfcwRev)) { plat::prefs().setB(("hsRev_" + std::string(hamsciId(K))).c_str(), s.dfcwRev); r.setDfcwReverse(s.dfcwRev); }
        break;
    case kHsIscat: {
        static const char* v[] = {"Automatic", "ISCAT-A", "ISCAT-B"};
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::BeginCombo("##hsiv", tr(v[s.variant]))) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(v[i]), s.variant == i)) { s.variant = i; plat::prefs().setI(("hsVariant_" + std::string(hamsciId(K))).c_str(), i); r.setVariant(i == 0 ? 2 : i - 1); if (i) manual(); else { s.autoOn = true; r.setAuto(true); } }
            ImGui::EndCombo();
        }
        break;
    }
    case kHsThrob: {
        static const char* v[] = {"Automatic", "Throb1", "Throb2", "Throb4", "ThrobX1", "ThrobX2", "ThrobX4"};
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::BeginCombo("##hstv", tr(v[s.variant]))) {
            for (int i = 0; i < 7; i++) if (ImGui::Selectable(tr(v[i]), s.variant == i)) { s.variant = i; plat::prefs().setI(("hsVariant_" + std::string(hamsciId(K))).c_str(), i); r.setVariant(i - 1); if (i) manual(); else { s.autoOn = true; r.setAuto(true); } }
            ImGui::EndCombo();
        }
        break;
    }
    case kHsSid:
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::SliderFloat(tr("Step threshold"), &s.sidRise, 0.5f, 6.f, "%.1f dB")) { save("hsSidRise", s.sidRise); r.setSidRiseDb(s.sidRise); }
        break;
    case kHsRiometer:
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputFloat(tr("Longitude (deg E)"), &s.rioLon, 0.f, 0.f, "%.2f")) { save("hsRioLon", s.rioLon); r.setRioLongitude(s.rioLon); }
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::SliderFloat(tr("Event threshold"), &s.rioThr, 0.1f, 3.f, "%.2f dB")) { save("hsRioThr", s.rioThr); r.setRioThresholdDb(s.rioThr); }
        break;
    case kHsPulsar: {
        int n = 0;
        const hamsci::PulsarTarget* tg = hamsci::pulsarTargets(n);
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::BeginCombo("##hspt", tg[std::clamp(s.psrSel, 0, n - 1)].name)) {
            for (int i = 0; i < n; i++) if (ImGui::Selectable(tg[i].name, s.psrSel == i)) { s.psrSel = i; r.setPulsarTarget(tg[i].periodS, tg[i].dm, tg[i].fluxJy); s.psrFlux = (float)tg[i].fluxJy; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Periods are from memory and drift with the Earth's motion; the search covers 0.02 % around the period. Check the ATNF pulsar catalogue before a long run."));
        if (ImGui::SmallButton(tr("Search now##psr"))) r.pulsarSearchNow();
        break;
    }
    case kHsSchumann: case kHsWhistler: case kHsEme: case kHsJove: case kHsCallisto: case kHsAurora:
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::SliderFloat(tr("Detection threshold (0 = automatic)"), &s.evtThr, 0.f, 20.f, "%.1f")) { save(("hsEvtThr_" + std::string(hamsciId(K))).c_str(), s.evtThr); r.setEvtThresholdDb(s.evtThr > 0.f ? s.evtThr : -1.0); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Schumann transients, Jove and CALLISTO bursts: dB over the background. Whistlers: path score. Moon echo and beacon openings: sigma over the noise. Lower finds weaker events and more false ones."));
        break;
    default: break;
    }
    if (ImGui::SmallButton((std::string(tr("Clear")) + "##hsclear").c_str())) r.clearResults();
}

void synth(App& a, bool& changed, int K) {
    SynthConfig& sc = a.tune.synth;
    if (K == kHsIscat) {
        static const char* v[] = {"ISCAT-A", "ISCAT-B"};
        ImGui::SetNextItemWidth(110 * gUi);
        int o = std::clamp(sc.modeOpt[0], 0, 1);
        if (ImGui::BeginCombo("##hssy", v[o])) { for (int i = 0; i < 2; i++) if (ImGui::Selectable(v[i], o == i)) { sc.modeOpt[0] = i; changed = true; } ImGui::EndCombo(); }
    } else if (K == kHsThrob) {
        static const char* v[] = {"Throb1", "Throb2", "Throb4", "ThrobX1", "ThrobX2", "ThrobX4"};
        ImGui::SetNextItemWidth(110 * gUi);
        int o = std::clamp(sc.modeOpt[0], 0, 5);
        if (ImGui::BeginCombo("##hssy6", v[o])) { for (int i = 0; i < 6; i++) if (ImGui::Selectable(v[i], o == i)) { sc.modeOpt[0] = i; changed = true; } ImGui::EndCombo(); }
    } else if (K == kHsSid) {
        bool fl = sc.modeOpt[0] == 1;
        if (ImGui::Checkbox(tr("Solar flare at 17 minutes"), &fl)) { sc.modeOpt[0] = fl ? 1 : 0; changed = true; }
    } else if (K == kHsPulsar) {
        float v = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 0.1f;
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::SliderFloat(tr("Pulse S/N over the band"), &v, 0.02f, 1.f, "%.2f", ImGuiSliderFlags_Logarithmic)) { sc.modeVal[0] = v; changed = true; }
    }
}

void tick(App& a, int K) {
    load(K);
    State& s = gS[K];
    HamsciReceiver& r = a.engine.hamsci(K);
    const bool run = a.engine.running();
    if (run && !s.wasRunning) {   // a new start: send the saved options again
        s.pushed = false;
    }
    if (!s.pushed && run) {
        s.pushed = true;
        r.setAuto(s.autoOn);
        if (!s.autoOn) {
            if (K == kHsQrss || K == kHsDfcw) { r.setQrssCentreHz(s.centreHz); r.setQrssDotSec(s.dotSec); r.setDfcwReverse(s.dfcwRev); }
            if (K == kHsIscat) r.setVariant(s.variant == 0 ? 2 : s.variant - 1);
            if (K == kHsThrob) r.setVariant(s.variant - 1);
        }
        if (K == kHsSid) r.setSidRiseDb(s.sidRise);
        if (hamsciEvtKind(K)) r.setEvtThresholdDb(s.evtThr > 0.f ? s.evtThr : -1.0);
        if (K == kHsRiometer) { r.setRioLongitude(s.rioLon); r.setRioThresholdDb(s.rioThr); if (!s.qdcSaved.empty()) r.rioImportQdc(s.qdcSaved); }
    }
    if (K == kHsRiometer && run && live(a, K) && a.rx.hamsci.seq != s.qdcSeen && a.rx.hamsci.rio.powerHistory.size() % 30 == 1) {   // keep the quiet-day curve between runs, every 30 minutes
        s.qdcSeen = a.rx.hamsci.seq;
        s.qdcSaved = r.rioExportQdc();
        plat::prefs().setS("hsRioQdc", s.qdcSaved);
        plat::prefs().flush();
    }
    s.wasRunning = run;
}

void status(App& a, int K) {
    const bool run = a.engine.running(), on = live(a, K);
    lamp("IQ", run ? 1 : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.hamsci.state >= 3 ? 1 : 2); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : hamsciSummary(a.rx.hamsci).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2, int K) {
    l1 = hamsciName(K);
    if (live(a, K)) l2 = hamsciSummary(a.rx.hamsci);
}

void command(App& a, int K) {
    load(K);
    if (!live(a, K)) { kit::modeHeader({(uint16_t)hamsciFamily(K), hamsciName(K), a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6}); intro(K); notRunning(a); return; }
    modepanel::panes({{"##hsmain", nullptr, [&a, K] { tab(a, K); }, 2.2f, 300.f},
                      {"##hsrx", tr("Receiver"), [&a, K] { receiver(a, K); }, 1.f, 220.f}}, 340.f);
}

template <int K> constexpr ModeUi makeUi() {
    return ModeUi{.sideTitle = hamsciAudioKind(K) ? "DECODES" : hamsciEvtKind(K) ? "EVENTS" : "READINGS", .tabName = hamsciAudioKind(K) ? "Messages" : hamsciEvtKind(K) ? "Events" : K == kHsSid ? "Stations" : K == kHsRiometer ? "Absorption" : "Pulsar", .tabIcon = Ic::Wave,
                  .tab = [](App& a) { tab(a, K); }, .receiver = [](App& a) { receiver(a, K); }, .list = [](App& a) { list(a, K); },
                  .status = [](App& a) { status(a, K); }, .summary = [](const App& a, std::string& x, std::string& y) { summary(a, x, y, K); },
                  .decoder = [](App& a, bool& rt) { decoder(a, rt, K); }, .synth = [](App& a, bool& ch) { synth(a, ch, K); },
                  .tick = [](App& a) { tick(a, K); }, .command = [](App& a) { command(a, K); }};
}

}  // namespace

extern const ModeUi kHamsciUi[kHamsciKinds];
const ModeUi kHamsciUi[kHamsciKinds] = {makeUi<0>(), makeUi<1>(), makeUi<2>(), makeUi<3>(), makeUi<4>(), makeUi<5>(), makeUi<6>(), makeUi<7>(), makeUi<8>(), makeUi<9>(), makeUi<10>(), makeUi<11>(), makeUi<12>(), makeUi<13>(), makeUi<14>(), makeUi<15>()};
