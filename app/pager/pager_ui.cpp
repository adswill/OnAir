// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers screens: lamps for the signal, the sync and the two protocols; the message list with a filter; the statistics per speed.
#include "../d40/d40_ui.h"
#include "dect2/common/diag.h"
#include "dect2/common/time_fmt.h"
#include "ui/kit/mode_panel.h"
#include "ui/kit/session_list.h"
#include "ui/kit/mode_frame.h"
#include "app.h"
#include "dect2/ease/ease.h"
#include "dect2/channels/freq_db.h"
#include <memory>
#include "pager_filter.h"
#include "pager_alias.h"
#include "pager_keywords.h"
#include "../panels/automation_internal.h"
#include "../ui/msglog_ui.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/pager/pager_tel.h"
#include "platform/platform.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <cmath>
#include <deque>
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
    char ignore[256] = "";               // capcodes to hide from the Messages view (saved as pagerIgnore)
    dect2::PagerWatch ignored;           // ignore, parsed
    bool showIgnored = false;            // show the ignored pages after all (nothing is lost silently)
    size_t ignoredHidden = 0;            // pages the ignore list hid in the last pass over the table
    bool watchOnly = false;              // the Messages view shows only watched addresses (saved as pagerWatchOnly)
    bool pocsag = true, flex = true;
    bool both = false;                   // decode POCSAG and FLEX together on this channel, from either mode entry (saved as pagerBoth)
    bool decodedOnly = false;            // hide tone-only and empty pages from the table, Copy and the list (saved as pagerDecodedOnly)
    bool hideAuto = false;               // hide the pages of capcodes the insight analyser found automatic or test pages (pagerHideAuto; insight/pager_patterns.h)
    bool baud512 = true, baud1200 = true, baud2400 = true;
    int pushedBaud = -1;
    int pushedNarrow = -1;
    bool loaded = false;
    int readMode = dect2::kPocsagReadAuto;   // PocsagReadMode, saved as "pocsagRead"
    int charset = dect2::kPagerCharAscii;    // PagerCharset, saved as "pagerCharset"
    dect2::PagerAliases aliases;             // capcode names (#5), kept in pager_aliases.csv in the data folder
    std::string aliasNote;                   // the result of the last import / export
    char nameEdit[128] = "";                 // the name being typed in the address menu
    bool aliasDirty = false;                 // names edited in the table, written to the file when the field is left
    bool combine = true;                     // multi-copy combining (#20), saved as pagerCombine
    int pushedCombine = -1;
    bool smartRepair = true, smartInsight = true, smartMedical = false;   // the smart layer (pager_intel.h), saved as pagerSmartRepair / pagerSmartInsight / pagerSmartMedical
    int pushedSmart = -1;
    bool pushedPlaces = false;
    bool selcall = false;                // also decode five-tone selective calls on the channel audio (saved as pagerSelcall)
    int pushedSelcall = -1;
    dect2::PagerKeywords kw;                 // keyword alerts: matched against the text and the capcode name, saved as pagerKeywords
    char kwText[2000] = "";                  // the list as typed, one keyword per line, "re:" starts a regular expression
    int kwBad = 0;                           // lines of the list that could not be used
    bool kwNotify = false, kwSound = false;  // desktop notification / beeps on a match (both off by default; saved as pagerKwNotify, pagerKwSound)
    struct KwHit { int64_t wall = 0; std::string kw, addr, text; };
    std::deque<KwHit> kwHits;                // the live ticker: the newest matches, newest last
    std::map<int, uint64_t> kwSeen;          // carrier -> highest serial looked at (serials restart per carrier)
    bool kwPrimed = false;                   // pages that were there when the list was set are not alerts
    std::map<uint32_t, int> byAddress;       // POCSAG address -> kPocsagReadAlpha or kPocsagReadNumeric, saved as "pocsagReadAddr" ("ric:a ric:n ...")
};
State S;
void saveKeywords();

int baudMask() {
    int m = 0;
    if (S.baud512) m |= 1;
    if (S.baud1200) m |= 2;
    if (S.baud2400) m |= 4;
    return m;
}

std::string aliasPath() { return plat::dataDir() + "/pager_aliases.csv"; }

void saveAliases() {
    std::ofstream f(aliasPath(), std::ios::binary | std::ios::trunc);
    if (f) f << S.aliases.exportCsv();
}

// The name of a capcode for the lists ("" = none); the category is in the tooltip
std::string aliasOf(const dect2::PagerMessage& m) { const dect2::PagerAlias* a = S.aliases.find(m.address, m.flex); return a ? a->name : std::string(); }

