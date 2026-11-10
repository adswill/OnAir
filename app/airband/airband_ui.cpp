// Airband screens: the channel list with live levels and squelch, adding and editing channels (validated to the 25 / 8.33 kHz raster),
// scan, squelch and hang, the activity log, and the band spectrum with a marker for every channel.
#include "app.h"
#include "transcribe/transcribe_app.h"
#include "ui/ui_common.h"
#include "gfx/plot.h"
#include "dect2/airband/airband_gen.h"
#include "dect2/airband/airband_mil.h"
#include "airband/airport_ui.h"
#include "dect2/airband/airband_store.h"
#include "dect2/airband/airband_tel.h"
#include "dect2/airband/airband_clips.h"
#include "state/recdir.h"
#include "voice/voice_clips_ui.h"
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    std::vector<dect2::AirbandChannel> chans;
    float squelch = 6, hang = 0.5f;
    bool nrOn = false; float nrStr = 80;   // neural noise remover
    bool scan = false, hold = false;
    bool onlyActive = false;             // channels list: only those open or heard in the last 30 s
    bool defaultList = false;            // the list is the built-in one (test signal) or empty, never saved by the user
    int lastKind = -1;
    bool dirty = true;                   // push the list and the settings to the receiver
    int view = 0;                        // 0 channels, 1 on the air, 2 activity, 3 spectrum
    char addName[24] = "", addLabel[32] = "";
    std::string addErr;
    int editing = -1;
    bool editFocus = false;              // the rename field takes the keyboard on its first frame
    char editLabel[32] = "";
    bool wasRunning = false;
    float pushedVol = -1; bool pushedMute = false;
    bool rec = false;                    // one WAV per transmission (prefs airRecEach)
    std::shared_ptr<dect2::AirbandClipWriter> clips;
    std::vector<dect2::voice::Clip> fresh;   // this run's clips with their channel label
    std::string recNote;
};
State S;

bool live(const App& a) { return a.rx.standard == 27; }   // the engine reports its standard code minus one

// the dial onto a channel outside the span taken in (the dial stays on the 25 kHz raster, where the receiver puts the DC spike between channels)
void tuneDialTo(App& a, double hz) {
    a.freqMhz = std::round(hz / 25e3) * 25e3 / 1e6;
    a.tune.centerHz = a.freqMhz * 1e6;
    if (a.engine.running()) { a.engine.retuneReset(a.tune); a.peak.clear(); }
    savePrefs(a);
}

void save() {
    plat::Prefs& d = plat::prefs();
    d.setS("airChans", dect2::airbandEncode(S.chans));
    d.setD("airSquelch", S.squelch); d.setD("airHang", S.hang); d.setB("airScan", S.scan);
    S.defaultList = false;
    d.setB("airHold", S.hold); d.setB("airOnlyActive", S.onlyActive);
    d.setB("airNr", S.nrOn); d.setD("airNrStr", S.nrStr);
    d.flush();
    S.dirty = true;
}

bool synthetic(const App& a) { return a.devIdx >= 0 && (size_t)a.devIdx < a.devices.size() && a.devices[(size_t)a.devIdx].kind == DeviceInfo::Synthetic; }

void load(const App& a) {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    if (d.has("airChans")) S.chans = dect2::airbandDecode(d.getS("airChans", ""));
    else {   // the test signal's channels only for the test signal: a real radio starts with an empty list (search an airport, or see what is on the air)
        S.defaultList = true;
        if (synthetic(a)) S.chans = dect2::airbandChannelsFor(dect2::airbandTestLayout(), dect2::airbandTuning().defMhz * 1e6);
    }
    S.hold = d.getB("airHold", false); S.onlyActive = d.getB("airOnlyActive", false);
    S.squelch = (float)d.getD("airSquelch", 6); S.hang = (float)d.getD("airHang", 0.5); S.scan = d.getB("airScan", false);
    S.nrOn = d.getB("airNr", false); S.nrStr = (float)std::clamp(d.getD("airNrStr", 80), 0.0, 100.0);
    S.rec = d.getB("airRecEach", false);
}

