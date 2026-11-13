// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Channelised analog voice screens (CB, FRS / GMRS, PMR446, ham FM repeaters, HFGCS / HF voice, MDC-1200, wireless mic finder): one UI for the seven
// families 340 - 346 (engine standards 342 - 348, active standards 341 - 347). A channel plan that follows the location setting (or a manual choice),
// the busy-channel list with the CTCSS / DCS tone heard on each channel, a squelch, a multi-channel scan (a narrow radio, HF: the scan retunes the
// radio), recording of every transmission, the call log, the MDC-1200 unit IDs and the wireless microphone candidates. Receive only (docs/CHANNELS-3.8.md).
#include "app.h"
#include "ui/ui_common.h"
#include "dect2/chanvoice/chv_plan.h"
#include "dect2/chanvoice/chv_rx.h"
#include "dect2/chanvoice/chv_tel.h"
#include "dect2/common/timecompat.h"
#include "i18n/i18n.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace chv = dect2::chv;

namespace {

constexpr int kFirstActive = 341;   // family 340 + 1

struct SvcState {
    int regionMan = -1;               // -1: follow the location setting
    int demod = -1;                   // -1: the plan's choice (CB: automatic)
    int squelch = 50;
    bool deemph = true, scan = false, rec = false, onlyActive = false;
    float dwell = 0.4f, hang = 2.0f;
    std::string scanList;             // labels, "" = every channel of the plan
    std::string listen;               // the label that plays ("" = automatic)
    std::string sel;
    char recDir[400] = "";
    int micBand = -1;
};
struct State {
    bool loaded = false;
    SvcState s[chv::kServiceCount];
    bool dirty = true, wasRunning = false;
    float pushedVol = -1; bool pushedMute = false;
    int view = 0;
    int pushedSvc = -1;
    double lastTuneReq = 0;
    int micListen = 0;
};
State S;

int svcIndex(const App& a) { return std::max(0, std::min(chv::kServiceCount - 1, a.rx.standard - kFirstActive)); }
bool live(const App& a) { return a.rx.standard >= kFirstActive && a.rx.standard < kFirstActive + chv::kServiceCount; }
// the service of the screen: the receiver's while it runs, else the selected mode's family
chv::Service curSvc(const App& a) {
    int f = a.family;
    if (f >= chv::kFamilyBase && f < chv::kFamilyBase + chv::kServiceCount) return (chv::Service)(f - chv::kFamilyBase);
    return live(a) ? (chv::Service)svcIndex(a) : chv::Service::Cb;
}
SvcState& st(chv::Service s) { return S.s[(int)s]; }

const char* kSvcKey[] = {"cb", "frs", "pmr", "ham", "hf", "mdc", "mic"};
std::string pk(chv::Service s, const char* k) { return std::string("chv.") + kSvcKey[(int)s] + "." + k; }

void save(chv::Service sv) {
    plat::Prefs& d = plat::prefs();
    const SvcState& x = st(sv);
    d.setI(pk(sv, "region").c_str(), x.regionMan); d.setI(pk(sv, "demod").c_str(), x.demod); d.setI(pk(sv, "squelch").c_str(), x.squelch);
    d.setB(pk(sv, "deemph").c_str(), x.deemph); d.setB(pk(sv, "scan").c_str(), x.scan); d.setB(pk(sv, "rec").c_str(), x.rec); d.setB(pk(sv, "onlyActive").c_str(), x.onlyActive);
    d.setD(pk(sv, "dwell").c_str(), x.dwell); d.setD(pk(sv, "hang").c_str(), x.hang);
    d.setI(pk(sv, "micband").c_str(), x.micBand); d.setS(pk(sv, "scanList").c_str(), x.scanList); d.setS(pk(sv, "recDir").c_str(), x.recDir); d.setS(pk(sv, "listen").c_str(), x.listen);
    d.flush();
    S.dirty = true;
}
void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    for (int i = 0; i < chv::kServiceCount; i++) {
        const chv::Service sv = (chv::Service)i;
        SvcState& x = S.s[i];
        x.regionMan = (int)d.getI(pk(sv, "region").c_str(), -1); x.demod = (int)d.getI(pk(sv, "demod").c_str(), -1); x.squelch = (int)d.getI(pk(sv, "squelch").c_str(), 50);
        x.deemph = d.getB(pk(sv, "deemph").c_str(), true); x.scan = d.getB(pk(sv, "scan").c_str(), false); x.rec = d.getB(pk(sv, "rec").c_str(), false); x.onlyActive = d.getB(pk(sv, "onlyActive").c_str(), false);
        x.dwell = (float)d.getD(pk(sv, "dwell").c_str(), sv == chv::Service::HfVoice ? 1.5 : 0.4); x.hang = (float)d.getD(pk(sv, "hang").c_str(), 2.0);
        x.micBand = (int)d.getI(pk(sv, "micband").c_str(), -1); x.scanList = d.getS(pk(sv, "scanList").c_str(), ""); x.listen = d.getS(pk(sv, "listen").c_str(), "");
        const std::string dir = d.getS(pk(sv, "recDir").c_str(), recordingsSub("audio-recordings") + "/" + chv::serviceId(sv));
        snprintf(x.recDir, sizeof x.recDir, "%s", dir.c_str());
    }
}

