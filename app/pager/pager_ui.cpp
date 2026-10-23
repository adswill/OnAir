// Pagers screens: lamps for the signal, the sync and the two protocols; the message list with a filter; the statistics per speed.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "pager_filter.h"
#include "../ui/msglog_ui.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/pager/pager_tel.h"
#include "platform/platform.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 messages, 1 statistics
    char filter[48] = "";                // matches the address or the text
    char watch[256] = "";                // the capcode watch list (#269), saved as pagerWatch
    dect2::PagerWatch watched;           // watch, parsed
    bool watchOnly = false;              // the Messages view shows only watched addresses (saved as pagerWatchOnly)
    bool pocsag = true, flex = true;
    bool decodedOnly = false;            // hide tone-only and empty pages from the table, Copy and the list (saved as pagerDecodedOnly)
    bool baud512 = true, baud1200 = true, baud2400 = true;
    int pushedBaud = -1;
    int pushedNarrow = -1;
    bool loaded = false;
    int readMode = dect2::kPocsagReadAuto;   // PocsagReadMode, saved as "pocsagRead"
    std::map<uint32_t, int> byAddress;       // POCSAG address -> kPocsagReadAlpha or kPocsagReadNumeric, saved as "pocsagReadAddr" ("ric:a ric:n ...")
};
State S;

int baudMask() {
    int m = 0;
    if (S.baud512) m |= 1;
    if (S.baud1200) m |= 2;
    if (S.baud2400) m |= 4;
    return m;
}

void loadRead() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    const long m = d.getI("pocsagRead", dect2::kPocsagReadAuto);
    S.readMode = m >= 0 && m < dect2::kPocsagReadModes ? (int)m : dect2::kPocsagReadAuto;
    S.decodedOnly = d.getI("pagerDecodedOnly", 0) != 0;
    snprintf(S.watch, sizeof S.watch, "%s", d.getS("pagerWatch", "").c_str());
    S.watched = dect2::pagerParseWatch(S.watch);
    S.watchOnly = d.getI("pagerWatchOnly", 0) != 0;
    const long b = d.getI("pocsagBaud", 7);
    S.baud512 = (b & 1) != 0;
    S.baud1200 = (b & 2) != 0;
    S.baud2400 = (b & 4) != 0;
    std::istringstream in(d.getS("pocsagReadAddr", ""));
    std::string tok;
    while (in >> tok) {
        const size_t c = tok.find(':');
        if (c == std::string::npos || c + 1 >= tok.size()) continue;
        const unsigned long ric = std::strtoul(tok.c_str(), nullptr, 10);
        S.byAddress[(uint32_t)ric] = tok[c + 1] == 'a' ? dect2::kPocsagReadAlpha : dect2::kPocsagReadNumeric;
    }
}

void saveBaud(const App& a) {
    plat::prefs().setI("pocsagBaud", baudMask());
    savePrefs(a);
}

void saveRead(const App& a) {
    std::string v;
    for (const auto& [ric, mode] : S.byAddress) v += std::to_string(ric) + (mode == dect2::kPocsagReadAlpha ? ":a " : ":n ");
    plat::prefs().setI("pocsagRead", S.readMode);
    plat::prefs().setS("pocsagReadAddr", v);
    savePrefs(a);
}

// The page as the user wants it read: the "Message type" setting, or the address override. FLEX pages are left alone.
dect2::PagerMessage resolved(const dect2::PagerMessage& m) {
    dect2::PagerMessage v = m;
    if (m.flex || m.numText.empty() && m.alphaText.empty()) return v;
    const auto it = S.byAddress.find(m.address);
    if (it != S.byAddress.end()) dect2::pocsagApplyRead(v, it->second, true);
    else dect2::pocsagApplyRead(v, S.readMode);
    return v;
}

std::string typeText(const dect2::PagerMessage& m) {
    std::string t = dect2::pagerTypeName(m.type);
    if (!m.flex && (m.type == dect2::kPagerAlpha || m.type == dect2::kPagerNumeric)) t += std::string(" (") + dect2::pocsagReadHowName(m.readHow) + ")";
    return t;
}

