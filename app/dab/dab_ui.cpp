// DAB / DAB+ digital radio panels.
#include "app.h"
#include "dab/played_ui.h"
#include "panels/journaline_view.h"
#include "panels/enhance_ui.h"
#include "state/results_export.h"
#include "dect2/dab/dab_announce.h"
#include "dect2/dab/dab_fig.h"
#include <ctime>
#include <filesystem>

// DAB / DAB+ screens (included by main.cpp): channel plan, status lines, constellation panels, station list and player,
// the Radio and Ensemble tabs, and the Band III scanner.

// ------------------------------------------------------------------ Band III channel plan
struct DabChan { const char* name; double mhz; };
static const DabChan kDabCh[] = {
    {"5A", 174.928}, {"5B", 176.640}, {"5C", 178.352}, {"5D", 180.064}, {"6A", 181.936}, {"6B", 183.648}, {"6C", 185.360}, {"6D", 187.072},
    {"7A", 188.928}, {"7B", 190.640}, {"7C", 192.352}, {"7D", 194.064}, {"8A", 195.936}, {"8B", 197.648}, {"8C", 199.360}, {"8D", 201.072},
    {"9A", 202.928}, {"9B", 204.640}, {"9C", 206.352}, {"9D", 208.064}, {"10A", 209.936}, {"10B", 211.648}, {"10C", 213.360}, {"10D", 215.072},
    {"11A", 216.928}, {"11B", 218.640}, {"11C", 220.352}, {"11D", 222.064}, {"12A", 223.936}, {"12B", 225.648}, {"12C", 227.360}, {"12D", 229.072},
    {"13A", 230.784}, {"13B", 232.496}, {"13C", 234.208}, {"13D", 235.776}, {"13E", 237.488}, {"13F", 239.200}};
static constexpr int kNumDabCh = (int)(sizeof kDabCh / sizeof *kDabCh);

const char* dabChannelName(double mhz) {
    for (const auto& c : kDabCh) if (std::fabs(c.mhz - mhz) < 0.05) return c.name;
    return nullptr;
}

