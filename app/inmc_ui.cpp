// Inmarsat-C screens: the message list with a reading pane, the channel status with the packet counters, and the system information of the
// station (bulletin board).
#include "app.h"
#include "dect2/inmc_pkt.h"
#include "dect2/inmc_tel.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>

namespace {

struct State {
    bool loaded = false;
    int viewMode = 0;                    // 0 messages, 1 channels, 2 system
    int minPrio = 0;                     // show messages of at least this priority
    int kindFilter = 0;                  // 0 all, 1 SafetyNET, 2 FleetNET
    uint64_t selected = 0;               // key of the message in the reading pane (0 = none)
    bool search = true;                  // look for more channels in the band
    int pushedSearch = -1;
    int sat = 0;                         // 0 = not chosen, else 1 + index into kSats
    double hintSince = -1;               // when the tuner started waiting for a first message with no satellite chosen
};
State S;

// Satellites that carry the Inmarsat-C NCS common channel (the TDM channel with the EGC broadcasts), one per ocean region.
// Sources: thebaldgeek.github.io/stdc.html, sigidwiki.com/wiki/Inmarsat-C_TDM, and the satellite tables of
// github.com/alphafox02/inmarsat-sniffer (satellites.c).
struct Sat { const char* name; double mhz; const char* tip; };
const Sat kSats[] = {
    {"Alphasat 25E (Europe, Africa, Indian Ocean)", 1537.10, "NCS common channel of the Indian Ocean region (IOR)."},
    {"Inmarsat-4 F3 98W (Americas)", 1537.70, "NCS common channel of the Atlantic Ocean region West (AOR-W)."},
    {"Inmarsat-3 F5 54W (Atlantic East)", 1541.45, "NCS common channel of the Atlantic Ocean region East (AOR-E)."},
    {"Inmarsat-4 F1 143.5E (Pacific, Asia)", 1541.45, "NCS common channel of the Pacific Ocean region (POR). Community value (thebaldgeek, sigidwiki), not confirmed from Inmarsat's own list."},
};
constexpr int kNumSats = (int)(sizeof kSats / sizeof kSats[0]);

bool live(const App& a) { return a.engine.running() && a.rx.standard == 18; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.search = plat::prefs().getD("inmcSearch", 1.0) != 0.0;
    const long v = plat::prefs().getI("inmcSat", 0);
    S.sat = v >= 1 && v <= kNumSats ? (int)v : 0;
}

uint64_t keyOf(const dect2::InmcMessage& m) { return ((uint64_t)m.channel << 40) | ((uint64_t)m.serviceCode << 24) | m.id | (1ull << 62); }

ImVec4 prioColour(int p) {
    if (p >= 3) return pal::badRed();
    if (p == 2) return pal::warnAmber();
    if (p == 1) return pal::accent();
    return pal::grey();
}

// the text on one line: line breaks and control characters become spaces
std::string oneLine(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '\n' || c == '\r' || (unsigned char)c < 32) ? ' ' : c;
    return o;
}

const char* stateName(int s) { return s == 2 ? "decoding" : s == 1 ? "carrier" : "searching"; }

bool passes(const dect2::InmcMessage& m) {
    if (m.priority < S.minPrio) return false;
    if (S.kindFilter && m.kind != S.kindFilter) return false;
    return true;
}

void tick(App& a) {
    loadState();
    if (!a.engine.running()) { S.pushedSearch = -1; return; }
    if (S.pushedSearch != (S.search ? 1 : 0)) { a.engine.inmc().setChannelSearch(S.search); S.pushedSearch = S.search ? 1 : 0; }
}

// ---------------------------------------------------------------- views

void kvLine(const App& a, const char* k, const char* v) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

const dect2::InmcMessage* findMsg(const dect2::InmcTelemetry& t, uint64_t key) {
    for (const auto& m : t.messages) if (keyOf(m) == key) return &m;
    return nullptr;
}

