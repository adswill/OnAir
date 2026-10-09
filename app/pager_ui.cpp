// Pagers screens: lamps for the signal, the sync and the two protocols; the message list with a filter; the statistics per speed.
#include "app.h"
#include "dect2/pager_tel.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 messages, 1 statistics
    char filter[48] = "";                // matches the address or the text
    bool pocsag = true, flex = true;
};
State S;

constexpr size_t kShown = 400;           // rows drawn at most: the list wraps its text, so every row has its own height

bool live(const App& a) { return a.engine.running() && a.rx.standard == 24; }   // the engine reports its standard code minus one

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

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

std::string oneLine(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '\n' || c == '\r' || (unsigned char)c < 32) ? ' ' : c;
    return o;
}

bool passes(const dect2::PagerMessage& m) {
    if (m.flex ? !S.flex : !S.pocsag) return false;
    if (!S.filter[0]) return true;
    char adr[16];
    snprintf(adr, sizeof adr, "%u", (unsigned)m.address);
    const std::string f = lower(S.filter);
    return std::string(adr).find(f) != std::string::npos || lower(m.text).find(f) != std::string::npos;
}

const std::vector<dect2::PagerMessage>& messagesOf(const dect2::PagerTelemetry& t) {
    static const std::vector<dect2::PagerMessage> none;
    return t.messages ? *t.messages : none;
}

std::string fnText(const dect2::PagerMessage& m) { return m.function >= 0 ? std::to_string(m.function) : "-"; }

// ---------------------------------------------------------------- the lamps

void lampRow(const App& a) {
    const bool on = live(a);
    const dect2::PagerTelemetry& t = a.rx.pager;
    lamp("Signal", !on ? 0 : t.signal ? 1 : 0); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : t.sync ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("POCSAG", !on ? 0 : t.timeSec - t.lastPocsagSec < 60 ? 1 : t.lastPocsagSec > 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("FLEX", !on ? 0 : t.timeSec - t.lastFlexSec < 60 ? 1 : t.lastFlexSec > 0 ? 2 : 0);
    flowEnd();
}

// ---------------------------------------------------------------- the views

void messageView(App& a) {
    const dect2::PagerTelemetry& t = a.rx.pager;
    const bool on = live(a);
    const std::vector<dect2::PagerMessage>& all = messagesOf(t);
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputText("##pgf", S.filter, sizeof S.filter);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show only pages whose address or text contains this.");
    flowNext(12 * gUi);
    ImGui::Checkbox("POCSAG", &S.pocsag);
    flowNext(10 * gUi);
    ImGui::Checkbox("FLEX", &S.flex);
    flowNext(12 * gUi);
    const bool copy = ImGui::Button("Copy");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the pages that match the filter to the clipboard, one per line.");
    flowEnd();
    if (copy) {
        std::string txt;
        for (const auto& m : all) {
            if (!passes(m)) continue;
            txt += clock(m.wallTime) + "\t" + dect2::pagerSpeedName(m.speed) + "\t" + std::to_string(m.address) + "\t" + fnText(m) + "\t" + dect2::pagerTypeName(m.type) + "\t" + oneLine(m.text) + "\n";
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (ImGui::BeginTable("##pgmsg", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("Protocol"); ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Fn");
        ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        size_t shown = 0, matching = 0;
        if (on) {
            for (const auto& m : all) {
                if (!passes(m)) continue;
                matching++;
                if (shown >= kShown) continue;
                shown++;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(m.wallTime).c_str());
                ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, pal::accent()); ImGui::TextUnformatted(dect2::pagerSpeedName(m.speed)); ImGui::PopStyleColor();
                ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)m.address);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fnText(m).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::pagerTypeName(m.type));
                ImGui::TableNextColumn();
                ImGui::PushFont(a.mono, 0);
                if (m.type == dect2::kPagerTone) ImGui::TextDisabled("(tone only)");
                else ImGui::TextWrapped("%s%s", m.text.c_str(), m.damaged ? "  [incomplete]" : "");
                ImGui::PopFont();
            }
        }
        ImGui::EndTable();
        if (!on) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see pages"); ImGui::PopTextWrapPos(); }
        else if (!matching) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(all.empty() ? "no page yet" : "no page matches the filter"); ImGui::PopTextWrapPos(); }
    }
}

