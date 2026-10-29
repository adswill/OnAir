// LoRaWAN (all channels) screens: the frame table (time, channel, SF, type, DevAddr, FCnt, FPort, level, SNR, MAC commands, payload bytes), the device list,
// the channel list of the plan with its counters, the plan and options. LoRaWAN FSK frames (DR7, lorawan_fsk.h) are in the same table, marked "FSK DR7". Receive only and keyless: the payloads are encrypted with the devices' session keys
// and are shown as bytes, no key is handled, nothing is decrypted, no MIC is checked, nothing is sent. "Level" is dB below full scale, not calibrated to dBm.
#include "app.h"
#include "dect2/lorawan/lorawan_plan.h"
#include "dect2/lorawan/lorawan_rx.h"
#include "dect2/lorawan/lorawan_tel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    dect2::LorawanSettings cfg;
    int view = 0;
    char filter[64] = "";
    int type = 0;
    bool hex = false;
};
State S;

void saveState() {
    auto& p = plat::prefs();
    p.setI("lwPlan", S.cfg.plan); p.setI("lwSub", S.cfg.subBand); p.setB("lwDown", S.cfg.downlinks); p.setB("lwDef", S.cfg.defaultsOnly);
    p.setB("lwPriv", S.cfg.privateSync); p.setI("lwMax", S.cfg.maxChannels); p.setB("lwWide", S.cfg.include250_500); p.setB("lwFsk", S.cfg.fsk);
    p.flush();
}
void loadState(App& a) {
    if (S.loaded) return;
    S.loaded = true;
    auto& p = plat::prefs();
    S.cfg.plan = (int)p.getI("lwPlan", 0); S.cfg.subBand = (int)p.getI("lwSub", 0); S.cfg.downlinks = p.getB("lwDown", true); S.cfg.defaultsOnly = p.getB("lwDef", false);
    S.cfg.privateSync = p.getB("lwPriv", false); S.cfg.maxChannels = (int)p.getI("lwMax", 24); S.cfg.include250_500 = p.getB("lwWide", true);
    S.cfg.fsk = p.getB("lwFsk", true);
    a.engine.lorawan().setSettings(S.cfg);
}
void apply(App& a) { a.engine.lorawan().setSettings(S.cfg); saveState(); }
bool live(const App& a) { return a.rx.standard == 201; }

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void privacyNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only and keyless: the headers every LoRaWAN gateway in range receives in the clear (message type, DevAddr, frame counter, port, MIC, MAC commands; "
                        "DevEUI and JoinEUI of join requests). The payloads are encrypted with the devices' own session keys and stay bytes: OnAir handles no keys, never decrypts, "
                        "never checks a MIC and never transmits. These are other people's devices. Needs a radio with 2 Msps or more; a 4 Msps capture covers all eight EU868 channels "
                        "and the FSK channel (DR7, 868.8 MHz).");
    ImGui::PopTextWrapPos();
}

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see LoRaWAN frames"); return; }
    const dect2::LorawanTelemetry& t = a.rx.lorawan;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", t.note.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(170 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##lwf", "DevAddr, EUI, type, MAC", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    static const char* types[] = {"all frames", "uplinks", "downlinks", "joins", "with MAC commands"};
    ImGui::SetNextItemWidth(std::min(140 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::Combo("##lwt", &S.type, types, 5);
    flowNext(10 * gUi);
    ImGui::Checkbox("hex", &S.hex);
    flowNext(10 * gUi);
    if (ImGui::Button("Clear")) a.engine.lorawan().clearResults();
    flowBreak();
    const std::string f = lower(S.filter);
    std::vector<size_t> rows;
    for (size_t i = 0; i < t.log.size(); i++) {
        const auto& r = t.log[i];
        if (S.type == 1 && !(r.lorawan && r.uplink && !r.inverted)) continue;
        if (S.type == 2 && !(r.lorawan && (!r.uplink || r.inverted))) continue;
        if (S.type == 3 && !(r.lorawan && (r.mtype == 0 || r.mtype == 1 || r.mtype == 6))) continue;
        if (S.type == 4 && r.mac.empty()) continue;
        if (!f.empty()) {
            char ad[16]; snprintf(ad, sizeof ad, "%08X", r.devAddr);
            const std::string hay = lower(std::string(r.hasDevAddr ? ad : "") + " " + r.type + " " + r.mac + " " + dect2::lorawan::eui(r.devEui) + " " + dect2::lorawan::eui(r.joinEui));
            if (hay.find(f) == std::string::npos) continue;
        }
        rows.push_back(i);
    }
    if (!ImGui::BeginTable("##lwlog", 12, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"Time UTC", "Channel MHz", "SF", "BW", "Type", "DevAddr / EUI", "FCnt", "FPort", "Level dBFS", "SNR dB", "MIC", "MAC commands / payload"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& r = t.log[rows[(size_t)i]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(r.wallTime).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f%s", r.freqHz / 1e6, r.inverted ? " v" : "");
            if (r.inverted && ImGui::IsItemHovered()) ImGui::SetTooltip("Inverted IQ: sent by a gateway (downlink)");
            ImGui::TableNextColumn();
            if (r.fsk) {
                ImGui::TextColored(pal::accent(), "%s", "FSK DR7");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "LoRaWAN FSK (DR7): 50 kbit/s GFSK, deviation 25 kHz, CRC-16 and whitening\nFrequency offset in the tooltip of the last column");
            } else ImGui::Text("%d", r.sf);
            ImGui::TableNextColumn();
            if (r.fsk) {
                ImGui::TextDisabled("%s", "50 kb/s");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Bit rate (the FSK channel is about 100 kHz wide)");
            } else ImGui::Text("%.0f", r.bwHz / 1e3);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.type.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0);
            if (r.hasDevAddr) ImGui::Text("%08X", r.devAddr); else if (r.devEui) ImGui::TextUnformatted(dect2::lorawan::eui(r.devEui).c_str()); else ImGui::TextDisabled("-");
            ImGui::PopFont();
            ImGui::TableNextColumn(); if (r.hasDevAddr) ImGui::Text("%u", r.fcnt); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.fport >= 0) ImGui::Text("%d", r.fport); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.0f", r.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.snrDb);
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); if (r.lorawan && r.mtype != 7 && r.mtype != 1) ImGui::Text("%08X", r.mic); else ImGui::TextDisabled("-"); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0);
            ImGui::TextUnformatted(S.hex || !r.lorawan ? r.hex.c_str() : (r.mac.empty() ? (r.flags.empty() ? r.summary.c_str() : r.flags.c_str()) : r.mac.c_str()));
            ImGui::PopFont();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s\n%d bytes%s\nfrequency offset %+.1f kHz", r.summary.c_str(), r.hex.c_str(), r.len, r.lorawan ? ", payload encrypted (never decrypted)" : "", r.cfoHz / 1e3);
        }
    ImGui::EndTable();
    if (t.log.empty()) ImGui::TextDisabled("no frames yet");
}

void devicesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver first"); return; }
    const dect2::LorawanTelemetry& t = a.rx.lorawan;
    if (!ImGui::BeginTable("##lwdev", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"Device", "Frames", "Up", "Down", "Joins", "Missed", "FCnt", "FPort", "SF", "Last"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (const auto& d : t.devices) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d.id.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%u", d.packets);
        ImGui::TableNextColumn(); ImGui::Text("%u", d.uplinks);
        ImGui::TableNextColumn(); ImGui::Text("%u", d.downlinks);
        ImGui::TableNextColumn(); ImGui::Text("%u", d.joins);
        ImGui::TableNextColumn(); ImGui::Text("%u", d.missed);
        ImGui::TableNextColumn(); if (d.fcnt >= 0) ImGui::Text("%ld", d.fcnt); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (d.lastFport >= 0) ImGui::Text("%d", d.lastFport); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (d.fsk) ImGui::TextDisabled("%s", "FSK"); else ImGui::Text("%d", d.sf);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.lastType.c_str());
    }
    ImGui::EndTable();
}

void channelsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver first"); return; }
    const dect2::LorawanTelemetry& t = a.rx.lorawan;
    ImGui::TextDisabled("%s", t.note.c_str());
    if (!ImGui::BeginTable("##lwch", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"MHz", "Bandwidth", "SF", "Use", "Decoded", "Preambles", "Frames / CRC errors"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (const auto& c : t.channels) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%.3f", c.freqHz / 1e6);
        ImGui::TableNextColumn(); ImGui::Text("%.0f kHz", c.bwHz / 1e3);
        ImGui::TableNextColumn(); if (c.fsk) ImGui::TextColored(pal::accent(), "%s", "FSK DR7"); else ImGui::Text("%d - %d", c.sfMin, c.sfMax);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.downlinkOnly ? "gateway downlink" : "uplink / both");
        ImGui::TableNextColumn(); if (c.inBand) ImGui::TextColored(pal::okGreen(), "yes"); else ImGui::TextDisabled("no");
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.preambles);
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)c.frames, (unsigned long long)c.crcBad);
    }
    ImGui::EndTable();
}