void readingPane(const App& a, const dect2::InmcMessage* m) {
    if (!m) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Click a message to read all of it."); ImGui::PopTextWrapPos(); } return; }
    char b[160];
    snprintf(b, sizeof b, "%s, %s", m->rxTime.c_str(), dect2::inmc::priorityName(m->priority)); kvLine(a, "received", b);
    snprintf(b, sizeof b, "%02X  %s", m->serviceCode, m->serviceText.c_str()); kvLine(a, "service", b);
    snprintf(b, sizeof b, "%u, repetition %d, %d packet%s%s", m->id, m->repetition, m->packets, m->packets == 1 ? "" : "s", m->complete ? "" : ", not complete yet"); kvLine(a, "message", b);
    if (!m->area.empty()) kvLine(a, "area bytes", m->area.c_str());
    if (m->sat >= 0 && !m->lesName.empty()) { snprintf(b, sizeof b, "%s, %s (a guess)", dect2::inmc::satName(m->sat).c_str(), m->lesName.c_str()); kvLine(a, "station", b); }
    snprintf(b, sizeof b, "%s", m->presentation == 6 ? "ITA2" : m->presentation == 7 ? "binary" : m->presentation == 0 ? "IA5" : "other"); kvLine(a, "coding", b);
    if (m->seen > 0) { snprintf(b, sizeof b, "%u time%s complete", m->seen, m->seen == 1 ? "" : "s"); kvLine(a, "heard", b); }
    ImGui::Spacing();
    ImGui::TextDisabled("Text");
    ImGui::BeginChild("##inmtext", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    if (m->text.empty()) ImGui::TextDisabled("(no text yet)"); else ImGui::TextWrapped("%s", m->text.c_str());
    if (m->truncated) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("(the rest of this older message was cut to keep the list small)"); ImGui::PopTextWrapPos(); }
    ImGui::PopFont();
    ImGui::EndChild();
}

void messageView(App& a) {
    const dect2::InmcTelemetry& t = a.rx.inmc;
    const bool on = live(a);
    ImGui::TextDisabled("Priority"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    static const char* prios[] = {"all", "safety +", "urgency +", "distress"};
    ImGui::Combo("##inmprio", &S.minPrio, prios, 4);
    ImGui::SameLine(0, 12 * gUi);
    ImGui::TextDisabled("Service"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    static const char* kinds[] = {"all", "SafetyNET", "FleetNET"};
    ImGui::Combo("##inmkind", &S.kindFilter, kinds, 3);
    const float H = ImGui::GetContentRegionAvail().y;
    const float listH = std::max(120.f * gUi, H * 0.5f);
    if (ImGui::BeginTable("##inmmsg", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("Priority"); ImGui::TableSetupColumn("Service"); ImGui::TableSetupColumn("Ch");
        ImGui::TableSetupColumn("No."); ImGui::TableSetupColumn("Pkts"); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int shown = 0;
        if (on) {
            for (const auto& m : t.messages) {
                if (!passes(m)) continue;
                shown++;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char id[32]; snprintf(id, sizeof id, "##m%llx", (unsigned long long)keyOf(m));
                if (ImGui::Selectable((m.rxTime + id).c_str(), S.selected == keyOf(m), ImGuiSelectableFlags_SpanAllColumns)) S.selected = keyOf(m);
                ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, prioColour(m.priority)); ImGui::TextUnformatted(dect2::inmc::priorityName(m.priority)); ImGui::PopStyleColor();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.kind == 1 ? "SafetyNET" : m.kind == 2 ? "FleetNET" : "System");
                ImGui::TableNextColumn(); ImGui::Text("%d", m.channel);
                ImGui::TableNextColumn(); ImGui::Text("%u", m.id);
                ImGui::TableNextColumn(); ImGui::Text("%d%s", m.packets, m.complete ? "" : "+");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(oneLine(m.text), ImGui::GetContentRegionAvail().x).c_str());   // a preview: the whole text is shown when it is selected
            }
        }
        ImGui::EndTable();
        if (!on) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); ImGui::PopTextWrapPos(); }
        else if (!shown) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(t.messages.empty() ? "no message yet: a frame takes 8.64 s and the first packets can take a minute" : "no message matches the filter"); ImGui::PopTextWrapPos(); }
    }
    ImGui::Spacing();
    ImGui::BeginChild("##inmdet", ImVec2(0, 0));
    readingPane(a, on ? findMsg(t, S.selected) : nullptr);
    ImGui::EndChild();
}