void statsView(App& a) {
    const dect2::PagerTelemetry& t = a.rx.pager;
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the counters"); ImGui::PopTextWrapPos(); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("last sync", "%s", t.lastSpeed >= 0 ? dect2::pagerSpeedName(t.lastSpeed) : "none yet");
    kv("channel", "%.0f dB over the noise, carrier %+.0f Hz off", t.snrDb, t.cfoHz);
    kv("pages", "%llu", (unsigned long long)t.messagesTotal);
    ImGui::Spacing();
    if (ImGui::BeginTable("##pgst", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("Speed"); ImGui::TableSetupColumn("Good"); ImGui::TableSetupColumn("Repaired"); ImGui::TableSetupColumn("Lost");
        ImGui::TableSetupColumn("Pages"); ImGui::TableSetupColumn(t.lastSpeed >= 0 && t.lastSpeed >= dect2::kFlex1600_2 ? "Frames" : "Batches");
        ImGui::TableHeadersRow();
        for (int s = 0; s < dect2::kPagerSpeeds; s++) {
            const dect2::PagerSpeedStat& st = t.speeds[s];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::pagerSpeedName(s));
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)st.ok);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)st.fixed);
            ImGui::TableNextColumn();
            if (st.failed) { ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber()); ImGui::Text("%llu", (unsigned long long)st.failed); ImGui::PopStyleColor(); }
            else ImGui::Text("0");
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)st.messages);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)st.transmissions);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Good: code words that passed the BCH check as received. Repaired: passed after the check corrected one or two bits. Lost: could not be repaired. "
                        "The last column counts POCSAG batches and FLEX frames that had at least one good code word.");
    ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------- hooks

void tab(App& a) {
    lampRow(a);
    ImGui::Spacing();
    subNav("pagerv", S.view, {"Messages", "Statistics"});
    if (S.view == 0) messageView(a); else statsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see pages"); ImGui::PopTextWrapPos(); return; }
    const dect2::PagerTelemetry& t = a.rx.pager;
    const std::vector<dect2::PagerMessage>& all = messagesOf(t);
    ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%llu pages", (unsigned long long)t.messagesTotal); ImGui::PopTextWrapPos();
    if (!ImGui::BeginTable("##pgl_s", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    size_t n = 0;
    for (const auto& m : all) {
        if (n++ >= 100) break;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)m.address);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(m.type == dect2::kPagerTone ? std::string("(tone only)") : oneLine(m.text), ImGui::GetContentRegionAvail().x).c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::PagerTelemetry& t = a.rx.pager;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.sync ? "reading a transmission" : t.signal ? "carrier, no sync" : "searching");
    kv("last sync", "%s", t.lastSpeed >= 0 ? dect2::pagerSpeedName(t.lastSpeed) : "none yet");
    kv("code words", "%llu good, %llu lost", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("channel", "%.0f dB SNR, %+.0f Hz off", t.snrDb, t.cfoHz);
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::PagerTelemetry& t = a.rx.pager;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signal", !on ? 0 : t.signal ? 1 : 0); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : t.sync ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.sync ? "Receiving" : t.state == 1 ? "Receiving" : "Searching");
    snprintf(b, sizeof b, "%llu", (unsigned long long)t.messagesTotal); ro("Pages", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Good / lost", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Pagers";
    if (live(a)) l2 = dect2::pagerSummary(a.rx.pager);
}

} // namespace

extern const ModeUi kPagerUi;
const ModeUi kPagerUi = {
    .sideTitle = "MESSAGES",
    .tabName = "Messages",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
};