void settings(App& a, bool& retune) {
    (void)retune;
    loadState(a);
    bool ch = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Plan"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##lwplan", dect2::lorawan::planName(S.cfg.plan))) {
        for (int p = 0; p < dect2::lorawan::kPlans; p++)
            if (ImGui::Selectable(dect2::lorawan::planName(p), p == S.cfg.plan)) { S.cfg.plan = p; ch = true; tuneFreq(a, dect2::lorawanPlanCentreMhz(p)); retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nChoosing a plan also sets the frequency to the middle of its channels.", dect2::lorawan::planNote(S.cfg.plan));
    if (S.cfg.plan == dect2::lorawan::US915 || S.cfg.plan == dect2::lorawan::AU915) {
        flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Sub-band"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(70 * gUi, ImGui::GetContentRegionAvail().x));
        char l[16]; snprintf(l, sizeof l, S.cfg.subBand ? "%d" : "all", S.cfg.subBand);
        if (ImGui::BeginCombo("##lwsb", l)) {
            for (int s = 0; s <= 8; s++) { snprintf(l, sizeof l, s ? "%d" : "all", s); if (ImGui::Selectable(l, s == S.cfg.subBand)) { S.cfg.subBand = s; ch = true; } }
            ImGui::EndCombo();
        }
    }
    flowNext(10 * gUi);
    ch |= ImGui::Checkbox("Gateway downlinks", &S.cfg.downlinks);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also decode the gateways' frames (inverted IQ: join accepts, acknowledgements, MAC commands). Doubles the demodulators.");
    flowNext(10 * gUi);
    ch |= ImGui::Checkbox("Join channels only", &S.cfg.defaultsOnly);
    flowNext(10 * gUi);
    ch |= ImGui::Checkbox("Sync 0x12 too", &S.cfg.privateSync);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Private networks that use the sync word 0x12 instead of the public LoRaWAN 0x34 (more CPU).");
    flowNext(10 * gUi);
    ch |= ImGui::Checkbox("250 / 500 kHz", &S.cfg.include250_500);
    flowNext(10 * gUi);
    ch |= ImGui::Checkbox("FSK (DR7)", &S.cfg.fsk);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Also the plan's FSK channel: 50 kbit/s GFSK on 868.8 MHz (EU868) or 924.8 MHz (AS923-1), the channel The Things Network's gateways use. "
                                "Synthetic tests only so far.");
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Max channels"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(80 * gUi, ImGui::GetContentRegionAvail().x));
    ch |= ImGui::InputInt("##lwmax", &S.cfg.maxChannels, 0, 0);
    S.cfg.maxChannels = std::max(1, std::min(72, S.cfg.maxChannels));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every channel runs one demodulator per spreading factor: this keeps the CPU load down (the channels nearest the middle first).");
    flowBreak();
    if (ch) apply(a);
}