void channelView(const App& a) {
    const dect2::InmcTelemetry& t = a.rx.inmc;
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the channel"); ImGui::PopTextWrapPos(); } return; }
    char b[160];
    snprintf(b, sizeof b, "%s, carrier %s, frames %s", stateName(t.state), t.carrierLock ? "locked" : "not locked", t.frameLock ? "in step" : "not found"); kvLine(a, "state", b);
    snprintf(b, sizeof b, "%u  (%s)", t.frameNumber, t.ncs.valid ? t.ncs.frameTime.c_str() : "-"); kvLine(a, "frame number", b);
    snprintf(b, sizeof b, "last %d of 128, average %.1f", t.uwErrors, t.uwErrorsAvg); kvLine(a, "unique word errors", b);
    snprintf(b, sizeof b, "%.2f %%", 100.0 * t.symbolErrorRate); kvLine(a, "symbol errors", b);
    snprintf(b, sizeof b, "Es/N0 %.1f dB, Eb/N0 %.1f dB", t.esn0Db, t.ebn0Db); kvLine(a, "signal quality", b);
    snprintf(b, sizeof b, "%+.0f Hz, drift %+.1f Hz/s", t.cfoHz, t.driftHzS); kvLine(a, "carrier", b);
    snprintf(b, sizeof b, "%llu found, %llu good, %llu without a good bulletin board", (unsigned long long)t.framesFound, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); kvLine(a, "frames", b);
    snprintf(b, sizeof b, "%llu good, %llu failed the check", (unsigned long long)t.packetsOk, (unsigned long long)t.packetsBad); kvLine(a, "packets", b);
    ImGui::Spacing();
    const float avail = ImGui::GetContentRegionAvail().y;
    if (t.channels.size() > 1 && ImGui::BeginTable("##inmch", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::min(avail * 0.4f, (float)(t.channels.size() + 1) * ImGui::GetTextLineHeightWithSpacing())))) {
        ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("kHz"); ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("Eb/N0");
        ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Msgs"); ImGui::TableSetupColumn("Station", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& c : t.channels) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", c.index);
            ImGui::TableNextColumn(); ImGui::Text("%+.1f", c.carrierHz / 1000.0);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(stateName(c.state));
            ImGui::TableNextColumn(); ImGui::Text("%.1f", c.ebn0Db);
            ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)c.framesOk, (unsigned long long)c.framesBad);
            ImGui::TableNextColumn(); ImGui::Text("%u", c.messages);
            ImGui::TableNextColumn(); ImGui::Text("%s %s", c.channelTypeName.c_str(), c.lesName.c_str());
        }
        ImGui::EndTable();
        ImGui::Spacing();
    }
    ImGui::TextDisabled("Packets by type");
    if (ImGui::BeginTable("##inmpk", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Descriptor"); ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Good"); ImGui::TableSetupColumn("Failed");
        ImGui::TableHeadersRow();
        for (int d = 0; d < 256; d++) {
            if (!t.pktOk[d] && !t.pktBad[d]) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%02X", d);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::inmc::descriptorName((uint8_t)d));
            ImGui::TableNextColumn(); ImGui::Text("%u", t.pktOk[d]);
            ImGui::TableNextColumn();
            if (t.pktBad[d]) { ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber()); ImGui::Text("%u", t.pktBad[d]); ImGui::PopStyleColor(); } else ImGui::TextDisabled("0");
        }
        ImGui::EndTable();
    }
}

void systemView(const App& a) {
    const dect2::InmcTelemetry& t = a.rx.inmc;
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the system information"); ImGui::PopTextWrapPos(); } return; }
    if (!t.ncs.valid) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("waiting for a bulletin board packet"); ImGui::PopTextWrapPos(); } return; }
    const dect2::InmcNcsInfo& n = t.ncs;
    char b[200];
    kvLine(a, "ocean region", n.region.c_str());
    snprintf(b, sizeof b, "%d  %s", n.lesId, n.lesName.empty() ? "(not in the table)" : n.lesName.c_str()); kvLine(a, "station", b);
    kvLine(a, "channel type", n.channelTypeName.c_str());
    snprintf(b, sizeof b, "%u, network time %s", n.frameNo, n.frameTime.c_str()); kvLine(a, "frame", b);
    snprintf(b, sizeof b, "%d", n.networkVersion); kvLine(a, "network version", b);
    snprintf(b, sizeof b, "%d", n.signallingChannel); kvLine(a, "signalling channel", b);
    snprintf(b, sizeof b, "%d", n.count); kvLine(a, "count", b);
    kvLine(a, "status", n.statusText.empty() ? "-" : n.statusText.c_str());
    snprintf(b, sizeof b, "%d", n.randomInterval); kvLine(a, "random interval", b);
    if (n.signallingUplinkMhz > 0) { snprintf(b, sizeof b, "%.4f MHz", n.signallingUplinkMhz); kvLine(a, "signalling uplink", b); }
    ImGui::TextDisabled("services"); kvColumn(130 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextWrapped("%s", n.servicesText.empty() ? "-" : n.servicesText.c_str()); ImGui::PopFont();
}