// A selector of DAB channels, used instead of the bandwidth box in DAB mode. Returns true when the frequency changed.
bool dabChannelCombo(App& a) {
    const char* cur = dabChannelName(a.freqMhz);
    char lbl[48];
    snprintf(lbl, sizeof lbl, cur ? "%s  %.3f MHz" : "Channel", cur ? cur : "", a.freqMhz);
    bool changed = false;
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::BeginCombo("##dabch", lbl)) {
        for (const auto& c : kDabCh) {
            char l[48];
            snprintf(l, sizeof l, "%-4s %.3f MHz", c.name, c.mhz);
            if (ImGui::Selectable(l, cur && !strcmp(cur, c.name))) { a.freqMhz = c.mhz; changed = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("DAB Band III channels 5A to 13F. Every DAB multiplex (ensemble) sits on one of these.");
    return changed;
}

// ------------------------------------------------------------------ status lines
void dabStatus(App& a) {
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const DabTelemetry& d = a.rx.dab;
    const bool live = run && a.rx.standard == 3;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sync", !live ? 0 : d.state == 2 ? 1 : 0); flowNext(12 * gUi);
    lamp("PRS", !live ? 0 : d.cirPeak > 40 ? 1 : d.cirPeak > 15 ? 2 : 0); flowNext(12 * gUi);
    lamp("FIC", !live ? 0 : d.ficRecentOk >= 11 ? 1 : d.ficRecentOk > 0 ? 2 : d.state == 2 ? 3 : 0); flowNext(12 * gUi);
    lamp("Ensemble", !live ? 0 : d.ensemble ? 1 : d.fibOk ? 2 : 0); flowNext(12 * gUi);
    if (live && d.dmb.active) {   // a DMB video service: the outer code's blocks, and the sound of the player
        lamp("MSC", d.dmb.rsOk > 0 && d.dmb.rsFailed * 4 <= d.dmb.rsOk ? 1 : d.dmb.sync ? 2 : 0); flowNext(12 * gUi);
        lamp("Audio", a.engine.player().stats().hasAudio ? 1 : 2, (int)Ic::Speaker); flowNext(10 * gUi);
    } else {
        lamp("MSC", !live || d.audio.sub < 0 ? 0 : d.audio.superframesOk > 0 && d.audio.superframesBad * 4 <= d.audio.superframesOk ? 1 : d.audio.frames > 0 ? 2 : 0); flowNext(12 * gUi);
        lamp("Audio", !live || d.audio.sub < 0 ? 0 : d.audio.decoding ? 1 : 2, (int)Ic::Speaker); flowNext(10 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!run) ro("State", "stopped", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (!live) ro("State", "starting", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (d.state == 2) ro("State", "Locked", ImVec4(0.35f, 0.90f, 0.45f, 1));
    else ro("State", "searching", ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (live && d.state == 2) {
        ro("Mode", "DAB mode I");
        if (d.ensemble) {
            ro("Ensemble", d.ensembleLabel.empty() ? "-" : d.ensembleLabel);
            snprintf(b, sizeof b, "%d", d.services); ro("Stations", b);
        } else ro("Ensemble", "reading...", ImVec4(0.6f, 0.64f, 0.68f, 1));
    }
    flowBreak();
    if (live && d.state == 2) {
        snprintf(b, sizeof b, "%+.1f Hz", d.cfoHz); ro("CFO", b);
        // the sample clock error the receiver follows: the radio's crystal is off by about as much (a ppm correction for the device)
        if (std::fabs(d.sroPpm) >= 0.5) { snprintf(b, sizeof b, "%+.1f ppm", d.sroPpm); ro("Clock", b); }
        snprintf(b, sizeof b, "%.1f dB", d.snrDb); ro("SNR", b);
        // coherent: demodulated with the channel estimate (about 1 dB more sensitive); differential: the channel changes too fast for it
        ro("Detection", d.coherent ? "coherent" : "differential (fast fading)");
        const double tot = (double)(d.fibOk + d.fibBad);
        snprintf(b, sizeof b, "%.1f%%", tot > 0 ? 100.0 * (double)d.fibOk / tot : 0.0); ro("FIC ok", b);
        if (d.scrambled) ro("Audio", "scrambled (conditional access): cannot be played", ImVec4(0.95f, 0.45f, 0.35f, 1));
        else if (d.audio.sub >= 0) {   // the audio's health: what share of the audio frames had to be concealed (docs/DAB-AUDIT.md)
            const DabAudioStats& au = d.audio;
            const double units = au.dabPlus ? (double)(au.auOk + au.auBad) : (double)au.frames;
            const double pc = units > 0 ? 100.0 * (double)au.concealedFrames / units : 0.0;
            snprintf(b, sizeof b, "%.1f%% concealed", pc);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Audio"); ImGui::SameLine(0, 5 * gUi);
            ImGui::PushFont(a.mono, 0);
            ImGui::TextColored(pc < 0.5 ? ImVec4(0.93f, 0.95f, 0.97f, 1) : pc < 5 ? ImVec4(0.95f, 0.70f, 0.15f, 1) : ImVec4(0.95f, 0.35f, 0.25f, 1), "%s", b);
            ImGui::PopFont();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reed-Solomon: %llu bytes fixed, %llu code words beyond repair\n%s: %llu ok, %llu lost and concealed\nSilent after long losses: %.1f s\n"
                                  "Sync lost %llu times (%llu found again at once), %llu CIFs lost on the way (decoded as erasures)\n"
                                  "Impulse blanker: %llu samples\nOutput: %d ms queued, rate correction %+.0f ppm",
                                  (unsigned long long)au.rsCorrected, (unsigned long long)au.rsUncorrectable, au.dabPlus ? "Access units" : "MP2 frames",
                                  (unsigned long long)(au.dabPlus ? au.auOk : au.frames - std::min(au.frames, au.concealedFrames)), (unsigned long long)au.concealedFrames,
                                  (double)au.silenceMs / 1000.0, (unsigned long long)d.syncLosses, (unsigned long long)d.relocks, (unsigned long long)d.erasedCifs,
                                  (unsigned long long)d.impulses, au.bufferedMs, au.rateCorrPpm);
            flowNext(15 * gUi);
        }
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Level"); ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? pal::remap(IM_COL32(40, 112, 150, 255)) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ADC level (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f", adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    flowNext(15 * gUi);
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Signal, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Signal"); ImGui::SameLine(0, 5 * gUi);
    {
        const float t = live && d.state == 2 ? std::min(1.f, std::max(0.f, (float)((d.snrDb - 4.0) / 18.0))) : 0.f;
        const ImU32 col = t < 0.25f ? IM_COL32(176, 66, 58, 255) : t < 0.5f ? IM_COL32(176, 130, 48, 255) : pal::remap(IM_COL32(40, 112, 150, 255));
        snprintf(b, sizeof b, live && d.state == 2 ? "%.0f%%  %s" : "-", t * 100, t > 0.8f ? "excellent" : t > 0.55f ? "good" : t > 0.3f ? "fair" : "poor");
        gaugePill(130, t, col, b);
        if (ImGui::IsItemHovered() && live) ImGui::SetTooltip("DAB needs roughly 10 dB SNR for error-free audio.\nSNR %.1f dB, FIC blocks ok %d of 12 in the last frame.", d.snrDb, d.ficRecentOk);
    }
    flowNext(15 * gUi);
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro("dropped", lossText(l), lossColour(l)); lossTooltip(l); }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (adc == AdcStatus::Overload) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "ADC OVERLOAD - %s", a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::Low) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level low - raise the gain"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::NoSignal) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC sees almost nothing - antenna / gain?"); ImGui::PopTextWrapPos(); }
        else flowBreak();
    } else flowBreak();
}

// ------------------------------------------------------------------ selection and announcements
static void dabPrefs(App& a) {
    if (a.dabPrefsLoaded) return;
    a.dabPrefsLoaded = true;
    a.dabAnnOn = plat::prefs().getB("dabAnnOn", true);
    a.dabAnnMask = (unsigned)plat::prefs().getI("dabAnnMask", 0x3F);
    a.dabRecFmt = (int)plat::prefs().getI("dabRecFmt", 0);
    a.dabRecLog = plat::prefs().getB("dabRecLog", true);
    a.dabFollow = plat::prefs().getB("dabFollow", false);
}

static void dabPlaySub(App& a, int sub) {
    a.engine.dabSelect(sub);
    a.engine.dabAudio().setVolume(a.volume);
    a.engine.dabAudio().setMuted(a.muted);
    a.engine.player().setVolume(a.volume);   // DMB video services play through the player
    a.engine.player().setMuted(a.muted);
}

// The radio onto a DAB channel (the band scan, the scan list, a transmitter on the Map tab, the other frequencies of an ensemble). A decode
// shift left by a click on the spectrum (sticky tuning) is dropped: kept, the receiver would look for the ensemble beside its channel by
// that shift (#262). The old ensemble is forgotten (retuneReset).
void dabTuneMhz(App& a, double mhz) {
    a.freqMhz = mhz;
    a.tune.centerHz = a.freqMhz * 1e6;
    if (!a.engine.running()) return;
    a.engine.setDecodeShiftHz(0);
    a.engine.retuneReset(a.tune);
    a.peak.clear();
}

// Signal lost for a few seconds: tune to the frequencies the ensemble gives for itself, and for the other ensembles that carry the service,
// one after the other, and play the service again where it is found (the sub-channel may have another number there)
static void dabFollowStep(App& a) {
    if (!a.dabFollow || !a.dabMode || !a.engine.running() || a.dabScan.running || a.dabUserSub < 0) { a.dabFollowing = false; a.dabLostSince = 0; return; }
    const double now = ImGui::GetTime();
    const bool locked = a.rx.standard == 3 && a.rx.dab.state == 2 && a.rx.dab.ensemble;
    auto next = [&] {
        if (a.dabAlts.empty()) return;
        const auto alt = a.dabAlts[(size_t)a.dabFollowIdx++ % a.dabAlts.size()];
        char b[96]; snprintf(b, sizeof b, "DAB: signal lost, trying %s %.3f MHz", dabChannelName(alt.first) ? dabChannelName(alt.first) : "", alt.first);
        a.engine.log(b);
        dabTuneMhz(a, alt.first);
        a.dabFollowing = true; a.dabFollowT0 = now; a.dabLostSince = now;
    };
    if (!locked) {
        if (a.dabLostSince == 0) a.dabLostSince = now;
        if (now - a.dabLostSince >= 6.0) next();
        return;
    }
    a.dabLostSince = 0;
    const DabEnsemble ens = a.engine.dabEnsemble();
    if (a.dabFollowing) {   // arrived somewhere: is the service here?
        for (const auto& kv : ens.services) {
            if (kv.first != a.dabFollowSid) continue;
            const DabComponent* c = kv.second.playable();
            if (!c) continue;
            a.dabFollowing = false;
            a.dabUserSub = c->subId;
            dabPlaySub(a, c->subId);
            a.engine.log("DAB: service found again");
            return;
        }
        if (ens.valid && now - a.dabFollowT0 > 12.0) next();   // another ensemble that does not have it: the next frequency
        return;
    }
    if (now - a.dabAltStamp < 5.0) return;
    a.dabAltStamp = now;
    const DabFicInfo info = dabFicInfo(a.engine.dabFic());
    a.dabFollowSid = 0;
    for (const auto& kv : ens.services) for (const auto& c : kv.second.comps) if (c.subId == a.dabUserSub) a.dabFollowSid = kv.first;
    std::vector<uint16_t> eids = {ens.eid};
    const auto oe = info.otherEnsembles.find(a.dabFollowSid);
    if (oe != info.otherEnsembles.end()) eids.insert(eids.end(), oe->second.begin(), oe->second.end());
    std::vector<std::pair<double, uint16_t>> alts;
    for (const auto& f : info.freqs) {
        if ((f.rm != 0 && f.rm != 1) || std::find(eids.begin(), eids.end(), (uint16_t)f.id) == eids.end()) continue;
        for (double khz : f.kHz) {
            const double mhz = khz / 1000.0;
            if (std::fabs(mhz - a.freqMhz) < 0.01 || !dabChannelName(mhz)) continue;
            bool dup = false;
            for (const auto& x : alts) if (std::fabs(x.first - mhz) < 0.01) dup = true;
            if (!dup) alts.push_back({mhz, (uint16_t)f.id});
        }
    }
    a.dabAlts = alts;
}

// While an announcement the user wants is on, the sub-channel that carries it is played; when it ends the service comes back
static void dabAnnouncementStep(App& a) {
    if (!a.dabMode || !a.engine.running()) { a.dabAnnActive = false; return; }
    const unsigned mask = a.dabAnnOn ? a.dabAnnMask : 0;
    const double now = ImGui::GetTime();
    const bool settled = now - a.dabAnnLastSwitch >= 5.0;   // a switch restarts the audio decoder: not more often than every 5 seconds
    auto go = [&](int sub, const char* what) {
        a.dabAnnLastSwitch = now;
        char b[96]; snprintf(b, sizeof b, "DAB announcement: %s, sub-channel %d", what, sub);
        a.engine.log(b);
        dabPlaySub(a, sub);
    };
    auto back = [&] { if (a.dabAnnActive && settled) { a.dabAnnActive = false; go(a.dabUserSub, "back to the station"); } };
    if (a.dabUserSub < 0 || !mask) { if (a.dabAnnActive) { a.dabAnnActive = false; dabPlaySub(a, a.dabUserSub); } a.dabAnnSuspended = false; return; }
    DabAnnouncement an;
    const bool have = dabActiveAnnouncement(a.engine.dabEnsemble(), a.dabUserSub, mask, an);
    if (!have) { back(); if (!a.dabAnnActive) a.dabAnnSuspended = false; return; }
    if (a.dabAnnSuspended || !settled) return;
    if (!a.dabAnnActive || an.subId != a.dabAnnSub) {
        a.dabAnnActive = true; a.dabAnnSub = an.subId; a.dabAnnType = an.type;
        go(an.subId, dabAnnouncementTypeName(an.type));
    }
}

// ------------------------------------------------------------------ constellation row
void dabHistory(App& a) {
    dabPrefs(a);
    dabFollowStep(a);
    if (a.dabStation >= 0 && a.dabMode && a.engine.running()) {
        a.engine.dabSelect(a.dabStation); a.engine.dabAudio().setVolume(a.volume);
        a.engine.player().setVolume(a.volume); a.engine.player().setMuted(a.muted);   // a DMB video service plays through the player
        a.dabUserSub = a.dabStation; a.dabAnnActive = false;
        a.dabStation = -1;
    }
    static uint64_t lastSeq = 0;
    if (a.rx.standard != 3 || a.rx.seq == lastSeq) return;
    lastSeq = a.rx.seq;
    dabAnnouncementStep(a);
    if (a.rx.dab.state == 2 && a.rx.dab.audio.sub >= 0) {   // the played list and the broadcast.* events of the playing service (dab/played_ui.h)
        static double lastPl = 0;
        if (ImGui::GetTime() - lastPl > 2) {
            lastPl = ImGui::GetTime();
            const DabEnsemble ens = a.engine.dabEnsemble();
            for (const auto& kv : ens.services) {
                const DabComponent* c = kv.second.playable();
                if (!c || c->subId != a.rx.dab.audio.sub) continue;
                std::string ar, ti;
                for (const DabDlpTag& t : kv.second.dlPlus) { if (t.type == 4 && ar.empty()) ar = t.text; if (t.type == 1 && ti.empty()) ti = t.text; }
                playedNote("DAB", kv.second.label, a.freqMhz, ar, ti, kv.second.dls, kv.second.pty >= 0 ? dabPtyName(1, kv.second.pty) : "", false);
                break;
            }
        }
    }
    auto push = [](std::deque<float>& dq, float v) { dq.push_back(v); if (dq.size() > 600) dq.pop_front(); };
    if (a.rx.dab.state == 2) {
        push(a.dabSnrH, (float)a.rx.dab.snrDb); push(a.dabFicH, (float)a.rx.dab.ficRecentOk);
        push(a.dabCfoH, (float)a.rx.dab.cfoHz); push(a.dabCirH, a.rx.dab.cirPeak);
    }
    // the measures of the selected service start again when another one is selected
    const DabAudioStats& au = a.rx.dab.audio;
    if (au.sub != a.dabHistSub) {
        a.dabHistSub = au.sub;
        a.dabBufH.clear(); a.dabRsH.clear(); a.dabAuH.clear();
        a.dabRsLast = au.rsCorrected; a.dabAuLast = au.auBad;
    }
    if (au.sub >= 0 && a.rx.dab.state == 2) {
        push(a.dabBufH, (float)au.bufferedMs);
        push(a.dabRsH, (float)(au.rsCorrected >= a.dabRsLast ? au.rsCorrected - a.dabRsLast : 0));
        push(a.dabAuH, (float)(au.auBad >= a.dabAuLast ? au.auBad - a.dabAuLast : 0));
        a.dabRsLast = au.rsCorrected; a.dabAuLast = au.auBad;
    }
}

void dabPanels(App& a) {
    const DabTelemetry& d = a.rx.dab;
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(90.f, availH - 26.f - ImGui::GetFrameHeight());
    const float sqW = std::min(plotH, std::max(120.f, availW * 0.25f));                    // the square plot keeps its own width,
    const float colW = std::max(120.f, (availW - 5 * gap - sqW) / 3.f);                    // the other three share the rest
    const ImVec2 sz(colW, plotH);
    const ImVec2 sq(sqW, sqW);        // the cell plot stays square
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    captionFit(sqW, "DQPSK cells (%zu)", d.constellation.size());
    scatter("##d1", d.constellation, sq, 1.8, pal::accent(0.40f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Impulse response (echoes)");
    if (plt::BeginPlot("##dcir", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("us", nullptr, 0, plt::AxisFlags_NoTickLabels);
        if (!d.cir.empty()) {
            const int n = (int)d.cir.size();
            int pk = 0;
            for (int i = 1; i < n; i++) if (d.cir[(size_t)i] > d.cir[(size_t)pk]) pk = i;
            std::vector<double> x(n), y(n);
            for (int i = 0; i < n; i++) {
                int rel = i - pk;
                if (rel > n / 2) rel -= n; else if (rel < -n / 2) rel += n;
                x[(size_t)i] = rel * (1e6 / 2.048e6);
                y[(size_t)i] = d.cir[(size_t)i] / std::max(1e-6f, d.cir[(size_t)pk]);
            }
            // plot in delay order
            std::vector<int> order(n);
            for (int i = 0; i < n; i++) order[(size_t)i] = i;
            std::sort(order.begin(), order.end(), [&](int p, int q) { return x[(size_t)p] < x[(size_t)q]; });
            std::vector<double> xs2(n), ys2(n);
            for (int i = 0; i < n; i++) { xs2[(size_t)i] = x[(size_t)order[(size_t)i]]; ys2[(size_t)i] = y[(size_t)order[(size_t)i]]; }
            plt::SetupAxisLimits(plt::X1, -60, 120, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, 0, 1.05, plt::Cond_Always);
            plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
            plt::PlotLine("cir", xs2.data(), ys2.data(), n, sp);
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "SNR (dB)");
    historyPlot("##dsnr", "dB", a.dabSnrH, sz);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "FIC blocks ok per frame (12 = all)");
    historyPlot("##dfic", "blocks", a.dabFicH, sz);
    ImGui::EndGroup();
}

// ------------------------------------------------------------------ stations (right panel)
std::string dabSubText(const DabEnsemble& e, int sub) {
    auto it = e.subs.find(sub);
    if (it == e.subs.end()) return "";
    const DabSubchannel& s = it->second;
    char b[96];
    if (!s.eep) snprintf(b, sizeof b, "sub %d, UEP %d", sub, s.uepIndex);
    else snprintf(b, sizeof b, "sub %d, %d kbit/s, EEP %d-%c", sub, s.bitrate, s.level + 1, s.option ? 'B' : 'A');
    return b;
}

void dabSelectStation(App& a, int sub, bool play) {
    a.dabUserSub = play ? sub : -1;
    a.dabAnnActive = false; a.dabAnnSuspended = false;
    if (play) dabPlaySub(a, sub);
    else a.engine.dabSelect(-1);
}

// Station logos (SPI service information and slide show pictures of the category "logo"), kept as textures per ensemble id for the session
static struct DabLogoCache {
    struct Tex { gfx::Image* img = nullptr; int w = 0, h = 0; };
    std::map<uint16_t, std::map<uint32_t, Tex>> byEns;
    uint64_t seq = 0;
    int lastSub = -2;
} gDabLogos;
static void dabLogoUpdate(App& a, const DabEnsemble& ens) {
    if (!gGfx || !ens.valid) return;
    DabAudio& au = a.engine.dabAudio();
    const int cur = a.engine.dabSelected();
    if (cur != gDabLogos.lastSub) { gDabLogos.lastSub = cur; gDabLogos.seq = 0; }   // the receiver's counter restarts with another service
    std::vector<DabLogo> v;
    const uint64_t seq = au.logoSeq();
    if (seq == gDabLogos.seq || !au.logos(v, UINT64_MAX)) return;
    gDabLogos.seq = seq;
    uint32_t curSid = 0;
    for (const auto& kv : ens.services) { const DabComponent* c = kv.second.playable(); if (c && c->subId == cur) curSid = kv.first; }
    for (const DabLogo& l : v) {
        const uint32_t sid = l.sid ? l.sid : curSid;
        if (!sid || l.w <= 0 || l.h <= 0 || l.rgba.size() < (size_t)l.w * (size_t)l.h * 4) continue;
        DabLogoCache::Tex& t = gDabLogos.byEns[ens.eid][sid];
        delete t.img;
        t.img = gGfx->createImage(l.w, l.h, 0xFF000000u);
        t.img->update(0, 0, l.w, l.h, reinterpret_cast<const uint32_t*>(l.rgba.data()));
        t.w = l.w; t.h = l.h;
    }
}
void dabLogoSet(uint16_t eid, uint32_t sid, int w, int h, const uint32_t* rgba) {
    if (!gGfx || w <= 0 || h <= 0) return;
    DabLogoCache::Tex& t = gDabLogos.byEns[eid][sid];
    if (t.img && t.w == w && t.h == h) { t.img->update(0, 0, w, h, rgba); return; }
    delete t.img;
    t.img = gGfx->createImage(w, h, 0xFF000000u);
    t.img->update(0, 0, w, h, rgba);
    t.w = w; t.h = h;
}
// The logo of a service in a box `h` high (square at most 1.4 times as wide); false when none is known
bool dabLogoImage(App& a, const DabEnsemble& ens, uint32_t sid, float h) {
    dabLogoUpdate(a, ens);
    auto e = gDabLogos.byEns.find(ens.eid);
    if (e == gDabLogos.byEns.end()) return false;
    auto t = e->second.find(sid);
    if (t == e->second.end()) t = e->second.find(sid & 0xFFFF);
    if (t == e->second.end() || !t->second.img) return false;
    const float sc = std::min(h / (float)t->second.h, 1.4f * h / (float)t->second.w);
    ImGui::Image(t->second.img->texture(), ImVec2((float)t->second.w * sc, (float)t->second.h * sc));
    return true;
}

// The slide show picture the selected DAB+ service sends in its PAD: kept as a texture while the picture lasts
static struct DabSlideView {
    uint64_t seq = 0;
    gfx::Image* img = nullptr;
    int w = 0, h = 0;
    std::string name;
} gDabSlide;
static enhui::LiveTex gDabEnh;   // the Enhance control on the slide
static void dabSlideUpdate(App& a) {
    DabSlide s;
    if (!gGfx || !a.engine.dabAudio().slide(s, gDabSlide.seq)) return;
    gDabSlide.seq = s.seq;
    delete gDabSlide.img;
    gDabSlide.img = nullptr; gDabSlide.w = gDabSlide.h = 0; gDabSlide.name.clear();
    if (s.w <= 0 || s.h <= 0 || s.rgba.size() < (size_t)s.w * (size_t)s.h * 4) return;
    gDabSlide.img = gGfx->createImage(s.w, s.h, 0xFF000000u);
    gDabSlide.img->update(0, 0, s.w, s.h, reinterpret_cast<const uint32_t*>(s.rgba.data()));
    gDabEnh.e.setSource("DAB slide show");
    gDabEnh.feed(reinterpret_cast<const uint32_t*>(s.rgba.data()), s.w, s.h, true);
    gDabSlide.w = s.w; gDabSlide.h = s.h; gDabSlide.name = s.name;
}
// The latest slide show picture of the playing service with the name it came with (and the label), in a box at most `maxH` high
static std::string dlPlusShort(const DabService& sv);
void dabSlidePanel(App& a, const DabEnsemble& ens, float maxH) {
    dabSlideUpdate(a);
    const int cur = a.engine.dabSelected();
    std::string dls, title;
    for (const auto& kv : ens.services) {
        const DabComponent* c = kv.second.playable();
        if (c && c->subId == cur) { dls = kv.second.dls; title = dlPlusShort(kv.second); }
    }
    if (cur < 0 || !gDabSlide.img) {
        ImGui::TextDisabled(cur < 0 ? "no service playing" : "no picture received yet (not every station sends one)");
        return;
    }
    const float bw = ImGui::GetContentRegionAvail().x;
    const float sc = std::max(0.1f, std::min(std::min(bw / (float)gDabSlide.w, maxH / (float)gDabSlide.h), 6.f));
    ImGui::Image(gDabEnh.texture(gDabSlide.img)->texture(), ImVec2((float)gDabSlide.w * sc, (float)gDabSlide.h * sc));
    ImGui::SameLine(0, 10 * gUi);
    ImGui::BeginGroup();
    gDabEnh.controls("dabenh");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(120.f * gUi, bw - (float)gDabSlide.w * sc - 14 * gUi));
    if (!gDabSlide.name.empty()) ImGui::TextUnformatted(gDabSlide.name.c_str());
    ImGui::TextDisabled("%d x %d", gDabSlide.w, gDabSlide.h);
    if (!title.empty()) ImGui::TextUnformatted(title.c_str());
    else if (!dls.empty()) ImGui::TextDisabled("%s", dls.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
}

// The DL Plus tags of a service as lines: "Title", "Artist", "Album" first, the others after them
static void dlPlusLines(const DabService& sv, std::vector<std::pair<std::string, std::string>>& out) {
    for (int want : {1, 4, 2}) for (const DabDlpTag& t : sv.dlPlus) if (t.type == want) out.push_back({dabDlpTypeName(t.type), t.text});
    for (const DabDlpTag& t : sv.dlPlus) if (t.type != 1 && t.type != 4 && t.type != 2) out.push_back({dabDlpTypeName(t.type), t.text});
}

// "Artist - Title" when the station tags them
static std::string dlPlusShort(const DabService& sv) {
    std::string ar, ti;
    for (const DabDlpTag& t : sv.dlPlus) { if (t.type == 4 && ar.empty()) ar = t.text; if (t.type == 1 && ti.empty()) ti = t.text; }
    return ar.empty() ? ti : ti.empty() ? ar : ar + " - " + ti;
}

// What the service says about its programme: type (dynamic or fixed) and language
static std::string programmeText(const DabEnsemble& ens, const DabService& sv) {
    std::string s;
    if (sv.pty >= 0) s = std::string(dabPtyName(1, sv.pty)) + (sv.ptyDynamic ? " (now)" : "");
    if (sv.language > 0) s += (s.empty() ? "" : "   ") + std::string(dabLanguageName(sv.language));
    (void)ens;
    return s;
}

static void dabRecordUi(App& a, const DabEnsemble& ens) {
    const DabRecStats rs = a.engine.dabAudio().recording();
    const DabAudioStats au = a.rx.dab.audio;
    sectionHeader(Ic::Record, "Recording");
    kit::recordingsButton("dab", "dab-recordings");
    if (!rs.active) {
        ImGui::BeginDisabled(au.sub < 0);
        if (ImGui::Button("Record")) {
            std::string name = "dab";
            for (const auto& kv : ens.services) { const DabComponent* c = kv.second.playable(); if (c && c->subId == au.sub) name = kv.second.label; }
            for (char& c : name) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80)) c = '_';
            const std::time_t now = std::time(nullptr);
            char ts[32];
            std::strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", std::localtime(&now));
            std::error_code ec;
            const std::string dir = recordingsSub("dab-recordings");
            std::filesystem::create_directories(dir, ec);
            std::string err;
            if (!a.engine.dabAudio().startRecording(dir + "/" + name + "_" + ts, a.dabRecFmt ? DabRecFormat::Encoded : DabRecFormat::Wav, a.dabRecLog, &err)) a.dabRecErr = err;
            else a.dabRecErr.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130 * gUi);
        if (comboTr("##recfmt", &a.dabRecFmt, {N_("WAV (decoded)"), N_("As broadcast (m4a / mp2)")})) plat::prefs().setI("dabRecFmt", a.dabRecFmt);
        {
            ImGui::SameLine();
            if (ImGui::Checkbox("Label log", &a.dabRecLog)) plat::prefs().setB("dabRecLog", a.dabRecLog);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also write the dynamic labels, with the time, to a text file beside the recording");
        }
        if (!rs.error.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::badRed(), "stopped: %s", rs.error.c_str()); ImGui::PopTextWrapPos(); }
        if (!a.dabRecErr.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::badRed(), "%s", a.dabRecErr.c_str()); ImGui::PopTextWrapPos(); }
        if (au.sub < 0) ImGui::TextDisabled("play a DAB or DAB+ station to record it");
    } else {
        if (ImGui::Button("Stop")) a.engine.dabAudio().stopRecording();
        ImGui::SameLine();
        const int sec = (int)rs.seconds;
        ImGui::Text("%02d:%02d   %.1f MB", sec / 60, sec % 60, (double)rs.bytes / 1e6);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", rs.path.c_str());
        ImGui::PopTextWrapPos();
    }
}