void loadRead() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    const long m = d.getI("pocsagRead", dect2::kPocsagReadAuto);
    S.readMode = m >= 0 && m < dect2::kPocsagReadModes ? (int)m : dect2::kPocsagReadAuto;
    { const long cs = d.getI("pagerCharset", 0); S.charset = cs >= 0 && cs < dect2::kPagerCharsets ? (int)cs : 0; }
    S.decodedOnly = d.getI("pagerDecodedOnly", 0) != 0;
    S.hideAuto = d.getI("pagerHideAuto", 0) != 0;
    S.combine = d.getI("pagerCombine", 1) != 0;
    S.smartRepair = d.getI("pagerSmartRepair", 1) != 0;
    S.smartInsight = d.getI("pagerSmartInsight", 1) != 0;
    S.smartMedical = d.getI("pagerSmartMedical", 0) != 0;
    S.selcall = d.getI("pagerSelcall", 0) != 0;
    S.both = d.getI("pagerBoth", 0) != 0;
    {
        std::string k = d.getS("pagerKeywords", "");
        for (auto& c : k) if (c == '\x1e') c = '\n';
        snprintf(S.kwText, sizeof S.kwText, "%s", k.c_str());
        S.kwBad = S.kw.parse(S.kwText);
        S.kwNotify = d.getI("pagerKwNotify", 0) != 0;
        S.kwSound = d.getI("pagerKwSound", 0) != 0;
    }
    {
        std::ifstream f(aliasPath(), std::ios::binary);
        if (f) { std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); S.aliases.importCsv(t); }
    }
    snprintf(S.watch, sizeof S.watch, "%s", d.getS("pagerWatch", "").c_str());
    S.watched = dect2::pagerParseWatch(S.watch);
    S.watchOnly = d.getI("pagerWatchOnly", 0) != 0;
    snprintf(S.ignore, sizeof S.ignore, "%s", d.getS("pagerIgnore", "").c_str());
    S.ignored = dect2::pagerParseWatch(S.ignore);
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
    plat::prefs().setI("pagerCharset", S.charset);
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
    if (v.type == dect2::kPagerAlpha) v.text = dect2::pagerApplyCharset(v.text, S.charset);
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
    if (m.selcall) return !S.filter[0] || lower(m.text + " " + m.selStd).find(lower(S.filter)) != std::string::npos;   // no address: the ignore and watch lists do not apply
    if (m.flex ? !S.flex : !S.pocsag) return false;
    if (!S.ignored.empty() && S.ignored.has(m.address) && !S.showIgnored) { S.ignoredHidden++; return false; }
    if (S.watchOnly && !S.watched.empty() && !S.watched.has(m.address)) return false;
    if (!S.filter[0]) return true;
    char adr[16];
    snprintf(adr, sizeof adr, "%u", (unsigned)m.address);
    const std::string f = lower(S.filter);
    if (std::string(adr).find(f) != std::string::npos || lower(m.text).find(f) != std::string::npos) return true;
    if (const dect2::PagerAlias* al = S.aliases.find(m.address, m.flex)) return lower(al->name).find(f) != std::string::npos || lower(al->category).find(f) != std::string::npos;
    return false;
}

// passes() plus the "Decoded only" switch; hidden (when given) is incremented for a page that only that switch removed.
bool shows(const dect2::PagerMessage& m, size_t* hidden = nullptr) {
    if (!passes(m)) return false;
    if (S.decodedOnly && !dect2::pagerIsDecoded(m)) { if (hidden) (*hidden)++; return false; }
    if (S.hideAuto && !m.selcall && insightPagerAutomatic(m.flex ? "FLEX" : "POCSAG", m.address)) { if (hidden) (*hidden)++; return false; }
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
    const int m = S.both || !sm ? 3 : sm->decoders;
    if (m & 1) { lamp("POCSAG", !on ? 0 : t.timeSec - t.lastPocsagSec < 60 ? 1 : t.lastPocsagSec > 0 ? 2 : 0); flowNext(12 * gUi); }
    if (m & 2) lamp("FLEX", !on ? 0 : t.timeSec - t.lastFlexSec < 60 ? 1 : t.lastFlexSec > 0 ? 2 : 0);
    flowEnd();
}

// ---------------------------------------------------------------- the views