std::string clipFolder() { return recordingsDir() + "/airband"; }

void clipChannels() {
    std::vector<dect2::AirbandClipChannel> v;
    for (const auto& c : S.chans) v.push_back({c.freqHz, c.label.empty() ? dect2::airbandName(c.freqHz, c.is833) : c.label});
    if (S.clips) S.clips->setChannels(v);
}

void recordingsView(App& a) {
    if (ImGui::Checkbox(tr("Save each transmission"), &S.rec)) { plat::prefs().setB("airRecEach", S.rec); if (S.clips) S.clips->setEnabled(S.rec); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Every squelch opening of every channel becomes a short WAV named with the channel, the frequency and the UTC time. How many are kept, and for how many days, is set in the list below."));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", clipFolder().c_str());
    if (!S.recNote.empty()) ImGui::TextDisabled("%s", S.recNote.c_str());
    voiceclips::panel(a, "airband", "airclips", clipFolder(), "airband-", S.fresh);
}

const dect2::AirbandChannelState* stateOf(const App& a, size_t i) {
    if (!live(a) || i >= a.rx.airband.channels.size()) return nullptr;
    const auto& s = a.rx.airband.channels[i];
    return std::fabs(s.freqHz - S.chans[i].freqHz) < 1 ? &s : nullptr;
}

// ---------------------------------------------------------------- the controls

