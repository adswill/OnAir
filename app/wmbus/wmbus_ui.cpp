// Wireless M-Bus screens (EN 13757-4 utility meters): the meters heard (manufacturer, ID, device type, readings, level), the frame log with the decoded
// fields, the channels with their level and the demodulated signal of the last frame. Encrypted payloads are listed as encrypted.
#include "app.h"
#include "dect2/wmbus/wmbus_live.h"
#include "dect2/wmbus/wmbus_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                 // 0 meters, 1 frames, 2 signal
    std::string sel;              // key of the meter shown in detail: manufacturer + ID
};
State S;

bool live(const App& a) { return a.rx.standard == 87; }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string clock(int64_t sec) {
    const int64_t d = ((sec % 86400) + 86400) % 86400;
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", (int)(d / 3600), (int)(d / 60 % 60), (int)(d % 60));
    return b;
}

std::string key(const dect2::WmbusDevice& d) { return d.mfr + " " + d.id; }

void stats(const dect2::WmbusTelemetry& t) {
    ImGui::TextDisabled("%llu frames, %llu CRC failures, %llu encrypted, %llu syncs, %llu false syncs",
                        (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.encryptedFrames,
                        (unsigned long long)t.syncs, (unsigned long long)t.falseSync);
}

void metersView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the meters")); return; }
    const dect2::WmbusTelemetry& t = a.rx.wmbus;
    stats(t);
    if (ImGui::Button(tr("Clear"))) a.engine.wmbus().clearResults();
    const float h = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y * 0.5f);
    if (ImGui::BeginTable("##wmdev", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Maker")); ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn(tr("Type"));
        ImGui::TableSetupColumn(tr("Mode")); ImGui::TableSetupColumn(tr("Heard")); ImGui::TableSetupColumn(tr("Last reading"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& d : t.devices) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool sel = S.sel == key(d);
            if (ImGui::Selectable((d.mfr + "##" + d.id).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = key(d);
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d.id.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.type.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::wmbus::phyName((dect2::wmbus::Phy)d.phy));
            ImGui::TableNextColumn(); ImGui::Text("%llux, %s", (unsigned long long)d.count, clock(d.lastSeen).c_str());
            ImGui::TableNextColumn();
            if (d.encrypted) ImGui::TextColored(pal::warnAmber(), "%s", tr("encrypted payload"));
            else ImGui::TextUnformatted(d.last.c_str());
        }
        ImGui::EndTable();
    }
    if (t.devices.empty()) ImGui::TextDisabled("%s", tr("no meter heard yet: most meters send every few seconds to a few minutes"));
    for (const auto& d : t.devices) {
        if (key(d) != S.sel) continue;
        ImGui::Spacing();
        ImGui::Text("%s %s", d.mfr.c_str(), d.id.c_str());
        kvRow(a, tr("type"), "%s, version %u", d.type.c_str(), (unsigned)d.version);
        kvRow(a, tr("level"), "%.1f dBFS, %s, %llu frames", d.rssiDb, dect2::wmbus::phyName((dect2::wmbus::Phy)d.phy), (unsigned long long)d.count);
        if (!d.status.empty()) kvRow(a, tr("status"), "%s", d.status.c_str());
        {   // the leak check (docs/SMART5-field-3.9.md) follows only the meters the user marks as their own
            bool mine = wmbusIsMine(key(d));
            if (ImGui::Checkbox(tr("My water meter: check for leaks"), &mine)) wmbusSetMine(key(d), mine);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Night flow and continuous flow from this meter's volume register; the findings appear in View > Insights. Only meters you mark are followed."));
        }
        if (d.encrypted) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", tr("The payload of this meter is encrypted (AES). It is listed but never decoded.")); ImGui::PopTextWrapPos(); }
        for (const auto& r : d.records) {
            kvRow(a, r.quantity.c_str(), "%s%s%s", r.value.c_str(), r.note.empty() ? "" : "   ", r.note.c_str());
        }
        break;
    }
}

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the frames")); return; }
    const dect2::WmbusTelemetry& t = a.rx.wmbus;
    if (ImGui::Button(tr("Copy"))) {
        std::string txt;
        for (const auto& l : t.log) txt += clock(l.wallTime) + "\t" + dect2::wmbus::phyName((dect2::wmbus::Phy)l.phy) + "\t" + l.mfr + " " + l.id + "\t" + l.summary + "\t" + l.hex + "\n";
        ImGui::SetClipboardText(txt.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) a.engine.wmbus().clearResults();
    ImGui::SameLine();
    stats(t);
    if (!ImGui::BeginTable("##wmlog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Mode")); ImGui::TableSetupColumn(tr("Meter"));
    ImGui::TableSetupColumn(tr("Result"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Frame"));
    ImGui::TableHeadersRow();
    for (const auto& l : t.log) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(l.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::wmbus::phyName((dect2::wmbus::Phy)l.phy));
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%s %s", l.mfr.c_str(), l.id.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (!l.crcOk) ImGui::TextColored(pal::badRed(), "%s", l.summary.c_str());
        else if (l.encrypted) ImGui::TextColored(pal::warnAmber(), "%s", l.summary.c_str());
        else ImGui::TextColored(pal::okGreen(), "%s", l.summary.c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(l.hex.c_str()); ImGui::PopFont();
    }
    ImGui::EndTable();
    if (t.log.empty()) ImGui::TextDisabled("%s", tr("no frame yet"));
}

void signalView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the signal")); return; }
    const dect2::WmbusTelemetry& t = a.rx.wmbus;
    if (ImGui::BeginTable("##wmch", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Channel")); ImGui::TableSetupColumn(tr("Mode")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("Frames")); ImGui::TableSetupColumn(tr("CRC failures"));
        ImGui::TableHeadersRow();
        for (const auto& c : t.channels) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.modes.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", c.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.frames);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.bad);
        }
        ImGui::EndTable();
    }
    if (t.channels.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("No Wireless M-Bus channel lies inside this capture. Tune to 868.625 MHz with 2 Msps (S1, T1, C1, R2) or to 169.4375 MHz (N1)."));
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    if (t.levelHist.size() > 1) {
        ImGui::TextDisabled("%s", tr("strongest channel"));
        float lo = 1e30f, hi = -1e30f;
        for (float v : t.levelHist) { lo = std::min(lo, v); hi = std::max(hi, v); }
        ImGui::PlotLines("##wmlv", t.levelHist.data(), (int)t.levelHist.size(), 0, nullptr, lo - 3, hi + 3, ImVec2(ImGui::GetContentRegionAvail().x, 46 * gUi));
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("last frame at the chip slicer"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The demodulated frequency of the last frame, one point per chip: +-1 is the nominal deviation of the mode."));
    std::vector<float> v(t.trace.size());
    for (size_t i = 0; i < v.size(); i++) v[i] = (float)t.trace[i] / 16384.f;
    if (v.size() > 1) ImGui::PlotLines("##wmtr", v.data(), (int)v.size(), 0, nullptr, -2.f, 2.f, ImVec2(ImGui::GetContentRegionAvail().x, 80 * gUi));
}

void tab(App& a) {
    subNav("wmb", S.view, {tr("Meters"), tr("Frames"), tr("Signal"), tr("Mine")});
    if (S.view == 0) metersView(a);
    else if (S.view == 1) framesView(a);
    else if (S.view == 3) wmbusMineView(a);
    else signalView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::WmbusTelemetry& t = a.rx.wmbus;
    ImGui::Text("%zu %s", t.devices.size(), tr("meters"));
    ImGui::PushFont(a.mono, 0);
    int n = 0;
    for (const auto& d : t.devices) { if (n++ >= 8) break; ImGui::Text("%s %s", d.mfr.c_str(), d.id.c_str()); }
    ImGui::PopFont();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::WmbusTelemetry& t = a.rx.wmbus;
    kvRow(a, tr("state"), "%s", dect2::wmbusSummary(t).c_str());
    kvRow(a, tr("channels"), "%zu in the capture, %.4f MHz centre", t.channels.size(), t.centreHz / 1e6);
    kvRow(a, tr("frames"), "%llu frames, %llu CRC failures, %llu encrypted, %llu syncs, %llu false syncs", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
          (unsigned long long)t.encryptedFrames, (unsigned long long)t.syncs, (unsigned long long)t.falseSync);
    kvRow(a, tr("input"), "%.3f Msps", t.inputRate / 1e6);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("868 MHz: S1 868.3, T1 and C1 868.95, R2 868.03 - 868.15 MHz; 169 MHz: N1 169.40625 - 169.46875 MHz. Encrypted payloads are never decoded."));
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.wmbus.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Frames"), !on ? 0 : a.rx.wmbus.blocksOk > 0 ? 1 : (a.rx.wmbus.blocksBad > 0 ? 2 : 0)); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::wmbusSummary(a.rx.wmbus).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Wireless M-Bus";
    if (live(a)) l2 = dect2::wmbusSummary(a.rx.wmbus);
}

// the filters: the meters and frames of one mode, or of one manufacturer (WmbusLiveReceiver::setPhyMask, setMfrFilter); every channel is still searched
struct PhyChoice { const char* name; uint32_t mask; };
const PhyChoice kPhys[] = {{"every mode", 0}, {"S1", 1u << (int)dect2::wmbus::Phy::S1}, {"T1", 1u << (int)dect2::wmbus::Phy::T1},
                           {"C1", (1u << (int)dect2::wmbus::Phy::C1A) | (1u << (int)dect2::wmbus::Phy::C1B)}, {"R2", 1u << (int)dect2::wmbus::Phy::R2},
                           {"N1 (Wize, 169 MHz)", (1u << (int)dect2::wmbus::Phy::N1) | (1u << (int)dect2::wmbus::Phy::N24)}};
struct MfrChoice { const char* name; const char* code; };
const MfrChoice kMfrs[] = {{"every manufacturer", ""}, {"Kamstrup", "KAM"}, {"Landis+Gyr", "LGB"}, {"Itron", "ITW"}, {"Sensus", "SEN"}};
constexpr int kNumPhys = (int)(sizeof kPhys / sizeof *kPhys), kNumMfrs = (int)(sizeof kMfrs / sizeof *kMfrs);
int& phyFilter() { static int v = std::clamp((int)plat::prefs().getI("wmbusPhyFilter", 0), 0, kNumPhys - 1); return v; }
int& mfrFilter() { static int v = std::clamp((int)plat::prefs().getI("wmbusMfrFilter", 0), 0, kNumMfrs - 1); return v; }
void pushFilters(App& a) { a.engine.wmbus().setPhyMask(kPhys[phyFilter()].mask); a.engine.wmbus().setMfrFilter(kMfrs[mfrFilter()].code); }   // every frame: each receiver gets them

void tick(App& a) { pushFilters(a); }

void decoder(App& a, bool&) {
    ImGui::TextDisabled("MODE"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##wmbphy", kPhys[phyFilter()].name)) {
        for (int i = 0; i < kNumPhys; i++) if (ImGui::Selectable(kPhys[i].name, i == phyFilter())) { phyFilter() = i; plat::prefs().setI("wmbusPhyFilter", i); pushFilters(a); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("List the meters and frames of one mode only. Every channel is still searched (N1 needs the 169 MHz plan).");
    ImGui::TextDisabled("MAKER"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##wmbmfr", kMfrs[mfrFilter()].name)) {
        for (int i = 0; i < kNumMfrs; i++) if (ImGui::Selectable(kMfrs[i].name, i == mfrFilter())) { mfrFilter() = i; plat::prefs().setI("wmbusMfrFilter", i); pushFilters(a); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("List the meters of one manufacturer only (the code in the frame header), in every mode.");
}

void synth(App&, bool&) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The test signal sends six simulated meters (S1, T1, C1 in both formats, R2) on the 868 MHz channels, one with an encrypted payload."));
    ImGui::PopTextWrapPos();
}

} // namespace

extern const ModeUi kWmbusUi;
const ModeUi kWmbusUi = {
    .sideTitle = "METERS",
    .tabName = "Meters",
    .tabIcon = Ic::Gauge,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
};