// ---------------------------------------------------------------- hooks

void tab(App& a) {
    loadState();
    subNav("inmcv", S.viewMode, {"Messages", "Channel", "System"});
    if (S.viewMode == 0) messageView(a);
    else if (S.viewMode == 1) channelView(a);
    else systemView(a);
}

// the bottom row of the overview: the newest message, and the packets seen by type
void panels(App& a) {
    const dect2::InmcTelemetry& t = a.rx.inmc;
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); ImGui::PopTextWrapPos(); } return; }
    const float W = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##inmnew", ImVec2(W * 0.64f, 0), ImGuiChildFlags_Borders);
    if (t.messages.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no message yet: a frame takes 8.64 s and a long message needs several frames"); ImGui::PopTextWrapPos(); }
    else {
        const dect2::InmcMessage& m = t.messages.front();
        ImGui::PushStyleColor(ImGuiCol_Text, prioColour(m.priority));
        ImGui::Text("%s  %s", dect2::inmc::priorityName(m.priority), m.serviceText.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(); ImGui::TextDisabled("%s, no. %u, %d packet%s%s", m.rxTime.c_str(), m.id, m.packets, m.packets == 1 ? "" : "s", m.complete ? "" : ", more to come");
        ImGui::PushFont(a.mono, 0);
        ImGui::TextWrapped("%s", m.text.empty() ? "(no text yet)" : m.text.c_str());
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##inmpkt", ImVec2(0, 0), ImGuiChildFlags_Borders);
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Packets good / failed"); ImGui::PopTextWrapPos(); }
    for (int d = 0; d < 256; d++) {
        if (!t.pktOk[d] && !t.pktBad[d]) continue;
        { ImGui::PushTextWrapPos(0); ImGui::Text("%02X %-22s %u / %u", d, dect2::inmc::descriptorName((uint8_t)d), t.pktOk[d], t.pktBad[d]); ImGui::PopTextWrapPos(); }
    }
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see messages"); ImGui::PopTextWrapPos(); } return; }
    const dect2::InmcTelemetry& t = a.rx.inmc;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu in the list, %u seen", t.messages.size(), t.messageCount); ImGui::PopTextWrapPos(); }
    if (!ImGui::BeginTable("##inml", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Prio"); ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& m : t.messages) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(m.rxTime.substr(0, 8).c_str());
        ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, prioColour(m.priority)); ImGui::TextUnformatted(dect2::inmc::priorityName(m.priority)); ImGui::PopStyleColor();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(m.kind == 1 ? "SafetyNET" : m.kind == 2 ? "FleetNET" : "System");
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::InmcTelemetry& t = a.rx.inmc;
    char b[160];
    snprintf(b, sizeof b, "%s, %zu channel%s", stateName(t.state), t.channels.size(), t.channels.size() == 1 ? "" : "s"); kvLine(a, "state", b);
    snprintf(b, sizeof b, "%+.0f Hz off, drift %+.1f Hz/s", t.cfoHz, t.driftHzS); kvLine(a, "carrier", b);
    snprintf(b, sizeof b, "Eb/N0 %.1f dB", t.ebn0Db); kvLine(a, "signal", b);
    snprintf(b, sizeof b, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); kvLine(a, "frames", b);
    snprintf(b, sizeof b, "%u seen", t.messageCount); kvLine(a, "messages", b);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::InmcTelemetry& t = a.rx.inmc;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.carrierLock ? 1 : 0); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : t.frameLock ? 1 : t.framesFound > 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("Messages", !on ? 0 : t.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", stateName(t.state));
    snprintf(b, sizeof b, "%u", t.frameNumber); ro("Frame", b);
    snprintf(b, sizeof b, "%.1f dB", t.ebn0Db); ro("Eb/N0", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Good / bad", b);
    snprintf(b, sizeof b, "%u", t.messageCount); ro("Messages", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Inmarsat-C";
    if (live(a)) l2 = dect2::inmcSummary(a.rx.inmc);
}

void tuner(App& a, bool& retune) {
    loadState();
    const dect2::InmcTelemetry& t = a.rx.inmc;
    ImGui::TextDisabled("Satellite"); ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##icsat", S.sat ? kSats[S.sat - 1].name : "Not chosen (tune by hand)")) {
        if (ImGui::Selectable("Not chosen (tune by hand)", S.sat == 0)) { S.sat = 0; plat::prefs().setI("inmcSat", 0); savePrefs(a); }
        for (int i = 0; i < kNumSats; i++) {
            if (ImGui::Selectable(kSats[i].name, S.sat == i + 1)) {
                S.sat = i + 1; plat::prefs().setI("inmcSat", i + 1); savePrefs(a);
                a.freqMhz = kSats[i].mhz; retune = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tunes to %.2f MHz.\n%s", kSats[i].mhz, kSats[i].tip);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Optional. Choosing a satellite tunes to its NCS common channel; you can still tune by hand.");
    if (ImGui::Checkbox("find more channels", &S.search)) plat::prefs().setD("inmcSearch", S.search ? 1.0 : 0.0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look for other Inmarsat-C channels in the captured band and decode up to three of them at the same time.\nThe channel you tuned to is always decoded.");
    // the hint only when nothing has come in for a while and no satellite is chosen
    const bool idle = !live(a) || t.messageCount > 0 || t.dataValid;
    if (S.sat != 0 || idle) S.hintSince = -1;
    else if (S.hintSince < 0) S.hintSince = ImGui::GetTime();
    if (S.hintSince >= 0 && ImGui::GetTime() - S.hintSince > 20.0)
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Tip: pick your satellite above so the receiver starts on its channels."); ImGui::PopTextWrapPos(); }
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated NCS");
    flowNext(); ImGui::SetNextItemWidth(150 * gUi);
    float eb = sc.modeVal[0] != 0 ? (float)sc.modeVal[0] : 10.f;
    if (ImGui::SliderFloat("##inmeb", &eb, 2.f, 20.f, "Eb/N0 %.1f dB")) { sc.modeVal[0] = eb; changed = true; }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    int more = std::max(0, std::min(3, sc.modeOpt[3]));
    if (ImGui::SliderInt("##inmmore", &more, 0, 3, "+%d ch.")) { sc.modeOpt[3] = more; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("More channels in the same capture, with their own messages.");
    flowNext(); ImGui::SetNextItemWidth(120 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##inmcfo", &cfo, -10, 10, "%.1f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    flowNext(); ImGui::SetNextItemWidth(120 * gUi);
    float dr = (float)sc.modeVal[1];
    if (ImGui::SliderFloat("##inmdr", &dr, -50, 50, "%.0f Hz/s")) { sc.modeVal[1] = dr; changed = true; }
    flowNext();
    bool inv = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("inverted", &inv)) { sc.modeOpt[1] = inv ? 1 : 0; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::InmcTelemetry& t = a.rx.inmc;
    out.push_back({"Eb/N0  dB", "%.1f", t.ebn0Db, 0, 15, !t.carrierLock ? 0 : t.ebn0Db >= 6 ? 1 : t.ebn0Db >= 4 ? 2 : 3});
    out.push_back({"UW ERRORS", "%.0f", t.uwErrorsAvg, 0, 40, t.framesFound == 0 ? 0 : t.uwErrorsAvg <= 10 ? 1 : t.uwErrorsAvg <= 25 ? 2 : 3});
    const double tot = (double)(t.blocksOk + t.blocksBad);
    const double good = tot > 0 ? 100.0 * (double)t.blocksOk / tot : 0.0;
    out.push_back({"FRAMES OK  %", "%.0f", good, 0, 100, tot == 0 ? 0 : good >= 90 ? 1 : good >= 60 ? 2 : 3});
    out.push_back({"MESSAGES", "%.0f", (double)t.messageCount, 0, 50, t.messageCount ? 1 : 0});
}

} // namespace

extern const ModeUi kInmcUi;
const ModeUi kInmcUi = {
    .sideTitle = "MESSAGES",
    .tabName = "Messages",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