// The region of the plan: the manual choice, else the setup country, else the receiver position, else the first plan
int autoRegion(const App& a, chv::Service sv) {
    if (a.setupCountry[0]) { const int r = chv::regionForCountry(sv, a.setupCountry); if (r >= 0) return r; }
    if (receiverLocationSet()) { const int r = chv::regionForPosition(sv, plat::prefs().getD("adsbLat", 0), plat::prefs().getD("adsbLon", 0)); if (r >= 0) return r; }
    return 0;
}
int region(const App& a, chv::Service sv) {
    const int n = (int)chv::regions(sv).size();
    const int r = st(sv).regionMan >= 0 ? st(sv).regionMan : autoRegion(a, sv);
    return std::max(0, std::min(n - 1, r));
}

std::vector<int> scanKeys(const App& a, chv::Service sv) {
    std::vector<int> v;
    std::stringstream ss(st(sv).scanList);
    std::string tok;
    while (std::getline(ss, tok, ',')) { const chv::Channel* c = chv::findLabel(sv, region(a, sv), tok); if (c) v.push_back((int)(c - &chv::channels(sv, region(a, sv))[0])); }
    return v;
}
bool inScanList(const App& a, chv::Service sv, const chv::Channel& c) {
    if (st(sv).scanList.empty()) return true;
    for (int k : scanKeys(a, sv)) if (&chv::channels(sv, region(a, sv))[(size_t)k] == &c) return true;
    return false;
}
void setScanMember(const App& a, chv::Service sv, const chv::Channel& c, bool on) {
    const auto& all = chv::channels(sv, region(a, sv));
    std::vector<std::string> labels;
    if (st(sv).scanList.empty()) { for (const auto& q : all) labels.push_back(q.label); }
    else for (int k : scanKeys(a, sv)) labels.push_back(all[(size_t)k].label);
    labels.erase(std::remove(labels.begin(), labels.end(), c.label), labels.end());
    if (on) labels.push_back(c.label);
    std::string s;
    for (auto& l : labels) { if (!s.empty()) s += ","; s += l; }
    st(sv).scanList = s.empty() ? std::string("-") : s;   // "-": nothing chosen (empty means all)
    save(sv);
}

bool covered(const dect2::ChvTelemetry& t, double mhz) {
    if (t.centerHz <= 0 || t.inputRate <= 0) return true;
    return std::fabs(mhz * 1e6 - t.centerHz) < t.inputRate * 0.42;
}
void tuneDialTo(App& a, double mhz) {
    a.freqMhz = mhz;
    a.tune.centerHz = a.freqMhz * 1e6;
    if (a.engine.running()) { a.engine.retuneReset(a.tune); a.peak.clear(); }
    savePrefs(a);
}

const chv::Channel* selectedChannel(const App& a, chv::Service sv) {
    const std::string& l = st(sv).sel.empty() ? st(sv).listen : st(sv).sel;
    return l.empty() ? nullptr : chv::findLabel(sv, region(a, sv), l);
}
void pick(App& a, chv::Service sv, const chv::Channel& c) {
    st(sv).sel = c.label; st(sv).listen = c.label;
    if (live(a) && !covered(a.rx.chanvoice, c.mhz)) tuneDialTo(a, c.mhz);
    save(sv);
}