static void dabAnnouncementUi(App& a, const DabEnsemble& ens) {
    if (ImGui::Checkbox("Switch to announcements", &a.dabAnnOn)) plat::prefs().setB("dabAnnOn", a.dabAnnOn);
    ImGui::SameLine();
    if (ImGui::SmallButton("types...")) ImGui::OpenPopup("dabanntypes");
    if (ImGui::BeginPopup("dabanntypes")) {
        for (int t = 0; t < kDabAnnTypes; t++) {
            bool on = (a.dabAnnMask >> t) & 1;
            if (ImGui::Checkbox(dabAnnouncementTypeName(t), &on)) { a.dabAnnMask = on ? (a.dabAnnMask | (1u << t)) : (a.dabAnnMask & ~(1u << t)); plat::prefs().setI("dabAnnMask", (long)a.dabAnnMask); }
        }
        ImGui::EndPopup();
    }
    if (ImGui::Checkbox("Follow to other frequencies", &a.dabFollow)) plat::prefs().setB("dabFollow", a.dabFollow);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When the signal is lost for a few seconds, look for the station on the other frequencies of its ensemble and on the other ensembles that carry it (frequency information of the broadcast)");
    if (a.dabFollowing) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "signal lost: looking on other frequencies"); ImGui::PopTextWrapPos(); }
    else if (a.dabFollow && !a.dabAlts.empty()) ImGui::TextDisabled("%zu alternative frequencies known", a.dabAlts.size());
    if (a.dabAnnActive) {
        std::string who = "sub-channel " + std::to_string(a.dabAnnSub);
        for (const auto& kv : ens.services) for (const auto& c : kv.second.comps) if (c.subId == a.dabAnnSub) who = kv.second.label;
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s: %s", dabAnnouncementTypeName(a.dabAnnType), who.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Back to the station")) {
            a.dabAnnSuspended = true; a.dabAnnActive = false;
            dabPlaySub(a, a.dabUserSub);
        }
    } else if (a.dabAnnSuspended) ImGui::TextDisabled("announcement skipped");
}