void tuner(App& a, bool& retune) {
    loadState(a);
    ImGui::TextDisabled("Plan");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##lwpre", dect2::lorawan::planName(S.cfg.plan))) {
        for (int p = 0; p < dect2::lorawan::kPlans; p++)
            if (ImGui::Selectable(dect2::lorawan::planName(p), p == S.cfg.plan)) { S.cfg.plan = p; apply(a); tuneFreq(a, dect2::lorawanPlanCentreMhz(p)); retune = true; }
        ImGui::EndCombo();
    }
}

void tab(App& a) {
    loadState(a);
    subNav("lwv", S.view, {"Frames", "Devices", "Channels", "Settings"});
    if (S.view == 0) { privacyNote(); framesView(a); }
    else if (S.view == 1) devicesView(a);
    else if (S.view == 2) channelsView(a);
    else { bool rt = false; privacyNote(); ImGui::Spacing(); ImGui::PushID("lwtab"); settings(a, rt); ImGui::PopID(); }
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::LorawanTelemetry& t = a.rx.lorawan;
    if (!ImGui::BeginTable("##lwl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("FCnt");
    for (size_t i = 0; i < t.devices.size() && i < 80; i++) {
        const auto& d = t.devices[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d.id.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); if (d.fsk) ImGui::TextDisabled("%s", "FSK"); else ImGui::TextDisabled("%d", d.sf);
        ImGui::TableNextColumn(); if (d.fcnt >= 0) ImGui::TextDisabled("%ld", d.fcnt);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::LorawanTelemetry& t = a.rx.lorawan;
    kvRow(a, "state", "%s", dect2::lorawanSummary(t).c_str());
    kvRow(a, "plan", "%s", dect2::lorawan::planName(t.cfg.plan));
    kvRow(a, "channels", "%s", t.note.c_str());
    kvRow(a, "frames", "%llu good, %llu CRC errors, %llu preambles", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.preambles);
    if (t.cfg.fsk && !dect2::lorawan::planFskChannelsMhz(t.cfg.plan).empty())   // DR7 in the plan (planHasFsk) is not enough: a channel to listen on
        kvRow(a, "FSK (DR7)", "%llu frames, %llu sync words, %llu CRC errors", (unsigned long long)t.fskFrames, (unsigned long long)t.fskSyncs, (unsigned long long)t.fskCrcBad);
    static const char* const mt[] = {"join req", "join acc", "unconf up", "unconf down", "conf up", "conf down", "rejoin", "proprietary"};
    std::string by;
    for (int i = 0; i < 8; i++) if (t.perType[i]) { char b[48]; snprintf(b, sizeof b, "%s%s %llu", by.empty() ? "" : ", ", mt[i], (unsigned long long)t.perType[i]); by += b; }
    kvRow(a, "by type", "%s", by.empty() ? "-" : by.c_str());
    kvRow(a, "centre", t.centreKnown ? "%.4f MHz" : "unknown (the plan's own is assumed)", t.centerHz / 1e6);
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    ImGui::Spacing();
    privacyNote();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : a.rx.lorawan.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::lorawanSummary(a.rx.lorawan).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "LoRaWAN";
    if (live(a)) l2 = dect2::lorawanSummary(a.rx.lorawan);
}

void synth(App& a, bool& changed) {
    ImGui::TextDisabled("Test signal plan");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
    int& o = a.tune.synth.modeOpt[0];
    if (ImGui::BeginCombo("##lwsyn", dect2::lorawan::planName(o))) {
        for (int p = 0; p < dect2::lorawan::kPlans; p++) if (ImGui::Selectable(dect2::lorawan::planName(p), o == p)) { o = p; changed = true; }
        ImGui::EndCombo();
    }
}

} // namespace

extern const ModeUi kLorawanUi;
const ModeUi kLorawanUi = {
    .sideTitle = "DEVICES",
    .tabName = "LoRaWAN",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = settings,
    .synth = synth,
};