void messageView(App& a) {
    const dect2::PagerTelemetry& t = a.rx.pager;
    const bool on = live(a);
    const std::vector<dect2::PagerMessage>& all = messagesOf(t);
    if (!S.kw.empty()) {   // the live keyword ticker: the last three matches
        if (S.kwHits.empty()) ImGui::TextDisabled("%s", tr("Keywords on: no match yet"));
        const size_t from = S.kwHits.size() > 3 ? S.kwHits.size() - 3 : 0;
        for (size_t i = from; i < S.kwHits.size(); i++) {
            const auto& h = S.kwHits[i];
            ImGui::TextColored(pal::warnAmber(), "%s", h.kw.c_str()); ImGui::SameLine(0, 8 * gUi);
            ImGui::TextDisabled("%s", clock(h.wall).c_str()); ImGui::SameLine(0, 8 * gUi);
            ImGui::TextUnformatted((h.addr + "  " + h.text.substr(0, 120)).c_str());
        }
    }
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputText("##pgf", S.filter, sizeof S.filter);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show only pages whose address, name or text contains this."));
    flowNext(12 * gUi);
    ImGui::BeginDisabled(S.watched.empty());
    if (ImGui::Checkbox(tr("Watched only"), &S.watchOnly)) { plat::prefs().setI("pagerWatchOnly", S.watchOnly ? 1 : 0); plat::prefs().flush(); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Show only the capcodes of the watch list (set it in the Capcodes view)"));
    flowNext(12 * gUi);
    if (ImGui::Checkbox(tr("POCSAG + FLEX (both)"), &S.both)) { plat::prefs().setI("pagerBoth", S.both ? 1 : 0); savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Decode POCSAG and FLEX at the same time on this channel, whichever mode entry is selected. They share one demodulator; only the channel you are tuned to is heard, so use it when a network or neighbouring channels carry both."));
    flowNext(12 * gUi);
    ImGui::Checkbox("POCSAG", &S.pocsag);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("POCSAG is the ITU-R M.584 / CCIR Radiopaging Code No. 1 (RPC1): the same signal, so CCIR paging on the pager bands is decoded here at 512, 1200 and 2400 baud. The CCIR 5-tone selective calls are in the Tones mode."));
    flowNext(10 * gUi);
    ImGui::Checkbox("FLEX", &S.flex);
    flowNext(10 * gUi);
    if (ImGui::Checkbox(tr("Selective call tones (CCIR / ZVEI / EEA / EIA / DZVEI...)"), &S.selcall)) { plat::prefs().setI("pagerSelcall", S.selcall ? 1 : 0); savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Also decode five-tone sequential calls (CCIR, ZVEI1/2, EEA, EIA, DZVEI) on the same channel audio, alongside POCSAG and FLEX. They appear in the table with the standard in the Protocol column and the digits as the message."));
    flowNext(12 * gUi);
    if (ImGui::Checkbox(tr("Decoded only"), &S.decodedOnly)) { plat::prefs().setI("pagerDecodedOnly", S.decodedOnly ? 1 : 0); savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hides tone-only pages and pages whose text could not be decoded."));
    flowNext(12 * gUi);
    if (ImGui::Checkbox(tr("Hide automatic pages"), &S.hideAuto)) { plat::prefs().setI("pagerHideAuto", S.hideAuto ? 1 : 0); savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hides the capcodes that send the same text at a steady pace (test and keep-alive pages), as found in View > Insights > Pagers."));
    flowNext(12 * gUi);
    if (ImGui::Checkbox(tr("Combine repeats"), &S.combine)) { plat::prefs().setI("pagerCombine", S.combine ? 1 : 0); savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Networks send a page more than once. When a copy has code words the error correction cannot repair, the same words from an earlier copy of the same page are used, so the message comes out complete. Pages made this way are marked."));
    flowNext(12 * gUi);
    if (ImGui::Checkbox(tr("Repair text"), &S.smartRepair)) { plat::prefs().setI("pagerSmartRepair", S.smartRepair ? 1 : 0); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("When a code word is lost, the missing characters are filled in from words this address sent before, but only when one reading clearly beats the others. Guessed characters are shown in colour and listed in the tooltip; characters that cannot be guessed stay as ?."));
    flowNext(12 * gUi);
    if (ImGui::Checkbox(tr("Read positions and fields"), &S.smartInsight)) { plat::prefs().setI("pagerSmartInsight", S.smartInsight ? 1 : 0); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Finds coordinates (decimal, degrees and minutes, UTM, MGRS) and place names from pager_places.csv (name,lat,lon per line) in the data folder, names the parts each address always sends, and groups pages of several addresses about one event. All of it stays on this computer."));
    if (S.smartInsight) {
        flowNext(12 * gUi);
        if (ImGui::Checkbox(tr("Include medical pages"), &S.smartMedical)) { plat::prefs().setI("pagerSmartMedical", S.smartMedical ? 1 : 0); plat::prefs().flush(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Off by default for privacy: pages that look medical are not analysed, grouped or learned from."));
    }
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
        ImGui::TextDisabled("Characters"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(130 * gUi);
        if (ImGui::BeginCombo("##pgchars", dect2::pagerCharsetName(S.charset))) {
            for (int i = 0; i < dect2::kPagerCharsets; i++)
                if (ImGui::Selectable(dect2::pagerCharsetName(i), S.charset == i)) { S.charset = i; saveRead(a); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("German networks send ä ö ü ß and Ä Ö Ü in the place of [ ] { | } ~ \\ (DIN 66003). Choose German to show them as letters.");
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
            txt += clock(m.wallTime) + "\t" + dect2::pagerProtoName(m) + "\t" + std::to_string(m.address) + "\t" + aliasOf(m) + "\t" + fnText(m) + "\t" + typeText(m) + "\t" + oneLine(m.text) + "\n";
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
            r.mode = dect2::pagerProtoName(m);
            r.freqHz = (m.carrierId > 0 ? radioCenterMhz(a) + m.carrierHz / 1e6 : a.freqMhz) * 1e6;
            r.address = std::to_string(m.address);
            if (const std::string nm = aliasOf(m); !nm.empty()) r.address += " " + nm;
            r.text = m.type == dect2::kPagerTone ? std::string("(tone only)") : m.text;
            rows.push_back(std::move(r));
        }
        std::stable_sort(rows.begin(), rows.end(), [](const dect2::msglog::Row& x, const dect2::msglog::Row& y) { return x.epoch < y.epoch; });
        msglog_ui::bar("pager", "pager", rows);
    }
    if (ImGui::BeginTable("##pgmsg", 6 + (wide ? 1 : 0) + (S.aliases.empty() ? 0 : 1), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        if (wide) ImGui::TableSetupColumn("Frequency MHz");
        ImGui::TableSetupColumn(dect2::timefmt::zone() == dect2::timefmt::Zone::Local ? "Time (local)" : "Time UTC"); ImGui::TableSetupColumn("Protocol"); ImGui::TableSetupColumn("Address");
        if (!S.aliases.empty()) ImGui::TableSetupColumn(tr("Name"));
        ImGui::TableSetupColumn("Fn");
        ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        size_t shown = 0, matching = 0, hiddenTones = 0;
        S.ignoredHidden = 0;
        if (on) {
            for (const auto& raw : all) {
                const dect2::PagerMessage m = resolved(raw);
                if (!shows(m, &hiddenTones)) continue;
                matching++;
                if (shown >= kShown) continue;
                shown++;
                ImGui::TableNextRow();
                if (!S.kw.empty() && S.kw.match(m.text, aliasOf(m))) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(150, 110, 20, 90));   // a keyword match
                if (wide) { ImGui::TableNextColumn(); wideCarrierCell(a, m.carrierId, m.carrierHz); }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(m.wallTime).c_str());
                ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, pal::accent()); ImGui::TextUnformatted(dect2::pagerProtoName(m).c_str()); ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::PushID((int)(m.serial * 31u + (unsigned)(int)std::lround(m.carrierHz / 100.0)));   // serials restart per carrier
                if (m.selcall) ImGui::TextDisabled("-"); else ImGui::Text("%u", (unsigned)m.address);
                if (!m.selcall && ImGui::BeginPopupContextItem("##pgadr", ImGuiPopupFlags_MouseButtonRight)) {
                    ImGui::TextDisabled("Address %u", (unsigned)m.address);
                    {   // a name for this capcode (#5): Enter keeps it, empty removes it
                        if (ImGui::IsWindowAppearing()) { snprintf(S.nameEdit, sizeof S.nameEdit, "%s", aliasOf(m).c_str()); }
                        ImGui::SetNextItemWidth(180 * gUi);
                        if (ImGui::InputTextWithHint("##pgname", tr("Name this address"), S.nameEdit, sizeof S.nameEdit, ImGuiInputTextFlags_EnterReturnsTrue)) {
                            const dect2::PagerAlias* cur = S.aliases.find(m.address, m.flex);
                            dect2::PagerAlias n; n.first = n.last = m.address; n.proto = m.flex ? dect2::kAliasFlex : dect2::kAliasPocsag; n.name = S.nameEdit;
                            if (cur && cur->first == cur->last) { n.category = cur->category; if (cur->proto == dect2::kAliasAny) n.proto = dect2::kAliasAny; }
                            S.aliases.set(n); saveAliases();
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    if (!m.flex) {
                        const auto it = S.byAddress.find(m.address);
                        const int cur = it == S.byAddress.end() ? -1 : it->second;
                        if (ImGui::MenuItem("Always alphanumeric", nullptr, cur == dect2::kPocsagReadAlpha)) { S.byAddress[m.address] = dect2::kPocsagReadAlpha; saveRead(a); }
                        if (ImGui::MenuItem("Always numeric", nullptr, cur == dect2::kPocsagReadNumeric)) { S.byAddress[m.address] = dect2::kPocsagReadNumeric; saveRead(a); }
                        if (ImGui::MenuItem("Auto (the Message type setting)", nullptr, cur < 0)) { S.byAddress.erase(m.address); saveRead(a); }
                        ImGui::Separator();
                    }
                    if (ImGui::MenuItem(tr("Ignore this address"))) {
                        std::string ig = S.ignore;
                        ig += (ig.empty() ? "" : " ") + std::to_string(m.address);
                        snprintf(S.ignore, sizeof S.ignore, "%s", ig.c_str());
                        S.ignored = dect2::pagerParseWatch(S.ignore);
                        plat::prefs().setS("pagerIgnore", S.ignore); plat::prefs().flush();
                    }
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.flex ? tr("Right-click to ignore this address.") : tr("Right-click to choose how pages to this address are read, or to ignore it."));
                ImGui::PopID();
                if (!S.aliases.empty()) {
                    ImGui::TableNextColumn();
                    if (const dect2::PagerAlias* al = S.aliases.find(m.address, m.flex)) {
                        ImGui::TextUnformatted(al->name.c_str());
                        if (!al->category.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", al->category.c_str());
                    }
                }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fnText(m).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(typeText(m).c_str());
                ImGui::TableNextColumn();
                ImGui::PushFont(a.mono, 0);
                if (m.type == dect2::kPagerTone) ImGui::TextDisabled("(tone only)");
                else if (!m.repairedText.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, pal::accent());
                    ImGui::TextWrapped("%s  [repaired]", m.repairedText.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        std::string marked;
                        bool in = false;
                        for (size_t k = 0; k < m.repairedText.size(); k++) {
                            const bool g = k < m.guessMask.size() && m.guessMask[k] == 1;
                            if (g && !in) marked += '[';
                            if (!g && in) marked += ']';
                            in = g;
                            marked += m.repairedText[k];
                        }
                        if (in) marked += ']';
                        ImGui::SetTooltip(tr("Characters in [ ] were guessed from earlier pages of this address; ? could not be recovered.\n\n%s"), marked.c_str());
                    }
                }
                else ImGui::TextWrapped("%s%s%s", m.text.c_str(), m.damaged ? "  [incomplete]" : "", m.combined ? "  [combined]" : "");
                if (!m.fields.empty() || m.hasPos || m.incident) {
                    std::string info;
                    for (const dect2::PagerField& f : m.fields) info += f.key + ": " + f.value + "   ";
                    if (m.hasPos) { char b[96]; snprintf(b, sizeof b, "%.5f, %.5f (%s%s)", m.lat, m.lon, m.place.c_str(), m.posApprox ? ", approximate" : ""); info += b; }
                    if (!info.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", info.c_str()); ImGui::PopTextWrapPos(); }
                }
                if (m.combined && ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Repaired from %d copies of this page: code words lost in this copy were taken from an earlier one."), m.copies);
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
        if (on && S.ignoredHidden) {
            ImGui::TextDisabled(tr("%zu ignored pages hidden"), S.ignoredHidden);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Show ignored"))) S.showIgnored = true;
        } else if (on && S.showIgnored && !S.ignored.empty()) {
            ImGui::TextDisabled("%s", tr("Ignored pages are shown"));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Hide ignored"))) S.showIgnored = false;
        }
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
    for (size_t i = 0; i < t.incidents.size() && i < 8; i++) {
        const dect2::PagerIncident& n = t.incidents[i];
        kv(i == 0 ? tr("incidents") : "", "%d pages, %zu addresses: %.40s%s", n.count, n.capcodes.size(), n.title.c_str(), n.hasPos ? "  (position)" : "");
    }
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
    d40PagerGroupsCard();   // 4.0: capcode groups (app/d40)
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
    ImGui::TextDisabled("%s", tr("Ignore list")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputTextWithHint("##pgignore", "1234 5678 2000-2099", S.ignore, sizeof S.ignore)) {
        S.ignored = dect2::pagerParseWatch(S.ignore);
        plat::prefs().setS("pagerIgnore", S.ignore); plat::prefs().flush();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Capcodes (RICs) whose pages are hidden from Messages, separated by spaces or commas; a range like 2000-2099. Works for POCSAG and FLEX. The pages are still received."));
    {   // keyword alerts: one keyword per line, "re:" for a regular expression
        ImGui::TextDisabled("%s", tr("Keywords"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextMultiline("##pgkw", S.kwText, sizeof S.kwText, ImVec2(0, 70 * gUi))) saveKeywords();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("One keyword per line, any case. It is looked for in the message text and in the capcode name. Start a line with re: for a regular expression, for example re:structure fire|MVA. Matching pages are highlighted in Messages and shown in the ticker."));
        if (ImGui::Checkbox(tr("Notify on a keyword match"), &S.kwNotify)) saveKeywords();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A desktop notification for each matching new page (off by default)"));
        ImGui::SameLine(0, 12 * gUi);
        ImGui::BeginDisabled(!S.kwNotify);
        if (ImGui::Checkbox(tr("Beep"), &S.kwSound)) saveKeywords();
        ImGui::EndDisabled();
        if (S.kwBad) { ImGui::SameLine(0, 12 * gUi); ImGui::TextColored(pal::badRed(), "%d %s", S.kwBad, tr("lines are not valid and were skipped")); }
    }
    {   // capcode names (#5): a CSV with the columns capcode (or a range), name, category, protocol
        if (ImGui::Button(tr("Import names..."))) {
            const std::string p = openFileDialog();
            if (!p.empty()) {
                std::ifstream f(p, std::ios::binary);
                if (!f) { DIAG_DEBUG(650544, "The pager alias file %s could not be opened, so no aliases were imported.", p.c_str()); S.aliasNote = tr("Could not read the file"); }
                else {
                    const std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    const dect2::PagerAliasImport r = S.aliases.importCsv(t);
                    saveAliases();
                    S.aliasNote = std::to_string(r.added) + " " + tr("added") + ", " + std::to_string(r.replaced) + " " + tr("replaced") + ", " + std::to_string(r.skipped) + " " + tr("skipped");
                }
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Load capcode names from a CSV file: columns capcode (or a range like 1000-1099), name, category, protocol (POCSAG, FLEX or empty for both). Semicolon or tab separated files work too. Names with the same capcode and protocol are replaced, the rest stay."));
        ImGui::SameLine();
        if (ImGui::Button(tr("Export names..."))) {
            const std::string p = saveFileDialog("pager-capcodes.csv");
            if (!p.empty()) {
                std::ofstream f(p, std::ios::binary | std::ios::trunc);
                if (f) { f << S.aliases.exportCsv(); S.aliasNote = std::string(tr("Saved")) + ": " + p; } else { DIAG(383284, "Writing the pager alias file %s failed (the folder may be read-only), so the aliases were not exported.", p.c_str()); S.aliasNote = tr("Could not write the file"); }
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Save the capcode names as a CSV file (opens in a spreadsheet)."));
        ImGui::SameLine();
        ImGui::BeginDisabled(S.aliases.empty());
        if (ImGui::SmallButton(tr("Clear names"))) { S.aliases.clear(); saveAliases(); S.aliasNote.clear(); }
        ImGui::EndDisabled();
        if (!S.aliases.empty() || !S.aliasNote.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%zu %s%s%s", S.aliases.rows().size(), tr("names"), S.aliasNote.empty() ? "" : "  -  ", S.aliasNote.c_str()); }
    }
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
    if (!ImGui::BeginTable("##pgcap", 7 + (wide ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Watch"); ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn("Pages"); ImGui::TableSetupColumn("Last"); ImGui::TableSetupColumn("Speed");
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
        ImGui::TableNextColumn();
        {   // the name: typed here it becomes an entry for exactly this capcode and protocol
            const dect2::PagerAlias* al = S.aliases.find(r.address, r.flex);
            char buf[128];
            snprintf(buf, sizeof buf, "%s", al ? al->name.c_str() : "");
            char nid[24]; snprintf(nid, sizeof nid, "##pgn%zu", i);
            ImGui::SetNextItemWidth(150 * gUi);
            if (ImGui::InputTextWithHint(nid, tr("name"), buf, sizeof buf)) {
                dect2::PagerAlias n; n.first = n.last = r.address; n.proto = r.flex ? dect2::kAliasFlex : dect2::kAliasPocsag; n.name = buf;
                if (al && al->first == al->last) n.category = al->category;
                S.aliases.set(n);
                S.aliasDirty = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && S.aliasDirty) { saveAliases(); S.aliasDirty = false; }
        }
        ImGui::TableNextColumn(); ImGui::Text("%d", r.count);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(r.lastWall).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::pagerSpeedName(r.speed));
        if (wide) { ImGui::TableNextColumn(); wideCarrierCell(a, r.carrierId, r.carrierHz); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(oneLine(r.lastText), ImGui::GetContentRegionAvail().x).c_str());
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- hooks

void saveKeywords() {
    S.kwBad = S.kw.parse(S.kwText);
    std::string k = S.kwText;
    for (auto& c : k) if (c == '\n') c = '\x1e';
    plat::prefs().setS("pagerKeywords", k);
    plat::prefs().setI("pagerKwNotify", S.kwNotify ? 1 : 0);
    plat::prefs().setI("pagerKwSound", S.kwSound ? 1 : 0);
    plat::prefs().flush();
    S.kwPrimed = false;   // what is on the screen now is not new
}

// New pages that match a keyword: the ticker, and a notification when asked. At most three alerts a pass (a flood of pages is one more line).
void keywordScan(App& a) {
    if (S.kw.empty() || !live(a)) return;
    const std::vector<dect2::PagerMessage>& all = messagesOf(a.rx.pager);
    int alerts = 0, extra = 0;
    for (const auto& raw : all) {
        uint64_t& seen = S.kwSeen[raw.carrierId];
        if (raw.serial <= seen) continue;
        seen = raw.serial;
        if (!S.kwPrimed) continue;
        const dect2::PagerMessage m = resolved(raw);
        const std::string nm = aliasOf(m);
        const std::string* k = S.kw.match(m.text, nm);
        if (!k) continue;
        State::KwHit h; h.wall = m.wallTime; h.kw = *k; h.addr = std::to_string(m.address) + (nm.empty() ? "" : " " + nm); h.text = oneLine(m.text);
        S.kwHits.push_back(h);
        while (S.kwHits.size() > 50) S.kwHits.pop_front();
        if (S.kwNotify) {
            if (alerts < 3) { alerts++; automationapp::alertNow(a, std::string(tr("Pager keyword")) + ": " + h.kw, h.addr + "  " + h.text.substr(0, 160), S.kwSound && alerts == 1); }
            else extra++;
        }
    }
    S.kwPrimed = true;
    if (extra) automationapp::alertNow(a, tr("Pager keyword"), std::to_string(extra) + " " + tr("more matching pages"), false);
}

void tick(App& a) {
    loadRead();
    keywordScan(a);
    const int m = baudMask();
    {   // both protocols from either entry; otherwise the entry's own (a mode switch resets the engine to the entry)
        const SubMode* sm = curSub(a);
        const int want = S.both || !sm ? 3 : sm->decoders;
        if (a.engine.pager().protocols() != want) a.engine.pager().setProtocols(want);
    }
    if ((int)a.pagerNarrow != S.pushedNarrow) { a.engine.pager().setNarrowChannel(a.pagerNarrow); S.pushedNarrow = a.pagerNarrow; }
    if (m != S.pushedBaud) { a.engine.pager().setPocsagBauds(m); S.pushedBaud = m; }
    if ((int)S.selcall != S.pushedSelcall) { a.engine.pager().setSelcall(S.selcall); S.pushedSelcall = S.selcall; }
    if (S.pushedPlaces != a.engine.running()) {   // the place list is read each time the receiver starts
        S.pushedPlaces = a.engine.running();
        if (S.pushedPlaces) {
            std::ifstream f(plat::dataDir() + "/pager_places.csv");
            std::stringstream ss; ss << f.rdbuf();
            a.engine.pager().setPlaces(ss.str());
        }
    }
    {
        const int sm = (S.smartRepair ? 1 : 0) | (S.smartInsight ? 2 : 0) | (S.smartMedical ? 4 : 0);
        if (sm != S.pushedSmart) {
            a.engine.pager().setSmartRepair(S.smartRepair); a.engine.pager().setSmartInsight(S.smartInsight); a.engine.pager().setAnalyseMedical(S.smartMedical);
            S.pushedSmart = sm;
        }
    }
    if ((int)S.combine != S.pushedCombine) { a.engine.pager().setCombineCopies(S.combine); S.pushedCombine = S.combine; }
}

void tab(App& a) {
    loadRead();
    lampRow(a);
    ImGui::Spacing();
    kit::modeHeader({23, "Pagers", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    kit::modeNav("pagerv", S.view, {{tr("Live"), 0}, {tr("Threads"), 3}, {tr("Capcodes"), 1, true}, {tr("Statistics"), 2, true}});
    if (S.view == 0) messageView(a); else if (S.view == 1) capcodeView(a); else if (S.view == 3) kit::sessionItems(23, 2, "pager"); else statsView(a);
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

// "Find pagers": the known pager frequencies of the region in turn, 20 s each, until a page is decoded. Tuning by hand ends it.
struct FindPagers { std::unique_ptr<dect2::ease::FreqHunt> h; double lastMhz = 0, backMhz = 0; std::string note; double noDecodeSince = -1; uint64_t seen = 0; };
FindPagers F;

void findPagersRow(App& a) {
    uint64_t msgs = 0;
    for (const auto& m : messagesOf(a.rx.pager)) msgs = std::max(msgs, m.serial);   // the serial grows per message and never restarts
    if (!F.h) {
        if (ImGui::SmallButton(tr("Find pagers"))) {
            std::vector<double> mhz;
            const std::string c = plat::prefs().getS("locCountry", "");
            for (const auto& e : dect2::builtinFrequencies()) if ((e.mode == "pocsag" || e.mode == "flex") && dect2::ease::freqRegionApplies(e.region, c)) mhz.push_back((double)e.hz / 1e6);
            if (mhz.empty()) F.note = tr("No known pager frequencies for your region.");
            else {
                F.backMhz = a.freqMhz; F.lastMhz = 0; F.note.clear();
                F.h = std::make_unique<dect2::ease::FreqHunt>(mhz, 20.0);
                if (!a.engine.running()) startReceiver(a);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Listens 20 s on each known pager frequency of your region and stops where a page is decoded."));
        if (!F.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", F.note.c_str()); }
    } else {
        bool stop = ImGui::SmallButton(tr("Stop finding"));
        if (F.lastMhz > 0 && std::fabs(a.freqMhz - F.lastMhz) > 0.0001) { stop = true; F.note = tr("Stopped: you tuned by hand."); F.backMhz = 0; }
        else if (!a.engine.running() && F.lastMhz > 0) { stop = true; F.note = tr("Stopped: the receiver was stopped."); F.backMhz = 0; }
        if (!stop && a.engine.running()) {
            const int i = F.h->update(ImGui::GetTime(), msgs);
            const double m = F.h->mhz(i);
            if (F.h->found()) { F.note = tr("Found pages here."); F.h.reset(); F.backMhz = 0; }
            else if (F.h->finished()) { F.note = tr("No pages heard on the known frequencies."); F.h.reset(); stop = true; }
            else { if (std::fabs(a.freqMhz - m) > 0.0001) tuneFreq(a, m); F.lastMhz = a.freqMhz; ImGui::SameLine(); ImGui::TextDisabled(tr("Listening on %.4f MHz (%d of %d)."), m, i + 1, F.h->size()); }
        }
        if (stop) { F.h.reset(); if (F.backMhz > 0) tuneFreq(a, F.backMhz); F.backMhz = 0; }
    }
    // the first time a wide radio has heard nothing for half a minute: offer Whole span, once
    plat::Prefs& p = plat::prefs();
    const bool wideRadio = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio() && a.devices[a.devIdx].maxRateHz >= 8e6;
    if (msgs != F.seen || !live(a)) { F.seen = msgs; F.noDecodeSince = ImGui::GetTime(); }
    if (F.noDecodeSince < 0) F.noDecodeSince = ImGui::GetTime();
    if (wideRadio && !wideActive(a) && modeSupportsWide(a.family) && !F.h && !p.has("pagerWideOffered") && ImGui::GetTime() - F.noDecodeSince > 30.0 && msgs == 0) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Nothing heard yet. Your radio is wide enough to decode every pager carrier in its band at once."));
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton(tr("Turn on Whole span"))) { a.wideDecode = true; setWideFam(a, a.family, true); savePrefs(a); p.setB("pagerWideOffered", true); }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("No thanks##pgwide"))) p.setB("pagerWideOffered", true);
    }
}

void receiver(App& a) {
    findPagersRow(a);
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

// Simple view: the messages, the capcodes and the statistics side by side
static void command(App& a) {
    loadRead();
    lampRow(a);
    ImGui::Spacing();
    kit::modeHeader({23, "Pagers", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    modepanel::panes({{"##pgmsgs", tr("Messages"), [&a] { messageView(a); }, 1.8f, 260.f},
                      {"##pgcap", tr("Capcodes"), [&a] { capcodeView(a); }, 1.f, 220.f},
                      {"##pgstat", tr("Statistics"), [&a] { statsView(a); }, 0.8f, 200.f}}, 320.f);
}

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
    .command = command,
};