void dabStations(App& a) {
    const bool run = a.engine.running();
    dabSpiTick(a);
    const DabEnsemble ens = a.engine.dabEnsemble();
    const int cur = a.engine.dabSelected();
    sectionHeader(Ic::Radio, "Stations");
    const float cardH = 54;
    ImGui::BeginChild("dabsvcs", ImVec2(0, std::max(140.f * gUi, ImGui::GetContentRegionAvail().y * 0.46f)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ens.services.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(run ? "reading the ensemble information..." : "start the receiver to see the stations"); ImGui::PopTextWrapPos(); }
    std::vector<const DabService*> list;
    for (const auto& kv : ens.services) list.push_back(&kv.second);
    for (const DabService* sv : list) {
        const DabComponent* ac = sv->playable();   // the audio, or the DMB video
        const bool sel = ac && ac->subId == cur;
        ImGui::PushID((int)sv->sid);
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##card", ImVec2(w, cardH)) && ac;
        const bool hov = ImGui::IsItemHovered();
        if (clicked) dabSelectStation(a, ac->subId, !sel);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + cardH - 4), sel ? IM_COL32(30, 36, 40, 255) : hov ? IM_COL32(26, 27, 29, 255) : IM_COL32(16, 17, 19, 255), 4.f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + cardH - 4), sel ? pal::remap(IM_COL32(115, 184, 209, 255)) : IM_COL32(48, 50, 52, 255), 4.f, 0, sel ? 1.6f : 1.f);
        icons::draw(sel ? Ic::Speaker : Ic::Radio, ImVec2(p.x + 20, p.y + 15), 17.f, sel ? pal::remap(IM_COL32(120, 200, 255, 255)) : IM_COL32(120, 136, 156, 255), dl);
        const std::string name = sv->label.empty() ? "service " + std::to_string(sv->sid) : sv->label;
        dl->AddText(a.ui, 16.f, ImVec2(p.x + 34, p.y + 6), IM_COL32(245, 247, 250, 255), name.c_str());
        float x = p.x + 10;
        const float ty = p.y + 30;
        if (ac) {
            x += tagAt(dl, ImVec2(x, ty), ac->dmb() ? "DMB" : sv->dabPlus() ? "DAB+" : "DAB", IM_COL32(34, 74, 108, 255)) + 4;
            auto it = ens.subs.find(ac->subId);
            if (it != ens.subs.end() && it->second.bitrate > 0) { char k[24]; snprintf(k, sizeof k, "%d kbit/s", it->second.bitrate); x += tagAt(dl, ImVec2(x, ty), k, IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255)) + 4; }
            if (it != ens.subs.end() && !it->second.eep) tagAt(dl, ImVec2(x, ty), "UEP", IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255));
        } else tagAt(dl, ImVec2(x, ty), "data", IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255));
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::Spacing();
    const DabAudioStats au = a.rx.dab.audio;
    const DmbStats& dmb = a.rx.dab.dmb;
    const bool video = cur >= 0 && dmb.active && dmb.sub == cur;   // a DMB video service: the player shows it
    sectionHeader(Ic::Play, "Player");
    if (cur < 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("click a station to play it"); ImGui::PopTextWrapPos(); }
    else if (video && ImGui::BeginTable("dmbkv", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 88 * gUi);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);   // the values wrap in the rest of the panel
        auto row = [&](Ic ic, const char* k, const char* fmt, auto... args) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(ic, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn();
            char b[120]; snprintf(b, sizeof b, fmt, args...); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos();
        };
        const PlayerStats ps = a.engine.player().stats();
        if (ps.hasVideo) row(Ic::Tv, "Video", "%s  %dx%d  %.1f fps", dmb.video.empty() ? ps.videoCodec.c_str() : dmb.video.c_str(), ps.width, ps.height, ps.fps);
        else row(Ic::Tv, "Video", "%s", dmb.video.empty() ? (dmb.sync ? "reading the stream..." : "searching the packet sync...") : ps.status.c_str());
        if (!dmb.note.empty()) row(Ic::Speaker, "Audio", "%s", dmb.note.c_str());
        else row(Ic::Speaker, "Audio", "%s  %.0f kHz %s", dmb.audio.empty() ? "-" : dmb.audio.c_str(), dmb.sampleRate / 1000.0, dmb.channels == 1 ? "mono" : "stereo");
        row(Ic::Clock, "Buffer", "%.0f ms     A/V %+.0f ms", ps.audioBufferMs, ps.avOffsetMs);
        row(Ic::Layers, "Blocks", "%llu Reed-Solomon ok / %llu lost", (unsigned long long)dmb.rsOk, (unsigned long long)dmb.rsFailed);
        row(Ic::Warning, "Errors", "%llu bytes fixed, %llu pictures late", (unsigned long long)dmb.rsCorrected, (unsigned long long)ps.late);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(a.muted ? Ic::Mute : Ic::Speaker, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Volume"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        float vol = a.volume * 100.f;
        if (ImGui::SliderFloat("##dvol", &vol, 0, 100, "%.0f %%")) { a.volume = vol / 100.f; a.engine.player().setVolume(a.volume); }
        ImGui::EndTable();
    }
    else if (!video && ImGui::BeginTable("dkv", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 88 * gUi);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);   // the values wrap in the rest of the panel
        auto row = [&](Ic ic, const char* k, const char* fmt, auto... args) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(ic, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn();
            char b[120]; snprintf(b, sizeof b, fmt, args...); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos();   // wraps in a narrow column
        };
        row(Ic::Speaker, "Audio", "%s  %dk  %.0f kHz %s", au.codec.empty() ? "-" : au.codec.c_str(), au.bitrate, au.sampleRate / 1000.0, au.channels == 1 ? "mono" : "stereo");
        if (au.aac.valid) {   // what the superframe header says, exactly
            const DabAacParams& p = au.aac;
            row(Ic::Layers, "Profile", "%s", p.profile.c_str());
            row(Ic::Wave, "Rates", "core %.0f kHz%s, output %.0f kHz", p.coreRate / 1000.0, p.sbr ? " + SBR" : "", p.outRate / 1000.0);
            if (p.sbr && au.sbrDecoded) row(Ic::Check, "Decoder", "%s: SBR%s applied", au.decoder.c_str(), au.psDecoded ? " and PS" : "");
            else if (p.sbr) row(Ic::Warning, "Decoder", "%s plays the %.0f kHz core only: no SBR%s (install faad2 / libfaad for full HE-AAC)", au.decoder.c_str(), p.coreRate / 1000.0, p.ps ? " or PS" : "");
            else if (!au.decoder.empty()) row(Ic::Layers, "Decoder", "%s", au.decoder.c_str());
            if (p.sbr && !au.sbrDecoded && !dect2::dabFaadAvailable()) {   // where the library goes, for this system
#ifdef _WIN32
                row(Ic::Info, "libfaad", "%s", "Put libfaad2.dll (a 64-bit build, also named libfaad.dll or faad2.dll) in the folder with OnAir.exe, then restart OnAir.");
#elif defined(__APPLE__)
                row(Ic::Info, "libfaad", "%s", "In Terminal: brew install faad2, then restart OnAir.");
#else
                row(Ic::Info, "libfaad", "%s", "Install the package libfaad2 (sudo apt install libfaad2), then restart OnAir.");
#endif
            }
            row(Ic::Speaker, "Channels", "core %s%s%s", p.coreChannels == 2 ? "stereo" : "mono", p.ps ? " + PS: output stereo" : "", p.surround ? " + MPEG Surround" : "");
        }
        for (const auto& kv : ens.services) {
            const DabComponent* c = kv.second.playable();
            if (!c || c->subId != cur) continue;
            const std::string pg = programmeText(ens, kv.second);
            if (!pg.empty()) row(Ic::Radio, "Programme", "%s", pg.c_str());
        }
        row(Ic::Clock, "Buffer", "%d ms", au.bufferedMs);
        row(Ic::Layers, "Frames", "%llu superframes ok / %llu bad", (unsigned long long)au.superframesOk, (unsigned long long)au.superframesBad);
        row(Ic::Warning, "Errors", "%llu bytes fixed, %llu audio frames bad", (unsigned long long)au.rsCorrected, (unsigned long long)au.auBad);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(a.muted ? Ic::Mute : Ic::Speaker, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Volume"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        float vol = a.volume * 100.f;
        if (ImGui::SliderFloat("##dvol", &vol, 0, 100, "%.0f %%")) { a.volume = vol / 100.f; a.engine.dabAudio().setVolume(a.volume); }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    dabAnnouncementUi(a, ens);
    if (!video) dabRecordUi(a, ens);
    ImGui::Spacing();
    {   // a "now playing" box where the video preview sits in the other modes
        const float vh = std::max(60.f, ImGui::GetContentRegionAvail().y - 30.f);
        ImGui::BeginChild("dabnow", ImVec2(0, vh), ImGuiChildFlags_Borders);
        std::string name = "no station";
        std::string dls;
        std::vector<std::pair<std::string, std::string>> tags;
        uint32_t playSid = 0;
        for (const auto& kv : ens.services) { const DabComponent* ac = kv.second.playable(); if (ac && ac->subId == cur) { name = kv.second.label; dls = kv.second.dls; dlPlusLines(kv.second, tags); playSid = kv.first; } }
        const std::string guideNow = playSid ? dabSpiNowText(a, playSid) : std::string();   // now / next from the programme guide (SPI)
        auto drawTags = [&](float bw) {   // the tagged parts of the label (DL Plus), the plain label as well when it says more
            for (const auto& t : tags) { ImGui::SetCursorPosX(8); ImGui::PushTextWrapPos(bw - 8); ImGui::TextDisabled("%s", t.first.c_str()); ImGui::SameLine(); ImGui::Text("%s", t.second.c_str()); ImGui::PopTextWrapPos(); }
            if (!guideNow.empty()) { ImGui::SetCursorPosX(8); ImGui::PushTextWrapPos(bw - 8); ImGui::TextDisabled("%s", guideNow.c_str()); ImGui::PopTextWrapPos(); }
        };
        if (video && a.video.has()) { a.video.draw(ImGui::GetContentRegionAvail()); ImGui::EndChild(); return; }   // the DMB picture, drawn as the TV modes draw theirs
        dabSlideUpdate(a);
        if (cur >= 0 && gDabSlide.img) {   // the station's picture under its name, as large as the box allows
            const float bw = ImGui::GetContentRegionAvail().x;
            const ImVec2 ns = ImGui::CalcTextSize(name.c_str());
            ImGui::SetCursorPosX(std::max(0.f, (bw - ns.x) * 0.5f));
            ImGui::TextUnformatted(name.c_str());
            const float dlsH = tags.empty() ? (dls.empty() ? 0.f : ImGui::CalcTextSize(dls.c_str(), nullptr, false, std::max(1.f, bw - 16)).y + ImGui::GetStyle().ItemSpacing.y)
                                            : (float)tags.size() * (ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y);
            const ImVec2 av = ImGui::GetContentRegionAvail();
            const float sc = std::min(std::min(av.x / (float)gDabSlide.w, (av.y - dlsH - (guideNow.empty() ? 0.f : 2.f * (ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y))) / (float)gDabSlide.h), 4.f);
            if (sc > 0.f) {
                const ImVec2 sz((float)gDabSlide.w * sc, (float)gDabSlide.h * sc);
                ImGui::SetCursorPosX(std::max(0.f, (bw - sz.x) * 0.5f));
                ImGui::Image(gDabEnh.texture(gDabSlide.img)->texture(), sz);
            }
            drawTags(bw);
            if (!dls.empty() && tags.empty()) { ImGui::SetCursorPosX(8); ImGui::PushTextWrapPos(bw - 8); ImGui::TextDisabled("%s", dls.c_str()); ImGui::PopTextWrapPos(); }
            ImGui::EndChild();
            return;
        }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float cw = ImGui::GetContentRegionAvail().x;
        if (!pal::dev()) icons::draw(Ic::Radio, ImVec2(p.x + cw * 0.5f, p.y + 28), 36.f, cur >= 0 ? pal::remap(IM_COL32(120, 200, 255, 255)) : IM_COL32(70, 80, 92, 255));
        ImGui::Dummy(ImVec2(1 * gUi, (pal::dev() ? 6 : 52) * gUi));
        const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
        ImGui::SetCursorPosX(std::max(0.f, (cw - ts.x) * 0.5f));
        ImGui::TextUnformatted(name.c_str());
        drawTags(cw);
        if (!dls.empty() && tags.empty()) { ImGui::SetCursorPosX(8); ImGui::PushTextWrapPos(cw - 8); ImGui::TextDisabled("%s", dls.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::EndChild();
    }
}

// ------------------------------------------------------------------ tabs
void dabRadioTab(App& a) {
    const DabEnsemble ens = a.engine.dabEnsemble();
    const int cur = a.engine.dabSelected();
    if (ImGui::CollapsingHeader(trw("Played (DL Plus songs)##dabpl"))) { ImGui::BeginChild("##dabplc", ImVec2(0, 240 * gUi)); playedPanel(a, "dabpl"); ImGui::EndChild(); }
    if (ens.services.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "waiting for the ensemble information (a second or two)..." : "start the receiver on a DAB channel (5A to 13F)"); ImGui::PopTextWrapPos(); } return; }
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.ui, 20.f);
    ImGui::TextColored(pal::dev() ? pal::accent() : ImVec4(0.55f, 0.80f, 1.f, 1), "%s", ens.label.empty() ? "DAB ensemble" : ens.label.c_str());
    ImGui::PopFont();
    {   // what does not fit goes on the next line (a narrow tab)
        char info[64]; snprintf(info, sizeof info, "%zu stations   ensemble id %04X", ens.services.size(), ens.eid);
        sameLineIf(ImGui::CalcTextSize(info).x, 12 * gUi);
        ImGui::TextDisabled("%s", info);
    }
    if (ens.utc) {
        time_t t = (time_t)ens.utc; struct tm m; dect2::gmTime(t, &m); char tb[64]; strftime(tb, sizeof tb, "broadcast time %a %d %b %Y %H:%M UTC", &m);
        sameLineIf(ImGui::CalcTextSize(tb).x, 12 * gUi); ImGui::TextDisabled("%s", tb);
    }
    {   // what the FIC says about the ensemble beyond its services: local time, alternative frequencies (FIG 0/21), the ensemble's own network (0/24)
        static DabFicInfo fic; static double ficAt = -10; static uint16_t ficEid = 0;
        if (ficEid != ens.eid || ImGui::GetTime() - ficAt > 2.0) { ficAt = ImGui::GetTime(); ficEid = ens.eid; fic = dabFicInfo(a.engine.dabFic()); }
        char lt[64] = "";
        if (fic.hasLto && ens.utc) {
            time_t t = (time_t)ens.utc + (time_t)fic.ltoHalfHours * 1800; struct tm m; dect2::gmTime(t, &m);
            snprintf(lt, sizeof lt, "local time %02d:%02d (UTC%+d:%02d)", m.tm_hour, m.tm_min, fic.ltoHalfHours / 2, std::abs(fic.ltoHalfHours) % 2 * 30);
        }
        std::string fr;
        int nf = 0;
        for (const auto& f : fic.freqs) for (double k : f.kHz) {
            char b[64]; snprintf(b, sizeof b, "%s%s %.3f MHz", fr.empty() ? "" : "\n", f.rm <= 1 ? "DAB" : f.rm <= 9 && f.rm >= 8 ? "FM" : "other", k / 1000);
            if (nf++ < 24) fr += b;
        }
        if (lt[0]) { sameLineIf(ImGui::CalcTextSize(lt).x, 12 * gUi); ImGui::TextDisabled("%s", lt); }
        if (nf) {
            char b[64]; snprintf(b, sizeof b, "%d alternative frequencies", nf);
            sameLineIf(ImGui::CalcTextSize(b).x, 12 * gUi); ImGui::TextDisabled("%s", b);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", fr.c_str(), nf > 24 ? "\n..." : "");
        }
    }
    ImGui::Spacing();
    smart5DabCard(a);   // round-5 smart checks: ensemble change log, fault or reception (app/smart5)
    if (ImGui::BeginTable("dabtbl", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 34 * gUi);
        ImGui::TableSetupColumn("Station");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Stream");
        ImGui::TableSetupColumn("Id");
        ImGui::TableSetupColumn("Now");
        ImGui::TableHeadersRow();
        for (const auto& kv : ens.services) {
            const DabService& sv = kv.second;
            const DabComponent* ac = sv.playable();
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 26 * gUi);
            ImGui::TableNextColumn();
            ImGui::PushID((int)sv.sid);
            if (ac) {
                const bool sel = ac->subId == cur;
                if (iconFlat(sel ? Ic::Stop : Ic::Play, sel ? "Stop" : "Play this station", sel)) dabSelectStation(a, ac->subId, !sel);
            }
            ImGui::PopID();
            ImGui::PushTextWrapPos(0);   // the cells wrap in a narrow tab
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
            if (dabLogoImage(a, ens, sv.sid, 22 * gUi)) ImGui::SameLine(0, 5 * gUi);
            ImGui::TextUnformatted(sv.label.empty() ? "-" : sv.label.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", ac ? (ac->dmb() ? "DMB (video)" : sv.dabPlus() ? "DAB+ (HE-AAC)" : "DAB (MP2)") : "data");
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", ac ? dabSubText(ens, ac->subId).c_str() : "");
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextDisabled("%04X", sv.sid); ImGui::PopFont();
            ImGui::TableNextColumn(); {
                const std::string dp = dlPlusShort(sv);
                const std::string gd = dabSpiNowText(a, sv.sid);
                ImGui::TextDisabled("%s", dp.empty() ? sv.dls.c_str() : dp.c_str());
                if (!gd.empty()) ImGui::TextDisabled("%s", gd.c_str());   // from the programme guide (SPI)
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
    }
}

void dabEnsembleTab(App& a) {
    const DabTelemetry& d = a.rx.dab;
    const DabEnsemble ens = a.engine.dabEnsemble();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("DAB receiver, transmission mode I: null symbol and phase reference sync, DQPSK, fast information channel, EEP sub-channels, DAB+ superframes"); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    auto row = [&](const char* k, const char* fmt, auto... v) {   // a long value wraps under itself in a narrow tab
        ImGui::TextDisabled("%s", k); ImGui::SameLine(std::min(190 * gUi, ImGui::GetContentRegionAvail().x * 0.45f));
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    row("frame sync", "%s", d.state == 2 ? "locked" : "searching");
    row("phase reference", "correlation peak %.0f times the noise floor", d.cirPeak);
    row("carrier offset", "%+.1f Hz", d.cfoHz);
    row("signal quality", "%.1f dB (DQPSK cluster scatter)", d.snrDb);
    row("frames", "%llu (96 ms each)", (unsigned long long)d.frames);
    row("fast information", "%llu blocks ok, %llu bad, %d of 12 ok in the last frame", (unsigned long long)d.fibOk, (unsigned long long)d.fibBad, d.ficRecentOk);
    row("ensemble", "%s  (id %04X)", ens.label.c_str(), ens.eid);
    ImGui::Spacing();
    sectionHeader(Ic::Layers, "Sub-channels");
    if (ImGui::BeginTable("dsub", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Id"); ImGui::TableSetupColumn("Start (CU)"); ImGui::TableSetupColumn("Size (CU)"); ImGui::TableSetupColumn("Protection"); ImGui::TableSetupColumn("kbit/s"); ImGui::TableSetupColumn("Carries");
        ImGui::TableHeadersRow();
        for (const auto& kv : ens.subs) {
            const DabSubchannel& s = kv.second;
            std::string who;
            for (const auto& sv : ens.services) for (const auto& c : sv.second.comps) if (c.subId == s.id) who += (who.empty() ? "" : ", ") + sv.second.label;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", s.id);
            ImGui::TableNextColumn(); ImGui::Text("%d", s.start);
            ImGui::TableNextColumn(); ImGui::Text("%d", s.size);
            ImGui::TableNextColumn(); ImGui::Text(s.eep ? "EEP %d-%c" : "UEP %d", s.eep ? s.level + 1 : s.uepIndex, s.option ? 'B' : 'A');
            ImGui::TableNextColumn(); ImGui::Text("%d", s.bitrate);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(who.c_str());
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ the census: every ensemble of a wide capture at once
static void dabCensusSection(App& a) {
    bool on = a.engine.dabCensusOn();
    if (ImGui::Checkbox(tr("Every ensemble in the capture"), &on)) a.engine.setDabCensus(on);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("With a capture of several MHz: reads every Band III ensemble in it at once, no scanning. Click a channel to listen."));
    if (!on) return;
    const auto rows = a.engine.dabCensusRows();
    if (rows.empty()) { ImGui::TextDisabled("%s", tr("No ensemble in the capture yet (needs at least 3 MHz of Band III).")); return; }
    if (ImGui::BeginTable("##dabcens", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("Channel")); ImGui::TableSetupColumn(tr("Ensemble")); ImGui::TableSetupColumn(tr("Services")); ImGui::TableSetupColumn(tr("SNR")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("TII")); ImGui::TableHeadersRow();
        int id = 0;
        for (const auto& r : rows) {
            ImGui::PushID(id++); ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (ImGui::SmallButton(r.channel.c_str())) tuneFreq(a, r.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.ensemble.empty() ? (r.locked ? tr("reading") : tr("searching")) : r.ensemble.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%zu", r.services.size());
            ImGui::TableNextColumn(); if (r.locked) ImGui::Text("%.1f dB", r.snrDb); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.0f dBFS", r.levelDb);
            ImGui::TableNextColumn(); if (!r.tii.empty()) ImGui::Text("%d/%d", r.tii[0].first, r.tii[0].second); else ImGui::TextDisabled("-");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const auto merged = dect2::dabCensusMerge(rows);
    if (!merged.empty() && ImGui::TreeNode(trw("All services"))) {
        int id = 0;
        for (const auto& m : merged) {
            ImGui::PushID(id++);
            if (ImGui::SmallButton(m.channel.c_str())) tuneFreq(a, m.mhz);
            ImGui::SameLine(); ImGui::Text("%s", m.label.empty() ? "?" : m.label.c_str());
            if (!m.alsoOn.empty()) { std::string o; for (const auto& c : m.alsoOn) { o += o.empty() ? "" : ", "; o += c; } ImGui::SameLine(); ImGui::TextDisabled("%s %s", tr("also"), o.c_str()); }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::Separator();
}

// ------------------------------------------------------------------ scanner (Band III, one ensemble per channel)
// First start: what the Scan button does (docs/EASE-3.8.md)
void dabAutoScan(App& a) { App::DabScan& s = a.dabScan; s.running = true; s.idx = -1; s.t0 = 0; s.results.clear(); s.savedFreq = a.freqMhz; dabSelectStation(a, -1, false); }

void dabScanTab(App& a) {
    App::DabScan& s = a.dabScan;
    dabCensusSection(a);
    const bool run = a.engine.running();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Tunes through the 38 Band III channels (5A to 13F) and lists the ensembles with their stations. Channels without DAB are left after 0.7 s, so it takes 30 s to a minute."); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    if (!s.running) {
        ImGui::BeginDisabled(!run);
        if (iconButton(Ic::Scan, "Scan Band III", pal::remap(IM_COL32(32, 96, 140, 255)), pal::remap(IM_COL32(44, 124, 178, 255)))) {
            s.running = true; s.idx = -1; s.t0 = 0; s.results.clear(); s.savedFreq = a.freqMhz;
            dabSelectStation(a, -1, false);
        }
        ImGui::EndDisabled();
        if (!run) { ImGui::SameLine(); ImGui::TextDisabled("start the receiver first"); }
    } else {
        if (iconButton(Ic::Stop, "Stop scan", IM_COL32(112, 48, 48, 255), IM_COL32(146, 62, 62, 255))) { s.running = false; dabTuneMhz(a, s.savedFreq); }   // back where it was (the radio too, not only the box)
        ImGui::SameLine(0, 12 * gUi);
        ImGui::TextDisabled("channel %d of %d", std::max(0, s.idx) + 1, kNumDabCh);
        ImGui::ProgressBar((float)(std::max(0, s.idx)) / kNumDabCh, ImVec2(220, ImGui::GetFrameHeight() - 6));
    }
    ImGui::Spacing();
    {
        static std::string note;
        resexp::Table t; t.cols = {"time", "frequency_hz", "mode", "channel", "ensemble", "snr_db", "stations"};
        const std::string now = resexp::stamp();
        for (const auto& r : s.results) if (r.found) t.add({now, resexp::num(r.mhz * 1e6, 0), "DAB", r.name, r.label, resexp::num(r.snr, 1), r.stations});
        resexp::saveButton(t, "dab", note, "svdab");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    if (ImGui::BeginTable("dscan", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 90 * gUi); ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableSetupColumn("Ensemble"); ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 70 * gUi); ImGui::TableSetupColumn("Stations");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.results.size(); i++) {
            const auto& r = s.results[i];
            if (!r.found) continue;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 26 * gUi);
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            if (ImGui::Selectable(r.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                dabTuneMhz(a, r.mhz);
                s.running = false;
            }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::Text("%.3f", r.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.label.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", r.snr);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.stations.c_str());
        }
        ImGui::EndTable();
    }
    if (!s.running && s.results.empty()) ImGui::TextDisabled("no scan yet");
}

// The scan state machine, called every frame
void dabScanStep(App& a) {
    App::DabScan& s = a.dabScan;
    dabTiiCollect(a);
    if (!s.running) return;
    if (!a.engine.running()) { s.running = false; return; }
    const double now = ImGui::GetTime();
    auto tuneTo = [&](int idx) {
        dabTuneMhz(a, kDabCh[idx].mhz);
        s.idx = idx; s.t0 = now; s.lockT = 0;
    };
    if (s.idx < 0) { tuneTo(0); return; }
    const DabTelemetry& d = a.rx.dab;
    const bool locked = a.rx.standard == 3 && d.state == 2;
    if (locked && s.lockT == 0) s.lockT = now;
    const DabEnsemble ens = a.engine.dabEnsemble();
    bool done = false;
    // finished when the ensemble is known with all labels, or a few seconds after sync, or no sync after the search time
    bool labelled = ens.valid && !ens.label.empty();
    for (const auto& kv : ens.services) if (kv.second.label.empty()) labelled = false;
    // the transmitter identification (TII) needs a few seconds of null symbols after the sync: wait for it, at most 7 s
    if (locked && labelled && now - s.lockT > 0.6 && (d.tiiFrames >= 40 || now - s.lockT > 5.0)) done = true;
    else if (locked && now - s.lockT > 7.0) done = true;
    else if (!locked && now - s.t0 > 2.2) done = true;
    // nothing like DAB (no null symbol in 0.7 s of input: noise, another service) moves on at once; the bound on elapsed time keeps a value
    // left from the channel before out
    else if (!locked && now - s.t0 > 0.5 && d.quietMs > 700 && d.quietMs < (now - s.t0) * 1000 + 100) done = true;
    if (!done) return;
    App::DabScan::Res r;
    r.name = kDabCh[s.idx].name; r.mhz = kDabCh[s.idx].mhz; r.found = locked && ens.valid;
    if (r.found) {
        r.label = ens.label; r.snr = (float)d.snrDb;
        for (const auto& kv : ens.services) { if (!r.stations.empty()) r.stations += ", "; r.stations += kv.second.label.empty() ? "?" : kv.second.label; }
        a.engine.log("DAB scan: " + r.name + " " + ens.label + " (" + std::to_string(ens.services.size()) + " stations)");
    }
    s.results.push_back(r);
    if (s.idx + 1 >= kNumDabCh) { s.running = false; dabTuneMhz(a, s.savedFreq); a.engine.log("DAB scan finished"); }
    else tuneTo(s.idx + 1);
}


// ------------------------------------------------------------------ Journaline (ETSI TS 102 979)
// The services that signal the Journaline user application (FIG 0/13, type 0x44a): in the PAD of an audio component (the audio keeps playing)
// or in a packet mode component (FIG 0/3 gives its sub-channel and packet address).
static bool dabJournalineSource(const DabEnsemble& ens, const DabService& sv, int& sub, int& addr) {
    if (!sv.journaline || sv.jlScIdS < 0 || sv.jlScIdS >= (int)sv.comps.size()) return false;
    const DabComponent& c = sv.comps[(size_t)sv.jlScIdS];
    if (c.tmid == 0) { sub = c.subId; addr = -1; return true; }   // the PAD of the audio
    if (c.tmid == 3) {
        auto it = ens.packet.find(c.scid);
        if (it == ens.packet.end() || it->second.subId < 0) return false;
        sub = it->second.subId; addr = it->second.address;
        return true;
    }
    return false;
}

bool dabHasJournaline(const DabEnsemble& ens) {
    for (const auto& kv : ens.services) if (kv.second.journaline) return true;
    return false;
}

void dabJournalineTab(App& a) {
    const DabEnsemble ens = a.engine.dabEnsemble();
    static uint32_t chosen = 0;
    static bool have = false;
    static JlViewState jv;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Journaline: text and menus ('news and information'), ETSI TS 102 979. Pick a service; the pages arrive in a carousel, so a page can take a while to show up. Packet mode services need no audio; for a station with Journaline in its PAD the audio plays as well.");
    ImGui::PopTextWrapPos();
    int shown = 0;
    for (const auto& kv : ens.services) {
        const DabService& sv = kv.second;
        int sub, addr;
        if (!sv.journaline || !dabJournalineSource(ens, sv, sub, addr)) continue;
        const bool on = have && chosen == sv.sid && a.engine.dabSelected() == sub;
        ImGui::PushID((int)sv.sid);
        char l[160]; snprintf(l, sizeof l, "%s%s  (%04X, %s)", on ? "[open] " : "", sv.label.empty() ? "service" : sv.label.c_str(), sv.sid, addr < 0 ? "PAD" : "packet mode");
        if (ImGui::Selectable(l, on) && !on) {
            chosen = sv.sid; have = true;
            a.dabUserSub = addr < 0 ? sub : -1;
            a.engine.dabSelectJournaline(sub, addr, sv.jlExt);
            if (addr < 0) { a.engine.dabAudio().setVolume(a.volume); a.engine.dabAudio().setMuted(a.muted); }
            jv.reset(sv.jlEntry);
        }
        ImGui::PopID();
        shown++;
    }
    if (!shown) { ImGui::TextDisabled("No service of this ensemble has a Journaline component that can be decoded yet."); return; }
    if (have) {
        auto it = ens.services.find(chosen);
        if (it != ens.services.end() && a.engine.dabSelected() >= 0) { ImGui::Separator(); journalineView("dabjl", a.engine.dabJournaline(), jv, it->second.jlEntry); }
    }
}