constexpr size_t kShown = 400;           // rows drawn at most: the list wraps its text, so every row has its own height

bool live(const App& a) { return a.rx.standard == 24; }   // the engine reports its standard code minus one

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

std::string oneLine(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '\n' || c == '\r' || (unsigned char)c < 32) ? ' ' : c;
    return o;
}

bool passes(const dect2::PagerMessage& m) {   // m: resolved
    if (m.flex ? !S.flex : !S.pocsag) return false;
    if (S.watchOnly && !S.watched.empty() && !S.watched.has(m.address)) return false;
    if (!S.filter[0]) return true;
    char adr[16];
    snprintf(adr, sizeof adr, "%u", (unsigned)m.address);
    const std::string f = lower(S.filter);
    return std::string(adr).find(f) != std::string::npos || lower(m.text).find(f) != std::string::npos;
}

// passes() plus the "Decoded only" switch; hidden (when given) is incremented for a page that only that switch removed.
bool shows(const dect2::PagerMessage& m, size_t* hidden = nullptr) {
    if (!passes(m)) return false;
    if (S.decodedOnly && !dect2::pagerIsDecoded(m)) { if (hidden) (*hidden)++; return false; }
    return true;
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
    const SubMode* sm = curSub(a);   // a protocol entry (POCSAG or FLEX) decodes only its own
    const int m = sm ? sm->decoders : 3;
    if (m & 1) { lamp("POCSAG", !on ? 0 : t.timeSec - t.lastPocsagSec < 60 ? 1 : t.lastPocsagSec > 0 ? 2 : 0); flowNext(12 * gUi); }
    if (m & 2) lamp("FLEX", !on ? 0 : t.timeSec - t.lastFlexSec < 60 ? 1 : t.lastFlexSec > 0 ? 2 : 0);
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
    ImGui::BeginDisabled(S.watched.empty());
    if (ImGui::Checkbox(tr("Watched only"), &S.watchOnly)) { plat::prefs().setI("pagerWatchOnly", S.watchOnly ? 1 : 0); plat::prefs().flush(); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Show only the capcodes of the watch list (set it in the Capcodes view)"));
    flowNext(12 * gUi);
    ImGui::Checkbox("POCSAG", &S.pocsag);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("POCSAG is the ITU-R M.584 / CCIR Radiopaging Code No. 1 (RPC1): the same signal, so CCIR paging on the pager bands is decoded here at 512, 1200 and 2400 baud. The CCIR 5-tone selective calls are in the Tones mode."));
    flowNext(10 * gUi);
    ImGui::Checkbox("FLEX", &S.flex);
    flowNext(12 * gUi);
    if (ImGui::Checkbox(tr("Decoded only"), &S.decodedOnly)) { plat::prefs().setI("pagerDecodedOnly", S.decodedOnly ? 1 : 0); savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hides tone-only pages and pages whose text could not be decoded."));
    flowNext(12 * gUi);
    ImGui::TextDisabled("%s", tr("Channel")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::BeginCombo("##pgbw", a.pagerNarrow ? tr("12.5 kHz") : tr("25 kHz"))) {
        if (ImGui::Selectable(tr("25 kHz"), !a.pagerNarrow)) { a.pagerNarrow = false; savePrefs(a); }
        if (ImGui::Selectable(tr("12.5 kHz"), a.pagerNarrow)) { a.pagerNarrow = true; savePrefs(a); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Receive filter width. 12.5 kHz cuts a neighbouring channel better but needs the signal on the channel centre; 25 kHz is the wider, more forgiving filter."));
    flowNext(12 * gUi);
    if (S.pocsag) {
        ImGui::Checkbox("512 bd", &S.baud512);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decode POCSAG at 512 baud.");
        flowNext(10 * gUi);
        ImGui::Checkbox("1200 bd", &S.baud1200);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decode POCSAG at 1200 baud.");
        flowNext(10 * gUi);
        ImGui::Checkbox("2400 bd", &S.baud2400);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decode POCSAG at 2400 baud. Turn off when only a slower speed is on air to avoid false decodes.");
        if (ImGui::IsItemDeactivatedAfterEdit()) { saveBaud(a); S.pushedBaud = -1; }
        flowNext(12 * gUi);
        ImGui::TextDisabled("Message type"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(150 * gUi);
        if (ImGui::BeginCombo("##pgread", dect2::pocsagReadModeName(S.readMode))) {
            static const char* const tips[] = {
                "Read each POCSAG message both ways and show it as text when it looks like text, else as digits. Many networks send text with function 0.",
                "Function 0 is numeric, 1 to 3 alphanumeric, as the POCSAG standard says.",
                "Read every POCSAG message as text.",
                "Read every POCSAG message as digits."};
            for (int i = 0; i < dect2::kPocsagReadModes; i++) {
                if (ImGui::Selectable(dect2::pocsagReadModeName(i), S.readMode == i)) { S.readMode = i; saveRead(a); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tips[i]);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How POCSAG messages are read: as text (alphanumeric) or digits. Right-click an address to fix it for that address.");
        flowNext(12 * gUi);
    }
    const bool wide = wideActive(a);   // whole span: a Frequency column (the carrier each page came from)
    const bool copy = ImGui::Button("Copy");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the pages that match the filter to the clipboard, one per line.");
    flowEnd();
    if (copy) {
        std::string txt;
        for (const auto& raw : all) {
            const dect2::PagerMessage m = resolved(raw);
            if (!shows(m)) continue;
            if (wide && m.carrierId > 0) { char fb[24]; snprintf(fb, sizeof fb, "%.4f\t", radioCenterMhz(a) + m.carrierHz / 1e6); txt += fb; }
            txt += clock(m.wallTime) + "\t" + dect2::pagerSpeedName(m.speed) + "\t" + std::to_string(m.address) + "\t" + fnText(m) + "\t" + typeText(m) + "\t" + oneLine(m.text) + "\n";
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    {   // Save... / Auto-save to file: the pages that match the filter, oldest first
        std::vector<dect2::msglog::Row> rows;
        for (const auto& raw : all) {
            const dect2::PagerMessage m = resolved(raw);
            if (!shows(m)) continue;
            dect2::msglog::Row r;
            r.epoch = m.wallTime;
            r.mode = dect2::pagerSpeedName(m.speed);
            r.freqHz = (m.carrierId > 0 ? radioCenterMhz(a) + m.carrierHz / 1e6 : a.freqMhz) * 1e6;
            r.address = std::to_string(m.address);
            r.text = m.type == dect2::kPagerTone ? std::string("(tone only)") : m.text;
            rows.push_back(std::move(r));
        }
        std::stable_sort(rows.begin(), rows.end(), [](const dect2::msglog::Row& x, const dect2::msglog::Row& y) { return x.epoch < y.epoch; });
        msglog_ui::bar("pager", "pager", rows);
    }
    if (ImGui::BeginTable("##pgmsg", 6 + (wide ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        if (wide) ImGui::TableSetupColumn("Frequency MHz");
        ImGui::TableSetupColumn(dect2::timefmt::zone() == dect2::timefmt::Zone::Local ? "Time (local)" : "Time UTC"); ImGui::TableSetupColumn("Protocol"); ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Fn");
        ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        size_t shown = 0, matching = 0, hiddenTones = 0;
        if (on) {
            for (const auto& raw : all) {
                const dect2::PagerMessage m = resolved(raw);
                if (!shows(m, &hiddenTones)) continue;
                matching++;
                if (shown >= kShown) continue;
                shown++;
                ImGui::TableNextRow();
                if (wide) { ImGui::TableNextColumn(); wideCarrierCell(a, m.carrierId, m.carrierHz); }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(m.wallTime).c_str());
                ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, pal::accent()); ImGui::TextUnformatted(dect2::pagerSpeedName(m.speed)); ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::PushID((int)(m.serial * 31u + (unsigned)(int)std::lround(m.carrierHz / 100.0)));   // serials restart per carrier
                ImGui::Text("%u", (unsigned)m.address);
                if (!m.flex && ImGui::BeginPopupContextItem("##pgadr", ImGuiPopupFlags_MouseButtonRight)) {
                    const auto it = S.byAddress.find(m.address);
                    const int cur = it == S.byAddress.end() ? -1 : it->second;
                    ImGui::TextDisabled("Address %u", (unsigned)m.address);
                    if (ImGui::MenuItem("Always alphanumeric", nullptr, cur == dect2::kPocsagReadAlpha)) { S.byAddress[m.address] = dect2::kPocsagReadAlpha; saveRead(a); }
                    if (ImGui::MenuItem("Always numeric", nullptr, cur == dect2::kPocsagReadNumeric)) { S.byAddress[m.address] = dect2::kPocsagReadNumeric; saveRead(a); }
                    if (ImGui::MenuItem("Auto (the Message type setting)", nullptr, cur < 0)) { S.byAddress.erase(m.address); saveRead(a); }
                    ImGui::EndPopup();
                }
                if (!m.flex && ImGui::IsItemHovered()) ImGui::SetTooltip("Right-click to choose how pages to this address are read.");
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fnText(m).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(typeText(m).c_str());
                ImGui::TableNextColumn();
                ImGui::PushFont(a.mono, 0);
                if (m.type == dect2::kPagerTone) ImGui::TextDisabled("(tone only)");
                else ImGui::TextWrapped("%s%s", m.text.c_str(), m.damaged ? "  [incomplete]" : "");
                ImGui::PopFont();
                if (!m.flex && !m.numText.empty() && ImGui::IsItemHovered()) {
                    static const char* const why[] = {"by the function code", "auto: the text reading %s", "forced by the Message type setting", "set for this address"};
                    char reason[96];
                    snprintf(reason, sizeof reason, why[m.readHow], m.looksAlpha ? "looks like text" : "does not look like text");
                    ImGui::SetTooltip("Shown as %s, %s\n\nAlphanumeric: %s\nNumeric: %s", dect2::pagerTypeName(m.type), reason, oneLine(m.alphaText).c_str(), m.numText.c_str());
                }
            }
        }
        ImGui::EndTable();
        if (on && matching && hiddenTones) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(tr("%zu tone-only or undecoded pages hidden"), hiddenTones); ImGui::PopTextWrapPos(); }
        if (!on) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see pages"); ImGui::PopTextWrapPos(); }
        else if (!matching && hiddenTones) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(tr("%zu tone-only or undecoded pages hidden (Decoded only)"), hiddenTones); ImGui::PopTextWrapPos(); }
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

// Every address heard in the page list, busiest first. A click on a row puts the address into the message filter.
void capcodeView(App& a) {
    const dect2::PagerTelemetry& t = a.rx.pager;
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the capcodes"); ImGui::PopTextWrapPos(); return; }
    const std::vector<dect2::PagerHeard> rows = dect2::pagerHeardTable(messagesOf(t));
    const bool wide = wideActive(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%zu addresses heard in the last %zu pages. Click an address to filter the Messages view by it.", rows.size(), messagesOf(t).size());
    if (!wide && modeSupportsWide(a.family))
        ImGui::TextDisabled("%s", tr("Unknown frequency? Turn on Whole span (top right of the spectrum): every pager carrier in the band the radio captures is decoded, and the MHz column shows where each capcode was heard."));
    ImGui::PopTextWrapPos();
    // the watch list (#269): capcodes to keep an eye on; their rows are marked here and the Messages view can show only them
    auto saveWatch = [] { S.watched = dect2::pagerParseWatch(S.watch); plat::prefs().setS("pagerWatch", S.watch); plat::prefs().flush(); };
    ImGui::TextDisabled("%s", tr("Watch list")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputTextWithHint("##pgwatch", "1234 5678 2000-2099", S.watch, sizeof S.watch)) saveWatch();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Capcodes (RICs) to watch, separated by spaces or commas; a range like 2000-2099. Watched rows are marked; Messages > Watched only shows just them."));
    auto toggle = [&](uint32_t adr) {
        std::string s = S.watch;
        if (S.watched.has(adr)) {   // remove the exact number (a range that holds it stays)
            const std::string n = std::to_string(adr);
            std::string out, tok;
            std::istringstream in(s);
            while (in >> tok) if (tok != n && tok != n + ",") out += (out.empty() ? "" : " ") + tok;
            s = out;
        } else s += (s.empty() ? "" : " ") + std::to_string(adr);
        snprintf(S.watch, sizeof S.watch, "%s", s.c_str());
        saveWatch();
    };
    if (!ImGui::BeginTable("##pgcap", 6 + (wide ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Watch"); ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Pages"); ImGui::TableSetupColumn("Last"); ImGui::TableSetupColumn("Speed");
    if (wide) ImGui::TableSetupColumn("MHz");
    ImGui::TableSetupColumn("Last text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < rows.size() && i < 500; i++) {
        const dect2::PagerHeard& r = rows[i];
        ImGui::TableNextRow();
        const bool w = S.watched.has(r.address);
        if (w) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(pal::accent(0.25f)));
        ImGui::TableNextColumn();
        bool wv = w;
        char wid[24]; snprintf(wid, sizeof wid, "##pgw%zu", i);
        if (ImGui::Checkbox(wid, &wv)) toggle(r.address);
        ImGui::TableNextColumn();
        char lab[32];
        snprintf(lab, sizeof lab, "%u##cap%zu", (unsigned)r.address, i);
        if (ImGui::Selectable(lab, false, ImGuiSelectableFlags_None)) { snprintf(S.filter, sizeof S.filter, "%u", (unsigned)r.address); S.view = 0; }
        ImGui::TableNextColumn(); ImGui::Text("%d", r.count);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(r.lastWall).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::pagerSpeedName(r.speed));
        if (wide) { ImGui::TableNextColumn(); wideCarrierCell(a, r.carrierId, r.carrierHz); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(oneLine(r.lastText), ImGui::GetContentRegionAvail().x).c_str());
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    loadRead();
    const int m = baudMask();
    if ((int)a.pagerNarrow != S.pushedNarrow) { a.engine.pager().setNarrowChannel(a.pagerNarrow); S.pushedNarrow = a.pagerNarrow; }
    if (m != S.pushedBaud) { a.engine.pager().setPocsagBauds(m); S.pushedBaud = m; }
}

void tab(App& a) {
    loadRead();
    lampRow(a);
    ImGui::Spacing();
    subNav("pagerv", S.view, {"Messages", "Capcodes", "Statistics"});
    if (S.view == 0) messageView(a); else if (S.view == 1) capcodeView(a); else statsView(a);
}

void list(App& a) {
    loadRead();
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see pages"); ImGui::PopTextWrapPos(); return; }
    if (resultsClearButton("pager")) a.engine.clearResults(25);   // also the whole-span receivers
    const dect2::PagerTelemetry& t = a.rx.pager;
    const std::vector<dect2::PagerMessage>& all = messagesOf(t);
    ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%llu pages", (unsigned long long)t.messagesTotal); ImGui::PopTextWrapPos();
    const bool wide = wideActive(a);
    if (!ImGui::BeginTable("##pgl_s", 2 + (wide ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wide) ImGui::TableSetupColumn("MHz");
    ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    size_t n = 0;
    for (const auto& raw : all) {
        if (n >= 100) break;
        const dect2::PagerMessage m = resolved(raw);
        if (S.decodedOnly && !dect2::pagerIsDecoded(m)) continue;
        n++;
        ImGui::TableNextRow();
        if (wide) { ImGui::TableNextColumn(); wideCarrierCell(a, m.carrierId, m.carrierHz); }
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)m.address);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(m.type == dect2::kPagerTone ? std::string("(tone only)") : oneLine(m.text), ImGui::GetContentRegionAvail().x).c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    wideCarriersTable(a, 0);
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
    loadRead();
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
    snprintf(b, sizeof b, "%llu", (unsigned long long)t.messagesTotal);
    if (S.decodedOnly) {
        size_t tones = 0;
        for (const auto& raw : messagesOf(t)) if (!dect2::pagerIsDecoded(resolved(raw))) tones++;
        if (tones) { char hb[48]; snprintf(hb, sizeof hb, "%s, %zu %s", b, tones, tr("hidden")); ro("Pages", hb); }
        else ro("Pages", b);
    } else ro("Pages", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Good / lost", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const SubMode* sm = curSub(a);
    l1 = sm ? sm->name : "Pagers";
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
    .tick = tick,
};