void controls(App& a) {
    if (ImGui::Checkbox("Scan", &S.scan)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play one channel at a time: stop on the first that opens (the priority channel first), go on 2 s after it closed.\nOff: every open channel is mixed; an open priority channel pushes the others 12 dB down.");
    if (S.scan) {
        flowNext(6 * gUi);
        if (ImGui::Checkbox(tr("Hold##airhold"), &S.hold)) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Stay on the channel the scan stopped on, also after it has gone quiet."));
    }
    flowNext(12 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Squelch"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::SliderFloat("##airsq", &S.squelch, -5.f, 30.f, "%.0f dB");
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Carrier to noise a channel needs to open. Lower opens on weaker stations; the noise floor is tracked per channel.");
    flowNext(12 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Hang"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::SliderFloat("##airhang", &S.hang, 0.f, 3.f, "%.1f s");
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
    flowNext(12 * gUi);
    if (ImGui::Checkbox(tr("Noise remover (neural)##airnr"), &S.nrOn)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Speech only; do not use on music or data modes."));
    if (S.nrOn) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        ImGui::SliderFloat("##airnrs", &S.nrStr, 0.f, 100.f, "%.0f%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Mix of cleaned and original audio: right removes more noise, left keeps more of the original. Adds about 20 ms of delay."));
    }
    flowNext(12 * gUi);
    if (live(a)) {
        const auto& t = a.rx.airband;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Radio off by"); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::Text("%+.2f kHz%s", t.cfoHz / 1e3, t.tuneKnown ? "" : " ?"); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio's tuning error, measured from the carriers of the listed channels and taken out of every channel.\n'?' until a carrier has been seen.");
    }
    flowEnd();
}

void applyChannels(const std::vector<dect2::AirbandChannel>& ch, bool replace);   // below

void addRow(App& a) {
    ImGui::SetNextItemWidth(90 * gUi);
    const bool enter = ImGui::InputTextWithHint("##airadd", "118.005", S.addName, sizeof S.addName, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channel name as dialled (118.025 is a 25 kHz channel; 118.030 / .035 / .040 are the 8.33 kHz channels of that block),\nor a frequency in MHz on the 8.33 kHz raster (118.0083).\nMilitary UHF air band: 225.000 - 399.975 MHz on the 25 kHz raster (243.000).");
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputTextWithHint("##airlab", "Tower", S.addLabel, sizeof S.addLabel);
    flowNext(6 * gUi);
    if (ImGui::Button("Add") || enter) {
        double f; bool n833;
        if (!dect2::airbandParse(S.addName, f, n833)) S.addErr = tr("not a channel: 118.000 - 136.990 on the 25 or 8.33 kHz raster, or 225.000 - 399.975 (military UHF) on the 25 kHz raster");
        else {
            bool dup = false;
            for (const auto& c : S.chans) dup |= std::fabs(c.freqHz - f) < 1 && c.is833 == n833;
            if (dup) S.addErr = "already in the list";
            else {
                dect2::AirbandChannel c; c.freqHz = f; c.is833 = n833; c.label = S.addLabel;
                S.chans.push_back(c);
                std::sort(S.chans.begin(), S.chans.end(), [](const auto& x, const auto& y) { return x.freqHz < y.freqHz; });
                S.editing = -1;   // the rows moved: a rename in progress would land on another channel
                S.addName[0] = S.addLabel[0] = 0; S.addErr.clear();
                save();
            }
        }
    }
    flowNext(12 * gUi);
    if (ImGui::Button("Test channels")) { S.chans = dect2::airbandChannelsFor(dect2::airbandTestLayout(), a.freqMhz * 1e6); S.editing = -1; save(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The channels of the built-in test signal around the frequency tuned now.");
    flowNext(6 * gUi);
    if (ImGui::Button(tr("Military UHF##airmil"))) {
        applyChannels(dect2::airbandMilUhfChannels(), false);
        if (!dect2::airbandIsMilUhf(a.freqMhz * 1e6)) tuneDialTo(a, 243.000e6);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Adds the published common channels of the military UHF air band (225 - 400 MHz AM, 25 kHz): 243.000 guard (priority)\nand 282.800 NATO on-scene SAR, and tunes to 243.000. Other channels are assigned per base: add them by hand,\nfrom On the air, or find them with the fast scan profile \"Military UHF air 225-400\"."));
    flowEnd();
    if (!S.addErr.empty()) ImGui::TextColored(pal::warnAmber(), "%s", S.addErr.c_str());
}

// ---------------------------------------------------------------- the views

// the channels of an airport (or of the band watch) into the list: replace it, or add what is not there yet; sorted by frequency
void applyChannels(const std::vector<dect2::AirbandChannel>& ch, bool replace) {
    if (replace) S.chans = ch;
    else for (const auto& c : ch) {
        bool dup = false;
        for (const auto& o : S.chans) dup |= std::fabs(o.freqHz - c.freqHz) < 1 && o.is833 == c.is833;
        if (!dup) S.chans.push_back(c);
    }
    std::sort(S.chans.begin(), S.chans.end(), [](const auto& x, const auto& y) { return x.freqHz < y.freqHz; });
    S.editing = -1;
    save();
}

ImU32 ledColour(const dect2::AirbandChannelState* s, double now) {
    if (!s) return IM_COL32(90, 90, 90, 255);
    if (!s->inBand) return IM_COL32(150, 70, 60, 255);
    if (s->open) return s->heterodyne ? IM_COL32(230, 170, 50, 255) : IM_COL32(90, 220, 120, 255);
    if (s->transmissions > 0 && now - s->lastSec < 30) return IM_COL32(60, 130, 80, 255);   // heard a moment ago
    return IM_COL32(85, 90, 100, 255);
}

void led(ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight(), r = 4.5f * gUi;
    dl->AddCircleFilled(ImVec2(p.x + r + 1, p.y + h / 2), r, col);
    ImGui::Dummy(ImVec2(2 * r + 4, h));
    ImGui::SameLine(0, 2 * gUi);
}

// What is in the list but outside what the radio takes in: one button per capture that holds some of it
void captureBanner(App& a) {
    if (!live(a) || S.chans.empty()) return;
    const auto& t = a.rx.airband;
    int in = 0;
    for (const auto& c : t.channels) in += c.inBand;
    if (t.channels.size() != S.chans.size() || in == (int)S.chans.size()) return;
    std::vector<double> f;
    for (const auto& c : S.chans) f.push_back(c.freqHz);
    const auto g = dect2::airbandPlanGroups(f, dect2::airbandUsableSpanHz(a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate));
    ImGui::PushTextWrapPos(0);
    if (in) ImGui::TextColored(pal::warnAmber(), tr("%d of %zu channels are outside what the radio takes in now. Tune to a capture:"), (int)S.chans.size() - in, S.chans.size());
    else ImGui::TextColored(pal::warnAmber(), tr("None of the %zu channels is inside what the radio takes in now. Tune to a capture:"), S.chans.size());
    ImGui::PopTextWrapPos();
    for (size_t k = 0; k < g.size(); k++) {
        char b[96];
        snprintf(b, sizeof b, "%.3f - %.3f MHz (%zu)##capt%zu", g[k].loHz / 1e6, g[k].hiHz / 1e6, g[k].members.size(), k);
        if (ImGui::Button(b)) airportTuneCentre(a, g[k].centerHz);
        flowNext(4 * gUi);
    }
    flowEnd();
}

void channelView(App& a) {
    controls(a);
    airportSection(a, applyChannels);
    addRow(a);
    ImGui::Spacing();
    captureBanner(a);
    const auto& tel = a.rx.airband;
    if (live(a) && !S.chans.empty()) {
        std::string names;
        int open = 0, inBand = 0;
        for (const auto& c : tel.channels) { inBand += c.inBand; if (c.open) { open++; names += (names.empty() ? "" : ", ") + (c.label.empty() ? c.name : c.label); } }
        if (open) ImGui::TextColored(pal::okGreen(), tr("On air: %s"), names.c_str());
        else if (inBand) ImGui::TextDisabled("%s", tr("No signal: every channel is quiet."));
    }
    const int nc = 11;
    if (!ImGui::BeginTable("##airch", nc, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing()))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
    ImGui::TableSetupColumn("Sq"); ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("Carrier"); ImGui::TableSetupColumn("Heard");
    ImGui::TableSetupColumn("Listen"); ImGui::TableSetupColumn("M"); ImGui::TableSetupColumn("S"); ImGui::TableSetupColumn("P");
    ImGui::TableHeadersRow();
    int remove = -1, hidden = 0;
    for (size_t i = 0; i < S.chans.size(); i++) {
        dect2::AirbandChannel& c = S.chans[i];
        const dect2::AirbandChannelState* s = stateOf(a, i);
        if (S.onlyActive && live(a) && s && !(s->open || (s->transmissions > 0 && tel.timeSec - s->lastSec < 30))) { hidden++; continue; }
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        led(ledColour(s, tel.timeSec));
        ImGui::PushFont(a.mono, 0);
        ImGui::Text("%s", dect2::airbandName(c.freqHz, c.is833).c_str());
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz, %s spacing", c.freqHz / 1e6, c.is833 ? "8.33 kHz" : "25 kHz");
        ImGui::SameLine(); ImGui::TextDisabled("%s", c.is833 ? "8.33" : "25");
        ImGui::TableNextColumn();
        if (S.editing == (int)i) {
            // the field takes the keys at once; Enter or a click elsewhere keeps the name, Escape drops it, right-click removes the
            // channel. Before, the field stayed open after a click elsewhere and the row could then not be removed or edited (#148).
            ImGui::SetNextItemWidth(100 * gUi);
            if (S.editFocus) { ImGui::SetKeyboardFocusHere(); S.editFocus = false; }
            ImGui::InputText("##ed", S.editLabel, sizeof S.editLabel);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) remove = (int)i;
            else if (ImGui::IsItemDeactivated()) {
                if (!ImGui::IsKeyPressed(ImGuiKey_Escape)) { c.label = S.editLabel; save(); }
                S.editing = -1;
            } else if (!ImGui::IsItemActive() && !ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) S.editing = -1;
        } else {
            ImGui::TextUnformatted(c.label.empty() ? "-" : c.label.c_str());
            if (ImGui::IsItemClicked()) { S.editing = (int)i; S.editFocus = true; snprintf(S.editLabel, sizeof S.editLabel, "%s", c.label.c_str()); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to rename; right-click to remove the channel.");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) remove = (int)i;
        }
        ImGui::TableNextColumn();
        {
            const float snr = s ? s->snrDb : -99;
            const float frac = std::max(0.f, std::min(1.f, (snr + 5) / 45));
            char ov[32] = "";
            if (s && s->inBand) { if (s->open || snr > 0) snprintf(ov, sizeof ov, "%.0f dB", snr); else snprintf(ov, sizeof ov, "%s", tr("no signal")); }
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, s && s->open ? pal::accent() : pal::grey(0.45f));
            ImGui::ProgressBar(frac, ImVec2(110 * gUi, 0), ov);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && s) ImGui::SetTooltip("carrier to noise %.1f dB (squelch %.0f dB), channel power %.1f dBFS", s->snrDb, c.squelchDb > -50 ? c.squelchDb : S.squelch, s->levelDb);
        }
        ImGui::TableNextColumn();
        {
            const bool own = c.squelchDb > -50;
            float v = own ? c.squelchDb : S.squelch;
            ImGui::SetNextItemWidth(58 * gUi);
            if (ImGui::DragFloat("##sq", &v, 0.2f, -5.f, 45.f, own ? "%.0f" : "(%.0f)")) c.squelchDb = std::max(-5.f, std::min(45.f, v));
            if (ImGui::IsItemDeactivatedAfterEdit()) save();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("This channel's squelch (carrier to noise, dB). In brackets: the global squelch applies. Drag to set; right-click to go back to the global one."));
            if (own && ImGui::IsItemClicked(ImGuiMouseButton_Right)) { c.squelchDb = dect2::kAirbandSquelchGlobal; save(); }
        }
        ImGui::TableNextColumn();
        if (!s) ImGui::TextDisabled("-");
        else if (!s->inBand) {   // outside the span the radio takes in around the dial (#52): one click moves the dial there
            if (ImGui::SmallButton(tr("tune here"))) tuneDialTo(a, c.freqHz);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Out of band: the radio takes in %.1f MHz around the dial (the sample rate), and this channel is outside it.\n"
                                                         "Click to move the dial to it, or raise the sample rate (Tuner) to take in more of 118 - 137 MHz at once.", a.engine.sampleRate() / 1e6);
        }
        else if (s->open) ImGui::TextColored(s->heterodyne ? pal::warnAmber() : pal::okGreen(), "%s%s", s->heterodyne ? "OPEN 2 stn" : "OPEN", s->playing ? " >" : "");
        else ImGui::TextDisabled("closed");
        ImGui::TableNextColumn();
        if (s && s->open) ImGui::Text("%+.0f Hz", s->carrierHz); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (s) ImGui::Text("%llu", (unsigned long long)s->transmissions); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        {
            bool only = c.solo;
            for (size_t k = 0; k < S.chans.size(); k++) if (k != i && S.chans[k].solo) only = false;
            if (ImGui::SmallButton(only ? tr("Stop##lis") : tr("Listen##lis"))) {
                for (size_t k = 0; k < S.chans.size(); k++) S.chans[k].solo = !only && k == i;
                if (!only) S.chans[i].muted = false;
                save();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", only ? tr("Back to hearing every open channel.") : tr("Hear only this channel (solo), whatever else is on the air."));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("30 s##rpl")) && S.clips) {
                std::string err;
                const std::string path = S.clips->saveReplay((int)i, err);
                if (path.empty()) S.recNote = err;
                else {
                    dect2::voice::Clip k; k.path = path; k.mode = "airband"; k.wallStart = (int64_t)time(nullptr); k.label = tr("last 30 s of ") + (c.label.empty() ? dect2::airbandName(c.freqHz, c.is833) : c.label);
                    int rate = 0; dect2::voice::wavInfo(path, rate, k.seconds);
                    S.fresh.insert(S.fresh.begin(), k);
                    S.recNote = tr("Saved: play it in Recordings.");
                    S.view = 4;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Replay: the last 30 s of transmissions on this channel (the gaps left out), saved as a clip in Recordings."));
        }
        ImGui::TableNextColumn(); if (ImGui::Checkbox("##m", &c.muted)) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute: listened to and logged, not played.");
        ImGui::TableNextColumn(); if (ImGui::Checkbox("##s", &c.solo)) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solo: while any channel is solo, only the solo channels play.");
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("##p", &c.priority)) { if (c.priority) for (size_t k = 0; k < S.chans.size(); k++) if (k != i) S.chans[k].priority = false; save(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Priority: takes over the scan when it opens, and pushes the other channels down in the mix.");
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove >= 0) { S.chans.erase(S.chans.begin() + remove); S.editing = -1; save(); }
    if (ImGui::Checkbox(tr("Only active##airact"), &S.onlyActive)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show only the channels that are open or were heard in the last 30 seconds."));
    if (hidden) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled(tr("%d quiet channels hidden"), hidden); }
    if (S.chans.empty()) {
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextDisabled("%s", tr("No channel yet: search an airport above, add a frequency, or see what is on the air (On the air tab)."));
    }
}

// carriers the band watch found that are not in the list
void seenView(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Carriers on the air in the whole capture that are not in your list, strongest activity first. Add the ones you want to hear."));
    ImGui::PopTextWrapPos();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("Start the receiver to see what is on the air.")); return; }
    const auto& seen = a.rx.airband.seen;
    std::vector<const dect2::AirbandSeen*> v;
    for (const auto& x : seen) {
        bool listed = false;
        for (const auto& c : S.chans) listed |= std::fabs(c.freqHz - x.freqHz) < 1200;
        if (!listed) v.push_back(&x);
    }
    if (ImGui::Button(tr("Add all##seenall")) && !v.empty()) {
        std::vector<dect2::AirbandChannel> add;
        for (const auto* x : v) { dect2::AirbandChannel c; c.freqHz = x->freqHz; c.is833 = x->is833; c.label = airportLabelFor(x->freqHz); add.push_back(c); }
        applyChannels(add, false);
    }
    if (v.empty()) { flowNext(8 * gUi); ImGui::TextDisabled("%s", tr("Nothing yet: carriers show here a moment after they start transmitting.")); }
    flowEnd();
    if (!v.empty() && v.front()->approx) ImGui::TextColored(pal::warnAmber(), "%s", tr("The radio's tuning error is not measured yet: names may be off by a channel."));
    if (!ImGui::BeginTable("##airseen", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Airport"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Heard");
    ImGui::TableSetupColumn("Last"); ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < v.size(); i++) {
        const dect2::AirbandSeen& x = *v[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        led(x.activeNow ? IM_COL32(90, 220, 120, 255) : IM_COL32(85, 90, 100, 255));
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(x.name.c_str()); ImGui::PopFont();
        ImGui::SameLine(); ImGui::TextDisabled("%s", x.is833 ? "8.33" : "25");
        ImGui::TableNextColumn();
        const std::string lb = airportLabelFor(x.freqHz);
        ImGui::TextUnformatted(lb.empty() ? "-" : lb.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", x.excessDb);
        ImGui::TableNextColumn(); ImGui::Text("%u", x.hits);
        ImGui::TableNextColumn(); if (x.activeNow) ImGui::TextColored(pal::okGreen(), "%s", tr("now")); else ImGui::TextDisabled("%.0f s", a.rx.airband.timeSec - x.lastSec);
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(tr("Add##seen"))) {
            dect2::AirbandChannel c; c.freqHz = x.freqHz; c.is833 = x.is833; c.label = lb;
            applyChannels({c}, false);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void activityView(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the activity"); ImGui::PopTextWrapPos(); return; }
    if (resultsClearButton("airband")) a.engine.airband().clearResults();
    const auto& log = a.rx.airband.activity;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& e : log) {
            char b[200];
            snprintf(b, sizeof b, "%s\t%s\t%s\t%.1f s\t%.0f dB\t%.1f dBFS\n", clockText(e.wallTime).c_str(), e.name.c_str(), e.label.c_str(), e.durSec, e.snrDb, e.levelDb);
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the log to the clipboard, one transmission per line.");
    if (!ImGui::BeginTable("##airlog", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("C/N"); ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& e : log) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.name.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.label.empty() ? "-" : e.label.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", e.durSec);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB%s", e.snrDb, e.heterodyne ? " 2 stn" : "");
        ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", e.levelDb);
    }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("nothing heard yet");
}

void spectrumView(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Every channel of the list: green while open, the light band its filter (8.33 kHz: +-3.4 kHz, 25 kHz: +-5 kHz).");
    ImGui::PopTextWrapPos();
    if (!plt::BeginPlot("##airspec", ImVec2(-1, ImGui::GetContentRegionAvail().y), plt::Flags_NoLegend | plt::Flags_NoTitle)) return;
    plt::SetupAxes("frequency (MHz)", "dBFS/bin");
    const double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
    const double rc = radioCenterMhz(a);
    plt::SetupAxisLimits(plt::X1, rc - fs / 2, rc + fs / 2, plt::Cond_Once);
    plt::SetupAxisLimits(plt::Y1, a.yMin, a.yMax, plt::Cond_Once);
    plt::SetupAxisSpan(plt::X1, std::min(0.001, fs), fs);   // the wheel limits of the main spectrum (#425, #442)
    plt::SetupAxisConstraints(plt::Y1, -160, 20);
    plt::SetupAxisSpan(plt::Y1, 10, 180);
    plt::SetupAxisFormat(plt::X1, "%.3f");
    for (size_t i = 0; i < S.chans.size(); i++) {
        const auto& c = S.chans[i];
        const dect2::AirbandChannelState* s = stateOf(a, i);
        const double f = c.freqHz / 1e6 + (live(a) ? a.rx.airband.cfoHz / 1e6 : 0), hw = (c.is833 ? 3400 : 5000) / 1e6;
        char id[24];
        snprintf(id, sizeof id, "b%zu", i);
        plt::Spec bs; bs.FillColor = s && s->open ? ImVec4(0.3f, 0.8f, 0.4f, 0.25f) : pal::accent(0.12f);
        plt::PlotVBand(id, f - hw, f + hw, bs);
        snprintf(id, sizeof id, "m%zu", i);
        plt::Spec ms; ms.LineColor = s && s->open ? ImVec4(0.3f, 0.85f, 0.4f, 0.9f) : pal::grey(0.5f);
        plt::PlotInfLines(id, &f, 1, ms);
        const std::string lb = c.label.empty() ? dect2::airbandName(c.freqHz, c.is833) : c.label;
        plt::PlotText(lb.c_str(), f, a.yMax, ImVec2(0, 10 + (float)(i % 3) * 13));
    }
    if (!a.smooth.empty()) {
        auto x = xs(a);
        std::vector<double> y(a.smooth.begin(), a.smooth.end());
        plt::Spec ss; ss.LineColor = pal::accent(); ss.LineWeight = 1.2f;
        plt::PlotLine("spectrum", x.data(), y.data(), (int)std::min(x.size(), y.size()), ss);
    }
    plt::EndPlot();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load(a);
    const int kind = synthetic(a) ? 0 : 1;
    if (S.defaultList && kind != S.lastKind) {   // an untouched default follows the source: the test channels for the test signal, none for a radio
        S.chans = kind == 0 ? dect2::airbandChannelsFor(dect2::airbandTestLayout(), dect2::airbandTuning().defMhz * 1e6) : std::vector<dect2::AirbandChannel>();
        S.dirty = true;
    }
    S.lastKind = kind;
    const bool run = a.engine.running() && a.rx.standard == 27;
    if (!a.engine.running()) { if (S.wasRunning && S.clips) S.clips->flush(); S.wasRunning = false; return; }
    dect2::AirbandReceiver& r = a.engine.airband();
    r.setNoiseRemover(S.nrOn, S.nrStr / 100.f);
    if (!S.wasRunning || S.dirty) {
        r.setChannels(S.chans); r.setSquelchDb(S.squelch); r.setHangSec(S.hang); r.setScan(S.scan); r.setScanHold(S.hold);
        clipChannels();
        S.dirty = false;
    }
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    if (!S.clips) { S.clips = std::make_shared<dect2::AirbandClipWriter>(clipFolder()); S.clips->setEnabled(S.rec); clipChannels(); }
    if (!S.wasRunning) { clipChannels(); r.setClipWriter(S.clips); }
    for (auto& c : S.clips->takeFinished()) { S.fresh.insert(S.fresh.begin(), c); if (S.fresh.size() > 300) S.fresh.pop_back(); }
    S.wasRunning = true;
    (void)run;
}

void tab(App& a) {
    load(a);
    subNav("airv", S.view, {"Channels", tr("On the air"), "Activity", "Spectrum", tr("Recordings")});
    tx::captions(a);
    if (S.view == 0) channelView(a); else if (S.view == 1) seenView(a); else if (S.view == 2) activityView(a); else if (S.view == 3) spectrumView(a); else recordingsView(a);
}

void list(App& a) {
    load(a);
    if (!ImGui::BeginTable("##airl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
    for (size_t i = 0; i < S.chans.size(); i++) {
        const auto& c = S.chans[i];
        const dect2::AirbandChannelState* s = stateOf(a, i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::airbandName(c.freqHz, c.is833).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        const std::string lb = c.label.empty() ? std::string("") : c.label + "  ";
        if (s && s->open) ImGui::TextColored(pal::okGreen(), "%sOPEN", lb.c_str());
        else if (s && !s->inBand) {
            ImGui::TextDisabled("%s", lb.c_str()); ImGui::SameLine(0, 0);
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("tune here")) tuneDialTo(a, c.freqHz);
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Out of band: outside the span the radio takes in around the dial. Click to move the dial to it.");
        } else ImGui::TextDisabled("%s", lb.c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::AirbandTelemetry& t = a.rx.airband;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    int inBand = 0, open = 0;
    for (const auto& c : t.channels) { inBand += c.inBand; open += c.open; }
    kv("state", "%s", dect2::airbandSummary(t).c_str());
    kv("channels", "%zu listed, %d in the band, %d open", t.channels.size(), inBand, open);
    kv("tuning error", "%+.0f Hz%s", t.cfoHz, t.tuneKnown ? " (measured)" : " (not measured yet)");
    kv("squelch", "%.0f dB carrier to noise, hang %.1f s", t.squelchDb, t.hangSec);
    kv("audio", "%s", t.scan ? (t.scanChan >= 0 ? "scan: stopped on a channel" : "scan: scanning") : "mix of the open channels");
    kv("transmissions", "%llu", (unsigned long long)t.blocksOk);
    kv("radio centre", "%.4f MHz", t.centerHz / 1e6);
    kv("input", "%.3f Msps, %.1f dBFS; channels at %.0f Hz", t.inputRate / 1e6, t.levelDb, t.channelRate);
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool on = live(a);
    StatusPanel panel;
    statusIqLamp(a);
    lamp("Open", !on ? 0 : a.rx.airband.state == 2 ? 1 : 0); flowNext(10 * gUi);
    statusState(a, on, [&] { return dect2::airbandSummary(a.rx.airband); });
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Airband";
    if (live(a)) l2 = dect2::airbandSummary(a.rx.airband);
}

} // namespace

// First start, with a real radio and an empty list: the nearest airport (ui/first_start.cpp)
// E15: "Listen" on an ADS-B aircraft: Airband, on the nearest airport to it. Multi-radio setups keep ADS-B where it is only with a second receiver window: this one switches.
bool airbandListenAt(App& a, double lat, double lon, int altFt, std::string& what) {
    load(a);
    if (a.family != 26) selectMode(a, 26);
    return airportListenAt(a, lat, lon, altFt, applyChannels, what);
}

bool airbandFirstLoad(App& a, std::string& what) {
    load(a);
    if (!S.chans.empty()) return false;
    return airportLoadNearest(a, applyChannels, what);
}

extern const ModeUi kAirbandUi;
const ModeUi kAirbandUi = {
    .sideTitle = "CHANNELS",
    .tabName = "Channels",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