std::string timeText(int64_t wall) {
    if (!wall) return "";
    struct tm tmv {};
    dect2::localTime((time_t)wall, &tmv);
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}
std::string agoText(double s) {
    char b[32];
    if (s < 0) return "";
    if (s < 90) snprintf(b, sizeof b, "%.0f s ago", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min ago", s / 60);
    else snprintf(b, sizeof b, "%.1f h ago", s / 3600);
    return b;
}
std::string toneText(const dect2::chv::SubTone& t) { return chv::toneLabel(t.ctcssHz, t.dcsCode, t.dcsInverted); }

// ---------------------------------------------------------------- the controls

void controls(App& a, chv::Service sv) {
    SvcState& x = st(sv);
    const auto& regs = chv::regions(sv);
    const int rg = region(a, sv);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Plan")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(260 * gUi);
    char cur[160]; snprintf(cur, sizeof cur, "%s%s", x.regionMan < 0 ? tr("Auto: ") : "", tr(regs[(size_t)rg].name));
    if (ImGui::BeginCombo("##chvreg", cur)) {
        if (ImGui::Selectable(tr("Auto: follow the location setting"), x.regionMan < 0)) { x.regionMan = -1; x.sel.clear(); x.listen.clear(); save(sv); }
        for (size_t i = 0; i < regs.size(); i++) if (ImGui::Selectable(tr(regs[i].name), x.regionMan == (int)i)) { x.regionMan = (int)i; x.sel.clear(); x.listen.clear(); save(sv); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The channel plan of the service for your region. Automatic takes it from the country or position set in the setup; the channel numbers and frequencies differ between regions."));
    if (sv == chv::Service::Mic) {
        // the band to search: UHF as before, and the VHF microphone bands (173.8 - 175.0 MHz and others); picking one tunes the radio to it
        flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Band")); ImGui::SameLine(0, 5 * gUi);
        const auto& mb = chv::micBandsAll(rg);
        const bool have = x.micBand >= 0 && x.micBand < (int)mb.size();
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::BeginCombo("##chvmicband", have ? tr(mb[(size_t)x.micBand].name) : tr("Tune the radio yourself"))) {
            if (ImGui::Selectable(tr("Tune the radio yourself"), !have)) { x.micBand = -1; a.engine.chanVoice().setMicBand(-1); save(sv); }
            for (size_t i = 0; i < mb.size(); i++) {
                if (ImGui::Selectable(tr(mb[i].name), x.micBand == (int)i)) {
                    x.micBand = (int)i; a.engine.chanVoice().setMicBand(x.micBand);
                    tuneDialTo(a, chv::micBandTuneMhz(mb[i]));
                    save(sv);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(mb[i].note));
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Tunes the radio to the middle of the band (or the bottom 8 MHz of a wide one). The VHF bands use 25 kHz channels; the UHF bands use 100 kHz cells. Move the radio with the frequency box to look at another part of a wide band."));
    }
    if (sv != chv::Service::Mic && sv != chv::Service::Mdc) {
        flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Channel")); ImGui::SameLine(0, 5 * gUi);
        const auto& chs = chv::channels(sv, rg);
        const chv::Channel* c = selectedChannel(a, sv);
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::BeginCombo("##chvch", c ? c->name.c_str() : tr("Automatic: the strongest open channel"), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable(tr("Automatic: the strongest open channel"), !c)) { x.sel.clear(); x.listen.clear(); save(sv); }
            for (const chv::Channel& q : chs) {
                char b[220]; snprintf(b, sizeof b, "%-9s %s   %.4f MHz", q.label.c_str(), q.name.c_str(), q.mhz);
                if (ImGui::Selectable(b, c == &q)) pick(a, sv, q);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pick a channel. With a wide radio every channel of the span is followed at once and this chooses the one you hear."));
    }
    if (sv == chv::Service::Cb) {
        flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Mode")); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(130 * gUi);
        const char* names[] = {"Automatic", "FM", "AM", "USB", "LSB"};
        const int idx = x.demod < 0 ? 0 : x.demod == (int)chv::Demod::Nfm ? 1 : x.demod == (int)chv::Demod::Am ? 2 : x.demod == (int)chv::Demod::Usb ? 3 : 4;
        if (ImGui::BeginCombo("##chvdm", tr(names[idx]))) {
            const int vals[] = {-1, (int)chv::Demod::Nfm, (int)chv::Demod::Am, (int)chv::Demod::Usb, (int)chv::Demod::Lsb};
            for (int i = 0; i < 5; i++) if (ImGui::Selectable(tr(names[i]), i == idx)) { x.demod = vals[i]; save(sv); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Automatic decides from the signal on each transmission: a constant envelope is FM, a carrier with a changing envelope is AM, no carrier is single sideband (USB or LSB from the side the energy is on). Pick one to force it."));
    }
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Squelch")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::SliderInt("##chvsq", &x.squelch, 0, 100, "%d %%");
    if (ImGui::IsItemDeactivatedAfterEdit()) save(sv);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("0 opens at the weakest signal, higher wants a cleaner one. FM channels open on the drop of the noise, AM and single sideband channels on the carrier finder's threshold."));
    if (sv != chv::Service::HfVoice && sv != chv::Service::Mic) {
        flowNext(10 * gUi);
        if (ImGui::Checkbox(tr("De-emphasis##chvde"), &x.deemph)) save(sv);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("FM radios send pre-emphasised audio (6 dB per octave, 750 us); this takes it out. Off gives flat audio."));
    }
    if (sv != chv::Service::Mic) {
        flowNext(10 * gUi);
        if (ImGui::Checkbox(tr("Scan##chvscan"), &x.scan)) save(sv);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sv == chv::Service::HfVoice ? tr("Steps the radio through the chosen frequencies and stays on one while it is on the air.") : tr("Plays the first of the chosen channels that opens, stays on it until it has been quiet for the hang time, then goes on."));
        if (x.scan) {
            ImGui::SameLine(0, 5 * gUi);
            ImGui::SetNextItemWidth(90 * gUi);
            ImGui::SliderFloat("##chvdw", &x.dwell, 0.1f, 5.f, "dwell %.1f s");
            if (ImGui::IsItemDeactivatedAfterEdit()) save(sv);
            ImGui::SameLine(0, 5 * gUi);
            ImGui::SetNextItemWidth(90 * gUi);
            ImGui::SliderFloat("##chvhg", &x.hang, 0.f, 6.f, "hang %.1f s");
            if (ImGui::IsItemDeactivatedAfterEdit()) save(sv);
            ImGui::SameLine(0, 5 * gUi);
            if (ImGui::Button(tr("Channels...##chvsl"))) ImGui::OpenPopup("##chvscanpop");
            if (ImGui::BeginPopup("##chvscanpop")) {
                if (ImGui::SmallButton(tr("All"))) { x.scanList.clear(); save(sv); }
                ImGui::SameLine(); if (ImGui::SmallButton(tr("None"))) { x.scanList = "-"; save(sv); }
                ImGui::Separator();
                ImGui::BeginChild("##chvsc", ImVec2(420 * gUi, 260 * gUi));
                for (const chv::Channel& q : chv::channels(sv, rg)) {
                    bool on = inScanList(a, sv, q);
                    char b[200]; snprintf(b, sizeof b, "%s  %s##chvsm%s", q.label.c_str(), q.name.c_str(), q.label.c_str());
                    if (ImGui::Checkbox(b, &on)) setScanMember(a, sv, q, on);
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }
        flowNext(10 * gUi);
        if (ImGui::Checkbox(tr("Record##chvrec"), &x.rec)) save(sv);
        ImGui::SameLine(); kit::recordingsButton("chv", "audio-recordings");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("One WAV file per transmission, for every channel of the span: time, channel and frequency in the name."));
        if (x.rec) {
            ImGui::SameLine(0, 5 * gUi);
            ImGui::SetNextItemWidth(230 * gUi);
            ImGui::InputText("##chvdir", x.recDir, sizeof x.recDir);
            if (ImGui::IsItemDeactivatedAfterEdit()) save(sv);
            ImGui::SameLine(0, 3 * gUi);
            if (ImGui::SmallButton("...##chvdb")) { const std::string p = plat::openFolderDialog(); if (!p.empty()) { snprintf(x.recDir, sizeof x.recDir, "%s", p.c_str()); save(sv); } }
            if (live(a)) { ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("%d %s", a.rx.chanvoice.recordedFiles, tr("saved")); }
        }
    }
    flowEnd();
}

void detail(App& a, chv::Service sv) {
    if (sv == chv::Service::Mdc) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("MDC-1200 is the 1200 bit/s data burst of Motorola radios that sends the unit ID when the key is pressed or released. Any FM carrier in the span is followed; the ID appears in the list below. FleetSync (Kenwood) is not decoded: its format is not publicly specified."));
        ImGui::PopTextWrapPos();
        return;
    }
    if (sv == chv::Service::Mic) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Finds wireless microphone transmitters: wide FM carriers (about 40 to 200 kHz) in the UHF bands of your region, and narrow FM ones (25 kHz channels) in the VHF bands 169 - 171, 173.8 - 175.0 and 174 - 216 MHz. Pick a band to tune the radio to it. Click a candidate to listen. Many systems compress the audio (companding), which sounds harsh and breathing without the matching expander; digital systems show as wide carriers that are not FM."));
        ImGui::PopTextWrapPos();
        return;
    }
    const chv::Channel* c = selectedChannel(a, sv);
    if (!c) { ImGui::TextDisabled("%s", tr("Automatic: the strongest open channel plays. Click a channel in the list to listen to it.")); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("%s  %s", c->label.c_str(), c->name.c_str());
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(c->note.c_str()));
    ImGui::PopTextWrapPos();
    if (c->duplex()) ImGui::Text("%s %.4f MHz, %s %.4f MHz (%s %+.3f MHz)", tr("Listen on"), c->mhz, tr("input"), c->txMhz, tr("offset"), c->txMhz - c->mhz);
    else ImGui::Text("%.4f MHz, %s", c->mhz, chv::demodName(c->demod));
    if (!c->minutes.empty()) {
        std::string m;
        for (int v : c->minutes) { char b[16]; snprintf(b, sizeof b, "%s%02d", m.empty() ? "" : ", ", v); m += b; }
        ImGui::TextDisabled("%s %s %s", tr("On the air at minutes"), m.c_str(), tr("past the hour (5 minute slots, UTC)"));
    }
}

// ---------------------------------------------------------------- the lists

void activity(App& a, chv::Service sv) {
    SvcState& x = st(sv);
    const bool on = live(a) && a.rx.chanvoice.service == (int)sv;
    const dect2::ChvTelemetry* t = on ? &a.rx.chanvoice : nullptr;
    if (ImGui::Checkbox(tr("Only channels that are active or heard##chvoa"), &x.onlyActive)) save(sv);
    ImGui::SameLine(0, 10 * gUi);
    if (t) ImGui::TextDisabled("%d %s, %llu %s", t->carriers, tr("carriers followed"), (unsigned long long)t->callsTotal, tr("calls"));
    else ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver: a wide radio follows every channel at once"));
    if (!ImGui::BeginTable("##chvact", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(160.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", 0, 18 * gUi); ImGui::TableSetupColumn(tr("Ch")); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn(tr("Use"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("State")); ImGui::TableSetupColumn(tr("Tone")); ImGui::TableSetupColumn(tr("Calls")); ImGui::TableSetupColumn(tr("Total")); ImGui::TableSetupColumn(tr("Last heard"));
    ImGui::TableHeadersRow();
    const auto& chs = chv::channels(sv, region(a, sv));
    const chv::Channel* sel = selectedChannel(a, sv);
    auto rowOf = [&](int key) -> const dect2::ChvChannelRow* { if (!t) return nullptr; for (const auto& r : t->rows) if (r.key == key) return &r; return nullptr; };
    auto draw = [&](const dect2::ChvChannelRow* r, const chv::Channel* c, int idx) {
        if (x.onlyActive && !(r && (r->active || r->open || r->calls))) return;
        ImGui::TableNextRow();
        ImGui::PushID(idx);
        ImGui::TableNextColumn();
        if (r && r->playing) ImGui::TextColored(pal::okGreen(), ">"); else ImGui::TextUnformatted(" ");
        ImGui::TableNextColumn();
        ImGui::PushFont(a.mono, 0);
        const bool pri = c && c->has(chv::kEmergency);
        if (pri) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        const std::string lab = c ? c->label : (r ? r->label : std::string("?"));
        if (ImGui::Selectable(lab.c_str(), c && sel == c, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) && c) pick(a, sv, *c);
        if (pri) ImGui::PopStyleColor();
        ImGui::PopFont();
        if (c && ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", c->name.c_str(), tr(c->note.c_str()));
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%.4f", c ? c->mhz : (r ? r->mhz : 0.0)); ImGui::PopFont();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(c ? c->name.c_str() : tr("off the plan"));
        if (c && c->duplex()) { ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("(+%.3f)", c->txMhz - c->mhz); }
        if (c && !c->minutes.empty() && r && !r->onAir) { ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("%s", tr("not on the air now")); }
        ImGui::TableNextColumn();
        if (r && (r->active || r->open) && r->levelDb > -150) {
            const float frac = std::max(0.f, std::min(1.f, ((float)r->levelDb + 70.f) / 60.f));
            char ov[24]; snprintf(ov, sizeof ov, "%.0f dB", r->levelDb);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, r->open ? pal::accent() : pal::grey(0.45f));
            ImGui::ProgressBar(frac, ImVec2(80 * gUi, 0), ov);
            ImGui::PopStyleColor();
        } else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (r && r->open) { ImGui::TextColored(pal::okGreen(), "%s", tr("OPEN")); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("%s", chv::demodName((chv::Demod)r->demod)); }
        else if (r && r->active) ImGui::TextDisabled("%s", tr("carrier"));
        if (r && r->inScan && x.scan) { ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("%s", tr("scan")); }
        ImGui::TableNextColumn();
        if (r) {
            const std::string tn = toneText(r->tone);
            if (!tn.empty()) ImGui::TextColored(pal::okGreen(), "%s", tn.c_str());
            else { const std::string lt = toneText(r->lastTone); if (!lt.empty()) ImGui::TextDisabled("%s", lt.c_str()); }
        }
        ImGui::TableNextColumn(); if (r && r->calls) ImGui::Text("%d", r->calls);
        ImGui::TableNextColumn(); if (r && r->calls) ImGui::Text("%.0f s", r->totalSec);
        ImGui::TableNextColumn();
        if (r && r->open) ImGui::TextColored(pal::okGreen(), "%s", tr("now"));
        else if (r && r->calls) ImGui::TextDisabled("%s", agoText(r->lastHeardAgoSec).c_str());
        ImGui::PopID();
    };
    for (size_t i = 0; i < chs.size(); i++) draw(rowOf((int)i), &chs[i], (int)i);
    if (t) for (const auto& r : t->rows) if (r.key >= (int)chs.size() || (sv == chv::Service::Mdc && r.key >= 0)) draw(&r, nullptr, 1000 + r.key);
    ImGui::EndTable();
}

void callLog(App& a, chv::Service sv) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver")); return; }
    const dect2::ChvTelemetry& t = a.rx.chanvoice;
    if (t.calls.empty()) { ImGui::TextDisabled("%s", tr("no call yet")); return; }
    if (!ImGui::BeginTable("##chvcalls", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(160.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Ch")); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn(tr("Mode")); ImGui::TableSetupColumn(tr("Tone"));
    ImGui::TableSetupColumn(tr("Length")); ImGui::TableSetupColumn(tr("Peak")); ImGui::TableSetupColumn(tr("Recording"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const dect2::ChvCall& c : t.calls) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(timeText(c.wallTime).c_str());
        ImGui::TableNextColumn();
        ImGui::PushID((int)c.serial);
        if (ImGui::Selectable(c.label.empty() ? "?" : c.label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) { const chv::Channel* ch = chv::findLabel(sv, region(a, sv), c.label); if (ch) pick(a, sv, *ch); }
        ImGui::PopID();
        ImGui::TableNextColumn(); ImGui::Text("%.4f", c.mhz);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(chv::demodName((chv::Demod)c.demod));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(toneText(c.tone).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", c.durationSec);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", c.peakDb);
        ImGui::TableNextColumn();
        if (!c.file.empty()) { const std::string f = std::filesystem::path(c.file).filename().string(); ImGui::TextDisabled("%s", f.c_str()); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.file.c_str()); }
    }
    ImGui::EndTable();
}

void mdcList(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver")); return; }
    const dect2::ChvTelemetry& t = a.rx.chanvoice;
    if (t.mdc.empty()) { ImGui::TextDisabled("%s", tr("no MDC-1200 burst heard yet")); return; }
    if (!ImGui::BeginTable("##chvmdc", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(160.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn(tr("Unit ID")); ImGui::TableSetupColumn("Op"); ImGui::TableSetupColumn(tr("Argument")); ImGui::TableSetupColumn(tr("Meaning"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const dect2::ChvMdcEvent& e : t.mdc) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(timeText(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.4f", e.mhz);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%04X", e.unitId); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%02X", e.op);
        ImGui::TableNextColumn(); ImGui::Text("%02X", e.arg);
        ImGui::TableNextColumn();
        if (e.dbl) ImGui::Text("%s  [%02X %02X %02X %02X]", e.text.c_str(), e.extra[0], e.extra[1], e.extra[2], e.extra[3]); else ImGui::TextUnformatted(e.text.c_str());
    }
    ImGui::EndTable();
}

void micList(App& a) {
    if (!live(a) || a.rx.chanvoice.service != (int)chv::Service::Mic) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver with a wide radio (10 Msps or more)")); return; }
    const dect2::ChvTelemetry& t = a.rx.chanvoice;
    if (t.mics.empty()) { ImGui::TextDisabled("%s", tr("no candidate yet: tune the radio to the middle of a microphone band and switch the microphone on")); return; }
    if (!ImGui::BeginTable("##chvmic", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(160.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn(tr("Width")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn(tr("Deviation")); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("State")); ImGui::TableSetupColumn(tr("Band"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const dect2::ChvMicRow& m : t.mics) {
        ImGui::TableNextRow();
        ImGui::PushID(m.id);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0);
        char f[24]; snprintf(f, sizeof f, "%.3f", m.mhz);
        if (ImGui::Selectable(f, m.listening, ImGuiSelectableFlags_SpanAllColumns)) { S.micListen = m.listening ? 0 : m.id; a.engine.chanVoice().setMicListen(S.micListen); }
        ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%.0f kHz", m.widthKhz);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", m.levelDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", m.snrDb);
        ImGui::TableNextColumn(); if (m.devKhz > 0) ImGui::Text("%.0f kHz", m.devKhz);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(m.fmLike ? tr("analog FM") : tr("not FM (digital?)"));
        ImGui::TableNextColumn();
        if (m.listening) ImGui::TextColored(pal::okGreen(), "%s", tr("listening")); else if (m.active) ImGui::TextUnformatted(tr("on")); else ImGui::TextDisabled("%s", tr("off"));
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", m.band.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- the engine side

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; S.pushedSvc = -1; return; }
    const chv::Service sv = curSvc(a);
    dect2::ChanVoiceReceiver& r = a.engine.chanVoice();
    const bool lv = live(a);
    SvcState& x = st(sv);
    // HF: the scan retunes the radio
    if (lv && x.scan && a.rx.chanvoice.singleTuner && a.rx.chanvoice.tuneRequestMhz > 0 && std::fabs(a.rx.chanvoice.tuneRequestMhz - S.lastTuneReq) > 1e-9 && !covered(a.rx.chanvoice, a.rx.chanvoice.tuneRequestMhz)) {
        S.lastTuneReq = a.rx.chanvoice.tuneRequestMhz;
        tuneDialTo(a, S.lastTuneReq);
    }
    if (!S.wasRunning || S.dirty || S.pushedSvc != (int)sv) {
        r.setService(sv);
        r.setRegion(region(a, sv));
        r.setSquelch(x.squelch); r.setDeemphasis(x.deemph); r.setDemodOverride(x.demod);
        r.setSingleTuner(sv == chv::Service::HfVoice);
        std::vector<int> keys;
        if (x.scanList == "-") keys.push_back(-1); else keys = scanKeys(a, sv);
        r.setScanKeys(keys); r.setScanTiming(x.dwell + (sv == chv::Service::HfVoice ? 0.5 : 0.0), x.hang); r.setScan(x.scan);
        const chv::Channel* l = x.listen.empty() ? nullptr : chv::findLabel(sv, region(a, sv), x.listen);
        r.listen(l ? (int)(l - &chv::channels(sv, region(a, sv))[0]) : -1);
        r.setRecording(x.rec, x.recDir);
        S.dirty = false; S.pushedSvc = (int)sv;
    }
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    load();
    const chv::Service sv = curSvc(a);
    controls(a, sv);
    ImGui::Separator();
    detail(a, sv);
    ImGui::Separator();
    if (sv == chv::Service::Mic) { micList(a); return; }
    if (sv == chv::Service::Mdc) {
        subNav("chvv", S.view, {tr("Carriers"), tr("MDC-1200 IDs"), tr("Call log")});
        if (S.view == 0) activity(a, sv); else if (S.view == 1) mdcList(a); else callLog(a, sv);
        return;
    }
    subNav("chvv", S.view, {tr("Channels"), tr("Call log")});
    if (S.view == 0 || S.view > 1) activity(a, sv); else callLog(a, sv);
}

void list(App& a) {
    load();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::ChvTelemetry& t = a.rx.chanvoice;
    if (t.service == (int)chv::Service::Mic) {
        for (const auto& m : t.mics) ImGui::Text("%.3f MHz  %s", m.mhz, m.fmLike ? "FM" : "?");
        return;
    }
    if (!ImGui::BeginTable("##chvl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Ch")); ImGui::TableSetupColumn(tr("State"), ImGuiTableColumnFlags_WidthStretch);
    for (const auto& r : t.rows) {
        if (!(r.active || r.open || r.calls)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(r.label.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        const std::string tn = toneText(r.tone);
        if (r.open) ImGui::TextColored(pal::okGreen(), "%s %s", tr("OPEN"), tn.c_str());
        else if (r.active) ImGui::TextDisabled("%s", tr("carrier"));
        else ImGui::TextDisabled("%d %s", r.calls, tr("calls"));
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::ChvTelemetry& t = a.rx.chanvoice;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    int open = 0, act = 0;
    for (const auto& r : t.rows) { open += r.open; act += r.active; }
    kv(tr("state"), "%s", dect2::chvSummary(t).c_str());
    kv(tr("plan"), "%s", tr(chv::regions((chv::Service)t.service)[(size_t)std::max(0, std::min((int)chv::regions((chv::Service)t.service).size() - 1, t.region))].name));
    kv(tr("channels"), "%d %s, %d %s, %d %s", t.carriers, tr("carriers"), act, tr("active"), open, tr("open"));
    kv(tr("plays"), "%s", t.playingLabel.empty() ? tr("nothing") : t.playingLabel.c_str());
    kv(tr("calls"), "%llu, %d %s", (unsigned long long)t.callsTotal, t.recordedFiles, tr("recorded"));
    if ((chv::Service)t.service == chv::Service::Mdc) kv("MDC-1200", "%llu", (unsigned long long)t.mdcTotal);
    kv(tr("radio centre"), "%.4f MHz, %.3f Msps", t.centerHz / 1e6, t.inputRate / 1e6);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool on = live(a);
    StatusPanel panel;
    statusIqLamp(a);
    lamp(tr("Open"), !on ? 0 : a.rx.chanvoice.state == 2 ? 1 : 0); flowNext(10 * gUi);
    statusState(a, on, [&] { return dect2::chvSummary(a.rx.chanvoice); });
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = chv::serviceName(curSvc(a));
    if (live(a)) l2 = dect2::chvSummary(a.rx.chanvoice);
}

} // namespace

extern const ModeUi kChanVoiceUi;
const ModeUi kChanVoiceUi = {
    .sideTitle = "ACTIVE",
    .tabName = "Channels",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
