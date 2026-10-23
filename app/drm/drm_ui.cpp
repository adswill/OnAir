// DRM screens: the Radio tab (the band presets, short wave and the VHF bands of DRM+, now playing, text message, time, signal summary), the services
// list, status lamps, the analysis plots (FAC / SDC / MSC cells, channel, SNR), the Receiver tab with all telemetry and the options of the test signal.
#include "app.h"
#include "panels/gallery.h"
#include "panels/enhance_ui.h"
#include "panels/journaline_view.h"
#include "codec/speech_codec_ui.h"
#include "dect2/drm/drm_usac_lib.h"
#include <cmath>
#include <ctime>
#include <deque>

bool freqStepArrow(const char* id, ImGuiDir dir);

namespace {

constexpr int kStd = 11;   // RxTelemetry::standard of DRM (the tuning table says 12, the engine reports 11)

struct State {
    bool wasRunning = false;
    float pushedVol = -1;
    bool pushedMute = false;
    int sel = -1;                     // short id the user picked, -1: the first audio service
    int pushedSel = -2;
    int dataSel = -1;                 // short id of the data service shown instead of the audio one, -1 none
    gfx::Image* img = nullptr;        // the slide show picture of that service
    uint64_t imgSeq = 0;
    int imgShort = -1;
    enhui::LiveTex enh;               // the Enhance control on the slide
    int objSel = -1;                  // the object of a website / EPG service whose text is shown
    std::string objSelName;
    uint64_t lastSeq = 0;
    std::deque<float> snr, audio;     // SNR of the cells and share of good audio frames, a point per report
    uint64_t prevFacOk = 0, prevFacBad = 0, prevSdcOk = 0, prevSdcBad = 0, prevAudOk = 0, prevAudBad = 0;
    double facOkAt = -1e9, facBadAt = -1e9, sdcOkAt = -1e9, sdcBadAt = -1e9, audOkAt = -1e9, audBadAt = -1e9;   // when the last good / bad block was counted
};
State S;

bool live(const App& a) { return a.rx.standard == kStd; }

void push(std::deque<float>& d, float v) { d.push_back(v); if (d.size() > 600) d.pop_front(); }

const ImVec4 kGood(0.40f, 0.85f, 0.50f, 1), kWarn(0.95f, 0.60f, 0.25f, 1), kBad(0.95f, 0.40f, 0.35f, 1);

// ---- text helpers

const char* modeText(int m) { static const char* n[] = {"A", "B", "C", "D", "E"}; return m >= 0 && m < 5 ? n[m] : "-"; }

std::string khz(float k) { char b[24]; snprintf(b, sizeof b, "%g kHz", (double)k); return b; }

std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

std::string serviceName(const DrmService& s) {
    if (!s.label.empty()) return s.label;
    char b[40]; snprintf(b, sizeof b, "service %d (%06X)", s.shortId + 1, s.id & 0xFFFFFF); return b;
}

std::string serviceFacts(const DrmService& s) {
    std::string r = s.audio ? (s.programmeName.empty() ? "audio" : s.programmeName) : s.appName.empty() ? "data" : s.appName;
    if (!s.languageName.empty() && s.languageName != "-") r += ", " + s.languageName;
    if (!s.country.empty()) r += ", " + upper(s.country);
    return r;
}

const DrmService* playing(const DrmTelemetry& t) {
    for (const auto& s : t.services) if (s.shortId == t.serviceAudioSelected) return &s;
    for (const auto& s : t.services) if (s.audio) return &s;
    return t.services.empty() ? nullptr : &t.services[0];
}

// 0 Sunday: the SDC gives a date, not a weekday
const char* weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    static const char* n[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (m < 1 || m > 12) return "";
    if (m < 3) y--;
    return n[(y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7];
}

std::string timeText(const DrmTelemetry& t) {
    if (!t.timeValid) return "-";
    char b[96];
    snprintf(b, sizeof b, "%s %04d-%02d-%02d %02d:%02d UTC", weekday(t.year, t.month, t.day), t.year, t.month, t.day, t.hour, t.minute);
    std::string r = b;
    if (t.hasLocalOffset) {
        const int o = t.localOffsetHalfHours, m = std::abs(o) * 30;
        snprintf(b, sizeof b, "  (local %c%d:%02d)", o < 0 ? '-' : '+', m / 60, m % 60);
        r += b;
    }
    return r;
}

const char* stateText(int s) { return s == 2 ? "decoding" : s == 1 ? "synchronised, reading the FAC" : "searching"; }

// one lamp from the times of the last good and bad block: green while good ones keep coming, amber when bad ones mix in, red when only bad ones come
int blockLamp(double okAt, double badAt, double window, double now) {
    const bool ok = now - okAt < window, bad = now - badAt < window;
    return ok && bad ? 2 : ok ? 1 : bad ? 3 : 0;
}

// ---------------------------------------------------------------- tick

void tick(App& a) {
    const bool run = a.engine.running();
    if (run && a.rx.standard == kStd) {
        if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
            a.engine.drm().setVolume(a.volume);
            a.engine.drm().setMuted(a.muted);
            S.pushedVol = a.volume; S.pushedMute = a.muted;
        }
    }
    if (run && (!S.wasRunning || S.sel != S.pushedSel)) { a.engine.drm().selectService(S.sel); S.pushedSel = S.sel; }
    S.wasRunning = run;
    if (run && a.rx.standard == kStd) {   // slide show pictures go to the gallery (View > Images), every one that completes
        int sid = 0;
        dect2::DrmSlide sl;
        while (a.engine.drm().takeSlide(sid, sl)) {
            if (sl.w <= 0 || sl.h <= 0 || sl.rgba.size() < (size_t)sl.w * (size_t)sl.h * 4) continue;
            gallery::Meta gm;
            gm.detail = sl.caption.empty() ? sl.name : sl.caption;
            gallery::publish("drm-slide-" + std::to_string(sid) + "-" + (sl.name.empty() ? std::to_string(sl.seq) : sl.name), "DRM slide show", (double)time(nullptr), sl.w, sl.h, sl.rgba.data(), gm);
        }
    }
    if (!live(a) || a.rx.seq == S.lastSeq) return;
    S.lastSeq = a.rx.seq;
    const DrmTelemetry& t = a.rx.drm;
    const double now = ImGui::GetTime();
    auto count = [&](uint64_t cur, uint64_t& prev, double& at) { if (cur < prev) prev = 0; if (cur > prev) at = now; prev = cur; };
    count(t.facOk, S.prevFacOk, S.facOkAt); count(t.facBad, S.prevFacBad, S.facBadAt);
    count(t.sdcOk, S.prevSdcOk, S.sdcOkAt); count(t.sdcBad, S.prevSdcBad, S.sdcBadAt);
    const uint64_t okBefore = S.prevAudOk, badBefore = S.prevAudBad;
    count(t.blocksOk, S.prevAudOk, S.audOkAt); count(t.blocksBad, S.prevAudBad, S.audBadAt);
    if (t.snrDb > 0 || t.state > 0) push(S.snr, t.snrDb);
    const uint64_t dOk = S.prevAudOk >= okBefore ? S.prevAudOk - okBefore : 0, dBad = S.prevAudBad >= badBefore ? S.prevAudBad - badBefore : 0;
    if (dOk + dBad > 0) push(S.audio, 100.f * (float)dOk / (float)(dOk + dBad));
}

// ---------------------------------------------------------------- the Radio tab

void kvRow(const App& a, const char* k, const std::string& v, float x = 110.f) {
    ImGui::TextDisabled("%s", k); kvColumn(x * gUi);   // a narrow pane: the column moves left and the value wraps
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

// ---- data services: MOT slide show and broadcast website, EPG, TPEG (ES 201 980 clause 6.6; the application is named by the SDC)

std::string bytesText(uint64_t n) { char b[32]; if (n >= 10240) snprintf(b, sizeof b, "%.1f kB", n / 1024.0); else snprintf(b, sizeof b, "%llu B", (unsigned long long)n); return b; }

void slideImage(const dect2::DrmDataView& v) {
    if (v.slide.seq == 0) return;
    if (S.imgSeq != v.slide.seq || S.imgShort != v.shortId) {
        delete S.img; S.img = nullptr;
        if (gGfx && v.slide.w > 0 && v.slide.h > 0 && v.slide.rgba.size() >= (size_t)v.slide.w * (size_t)v.slide.h * 4) {
            S.img = gGfx->createImage(v.slide.w, v.slide.h, 0xFF000000u);
            S.img->update(0, 0, v.slide.w, v.slide.h, reinterpret_cast<const uint32_t*>(v.slide.rgba.data()));
            S.enh.e.setSource("DRM slide show");
            S.enh.feed(reinterpret_cast<const uint32_t*>(v.slide.rgba.data()), v.slide.w, v.slide.h, true);
        }
        S.imgSeq = v.slide.seq; S.imgShort = v.shortId;
    }
}

void dataPanel(App& a, float w, float h, const DrmService& s) {
    using namespace dect2;
    DrmDataView v;
    const bool have = a.engine.drm().dataView(s.shortId, v);
    ImGui::BeginChild("##drm_data", ImVec2(w, h), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.ui, 26.f);
    ImGui::TextColored(pal::dev() ? pal::accent() : ImVec4(0.55f, 0.80f, 1.f, 1), "%s", serviceName(s).c_str());
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s: %s", s.appName.empty() ? "data service" : s.appName.c_str(), s.appMode.empty() ? "waiting for the application information" : s.appMode.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!have) { ImGui::TextDisabled("waiting for the application information (SDC)"); ImGui::EndChild(); return; }
    char b[200];
    ImGui::PushTextWrapPos(0);
    if (!v.packet) {
        ImGui::TextColored(kWarn, "Stream mode data, not decoded: this receiver reads the packet mode applications only.");
    } else if (!v.dataUnit && v.userApp != 0x44A) {
        ImGui::TextColored(kWarn, "Single packet mode (not data units): not decoded.");
    } else if (v.journaline) {
        ImGui::TextDisabled("Journaline is shown in the Receiver tab (section Journaline).");
    } else if (!v.decoded) {
        ImGui::TextColored(kWarn, "%s: this application is not decoded. The packets are counted below.", v.appName.c_str());
    } else if (v.userApp == 0x002) {
        slideImage(v);
        if (v.slide.seq == 0) ImGui::TextDisabled("no picture received yet (the carousel sends one object after the other)");
        else if (S.img) {
            const float sc = std::max(0.1f, std::min(std::min((ImGui::GetContentRegionAvail().x - 4 * gUi) / (float)v.slide.w, std::max(80.f * gUi, h * 0.45f) / (float)v.slide.h), 8.f));
            ImGui::Image(S.enh.texture(S.img)->texture(), ImVec2((float)v.slide.w * sc, (float)v.slide.h * sc));
            S.enh.controls("drmenh");
        }
        if (v.slide.seq) {
            if (!v.slide.caption.empty()) { ImGui::PushFont(a.ui, 18.f); ImGui::TextUnformatted(v.slide.caption.c_str()); ImGui::PopFont(); }
            snprintf(b, sizeof b, "%s, %d x %d, %s", v.slide.type.c_str(), v.slide.w, v.slide.h, bytesText(v.slide.bytes).c_str());
            kvRow(a, "picture", b);
            if (!v.slide.name.empty()) kvRow(a, "name", v.slide.name);
            if (!v.slide.category.empty() || v.slide.categoryId >= 0) {
                std::string cat = v.slide.category;
                if (v.slide.categoryId >= 0) cat += (cat.empty() ? "" : "  ") + std::string("(category ") + std::to_string(v.slide.categoryId) + ", slide " + std::to_string(v.slide.slideId) + ")";
                kvRow(a, "category", cat);
            }
            if (!v.slide.trigger.empty()) kvRow(a, "trigger time", v.slide.trigger);
            if (!v.slide.clickUrl.empty()) kvRow(a, "link", v.slide.clickUrl + "  (shown only, never opened)");
            if (!v.slide.altUrl.empty()) kvRow(a, "alternative", v.slide.altUrl);
            snprintf(b, sizeof b, "%llu picture%s received; every picture is saved to View > Images", (unsigned long long)v.slide.seq, v.slide.seq == 1 ? "" : "s");
            ImGui::TextDisabled("%s", b);
        }
    } else if (v.userApp == 0x004) {
        snprintf(b, sizeof b, "%llu frames, %s in %llu data groups", (unsigned long long)v.tpegFrames, bytesText(v.tpegBytes).c_str(), (unsigned long long)v.mot.groups);
        kvRow(a, "TPEG", b);
        ImGui::TextDisabled("Traffic and travel messages are identified and counted; the TPEG content (TS 102 818) is not decoded.");
    } else {   // 0x003 broadcast website, 0x007 EPG: the object list
        ImGui::TextDisabled(v.userApp == 0x003 ? "Web pages are shown as text: nothing is rendered or run, links are not followed." : "EPG files in the clear (XML) are shown as text; binary coded ones are only listed.");
        if (v.objects.empty()) ImGui::TextDisabled("no object received yet");
        else if (ImGui::BeginTable("##drm_obj", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, std::max(70.f * gUi, h * 0.28f)))) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 3.f);
            ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthStretch, 1.f);
            ImGui::TableSetupColumn("seen", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < v.objects.size(); i++) {
                const DrmObjectInfo& o = v.objects[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID((int)i);
                if (ImGui::Selectable(o.name.c_str(), S.objSelName == o.name, ImGuiSelectableFlags_SpanAllColumns)) { S.objSel = (int)i; S.objSelName = o.name; }
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(o.type.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(bytesText(o.size).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llux", (unsigned long long)o.received);
            }
            ImGui::EndTable();
        }
        const DrmObjectInfo* sel = nullptr;
        for (const DrmObjectInfo& o : v.objects) if (o.name == S.objSelName) sel = &o;
        if (!sel && !v.objects.empty()) sel = &v.objects.back();
        if (sel) {
            ImGui::TextDisabled("%s%s%s", sel->name.c_str(), sel->note.empty() ? "" : "  -  ", sel->note.c_str());
            if (sel->text) {
                ImGui::BeginChild("##drm_obj_text", ImVec2(0, std::max(40.f * gUi, ImGui::GetContentRegionAvail().y - 3 * ImGui::GetTextLineHeightWithSpacing())), ImGuiChildFlags_Borders);
                ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(sel->content.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
                ImGui::EndChild();
            } else ImGui::TextDisabled("not text: %s, %s", sel->type.c_str(), bytesText(sel->size).c_str());
        }
    }
    ImGui::PopTextWrapPos();
    if (v.packet && v.dataUnit && v.packetLen > 0) {
        snprintf(b, sizeof b, "%llu ok, %llu bad CRC, %llu lost, %llu orphan", (unsigned long long)v.packets, (unsigned long long)v.badPackets, (unsigned long long)v.lostPackets, (unsigned long long)v.orphanPackets);
        kvRow(a, "packets", b, 80);
        snprintf(b, sizeof b, "%llu ok, %llu dropped", (unsigned long long)v.unitsOk, (unsigned long long)v.unitsDropped);
        kvRow(a, "data units", b, 80);
    }
    ImGui::EndChild();
}

void nowPlaying(App& a, float w, float h) {
    const DrmTelemetry& t = a.rx.drm;
    const bool on = live(a);
    const DrmService* s = on ? playing(t) : nullptr;
    if (on && S.dataSel >= 0) {
        for (const auto& d : t.services) if (d.shortId == S.dataSel && !d.audio) { dataPanel(a, w, h, d); return; }
    }
    ImGui::BeginChild("##drm_now", ImVec2(w, h), ImGuiChildFlags_Borders);
    if (!on || !s) {
        ImGui::PushFont(a.ui, 22.f);
        { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(!a.engine.running() ? "stopped" : t.mode >= 0 ? "reading the multiplex" : "searching for a DRM signal"); ImGui::PopTextWrapPos(); }
        ImGui::PopFont();
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? (t.status.empty() ? "" : t.status.c_str()) : "Start the receiver. Test signal: pick the synthetic source."); ImGui::PopTextWrapPos(); }
        ImGui::EndChild();
        return;
    }
    ImGui::PushFont(a.ui, 26.f);
    ImGui::TextColored(pal::dev() ? pal::accent() : ImVec4(0.55f, 0.80f, 1.f, 1), "%s", serviceName(*s).c_str());
    ImGui::PopFont();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", s->audio ? "audio service" : s->appName.empty() ? "data service" : (s->appName + ", " + s->appMode).c_str()); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    kvRow(a, "programme", s->programmeName.empty() ? "-" : s->programmeName);
    kvRow(a, "language", s->languageName.empty() ? "-" : s->languageName);
    kvRow(a, "country", s->country.empty() ? "-" : upper(s->country));
    char b[64];
    snprintf(b, sizeof b, "%06X", s->id & 0xFFFFFF);
    kvRow(a, "service id", b);
    kvRow(a, "audio", s->codecText.empty() ? "-" : s->codecText);
    if (s->bitrateBps > 0) { snprintf(b, sizeof b, "%.1f kbit/s", s->bitrateBps / 1000.0); kvRow(a, "stream rate", b); }
    ImGui::TextDisabled("state"); kvColumn(110 * gUi);
    const ImVec4 col = t.audioState == 2 ? kGood : t.audioState == 1 ? kWarn : ImVec4(0.62f, 0.65f, 0.68f, 1);
    { ImGui::PushTextWrapPos(0); ImGui::TextColored(col, "%s", t.audioState == 2 ? "playing" : t.audioState == 1 ? "frames received, cannot decode" : s->audio ? "waiting for audio" : "no audio"); ImGui::PopTextWrapPos(); }
    ImGui::PushTextWrapPos(0);
    if (!t.audioInfo.empty() && t.audioState != 0 && t.audioInfo != s->codecText) ImGui::TextDisabled("%s", t.audioInfo.c_str());
    if (t.holding) ImGui::TextColored(kWarn, "Fade: the receiver holds the synchronisation and bridges the sound until the signal comes back.");
    else if (t.audioState == 2 && t.audioConcealedPct >= 0.5f) ImGui::TextColored(t.audioConcealedPct > 10 ? kWarn : ImVec4(0.62f, 0.65f, 0.68f, 1), "%.0f %% of the sound in the last 10 s was concealed (lost frames)", t.audioConcealedPct);
    if (t.hierarchicalUnsupported) ImGui::TextColored(kWarn, "The signal uses a hierarchical mapping, which this receiver does not decode.");
    if (s->conditionalAccess) ImGui::TextColored(kWarn, "Conditional access is flagged: the stream may be scrambled.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    sectionHeader(Ic::Radio, "Text message");
    ImGui::PushTextWrapPos(0);
    if (!s->textMessage) ImGui::TextDisabled("this service sends none");
    else if (t.textMessage.empty()) ImGui::TextDisabled("waiting for the first segment");
    else ImGui::TextUnformatted(t.textMessage.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void signalPanel(App& a, float w, float h) {
    const DrmTelemetry& t = a.rx.drm;
    const bool on = live(a) && t.mode >= 0;
    ImGui::BeginChild("##drm_sig", ImVec2(w, h), ImGuiChildFlags_Borders);
    sectionHeader(Ic::Wave, "Signal");
    char b[96];
    kvRow(a, "mode", on ? std::string("robustness ") + modeText(t.mode) : "-", 100);
    kvRow(a, "occupancy", on && t.occupancy >= 0 ? khz(t.bandwidthKhz) : "-", 100);
    kvRow(a, "interleaver", on && t.longInterleave >= 0 ? (t.longInterleave ? "long (2 s)" : "short (400 ms)") : "-", 100);
    if (on && t.mscQam) snprintf(b, sizeof b, "%d-QAM  (SDC %d-QAM)", t.mscQam, t.sdcQam); else snprintf(b, sizeof b, "-");
    kvRow(a, "MSC", b, 100);
    if (on && t.numStreams > 0) {
        const bool two = t.streamLenA[0] > 0;   // part A exists: unequal error protection
        if (two && t.protRateA > 0) snprintf(b, sizeof b, "A %d (%d%%), B %d (%d%%)", t.protA, t.protRateA, t.protB, t.protRateB);
        else if (two) snprintf(b, sizeof b, "A %d, B %d", t.protA, t.protB);
        else if (t.protRateB > 0) snprintf(b, sizeof b, "level %d (rate %d%%)", t.protB, t.protRateB);
        else snprintf(b, sizeof b, "level %d", t.protB);
    } else snprintf(b, sizeof b, "-");
    kvRow(a, "protection", b, 100);
    snprintf(b, sizeof b, on ? "%.1f dB" : "-", t.snrDb); kvRow(a, "SNR", b, 100);
    snprintf(b, sizeof b, on ? "%+.1f Hz" : "-", t.cfoHz); kvRow(a, "carrier offset", b, 108);
    ImGui::Spacing();
    sectionHeader(Ic::Clock, "Time from the signal");
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(on ? timeText(t).c_str() : "-"); ImGui::PopFont();
    ImGui::EndChild();
}

// Band presets: DRM30 in the short wave broadcast bands, DRM+ (mode E, 100 kHz channels) in VHF bands I, II and III; in VHF the arrows step by the
// 100 kHz DRM+ raster. The receiver finds the robustness mode by itself (DRM+ needs a radio rate of 100 ksps or more).
void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Bands"); flowNext(6 * gUi);
    static const struct { const char* name; double mhz; const char* tip; } kP[] = {
        {"SW 41 m", 7.325, "DRM30, short wave 41 m band (7.2 - 7.45 MHz)"},
        {"SW 31 m", 9.5, "DRM30, short wave 31 m band (9.4 - 9.9 MHz)"},
        {"SW 25 m", 11.65, "DRM30, short wave 25 m band (11.6 - 12.1 MHz)"},
        {"SW 19 m", 15.1, "DRM30, short wave 19 m band (15.1 - 15.8 MHz)"},
        {"VHF I", 47.1, "DRM+, VHF band I (47 - 68 MHz)"},
        {"VHF II", 87.6, "DRM+, VHF band II (87.5 - 108 MHz), next to the FM stations"},
        {"VHF III", 174.1, "DRM+, VHF band III (174 - 240 MHz), between the DAB blocks"},
    };
    for (const auto& p : kP) {
        if (ImGui::Button(p.name)) tuneFreq(a, p.mhz);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %.3f MHz", p.tip, p.mhz);
        flowNext(6 * gUi);
    }
    const bool vhf = a.freqMhz >= 30;
    if (vhf) {
        if (freqStepArrow("##drmdn", ImGuiDir_Left)) tuneFreq(a, std::round(a.freqMhz * 10 - 1) / 10);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("100 kHz down (the DRM+ channel raster). Hold for a continuous change.");
        flowNext(4 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%.1f MHz", a.freqMhz);
        flowNext(4 * gUi);
        if (freqStepArrow("##drmup", ImGuiDir_Right)) tuneFreq(a, std::round(a.freqMhz * 10 + 1) / 10);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("100 kHz up (the DRM+ channel raster). Hold for a continuous change.");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%.3f MHz", a.freqMhz);
    }
    flowEnd();
}

void tab(App& a) {
    presets(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 8 * gUi;
    const float sw = std::min(std::max(250.f * gUi, W * 0.34f), W * 0.5f);
    nowPlaying(a, W - sw - gap, H);
    ImGui::SameLine(0, gap);
    signalPanel(a, sw, H);
}

// ---------------------------------------------------------------- the services list

void list(App& a) {
    const DrmTelemetry& t = a.rx.drm;
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see services"); ImGui::PopTextWrapPos(); } return; }
    if (t.services.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(t.mode >= 0 ? "reading the multiplex" : "searching for a DRM signal"); ImGui::PopTextWrapPos(); } return; }
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu service%s, click to play or to see the data", t.services.size(), t.services.size() == 1 ? "" : "s"); ImGui::PopTextWrapPos(); }
    const DrmService* cur = playing(t);
    for (const auto& s : t.services) {
        ImGui::PushID(s.shortId);
        const bool isCur = cur && cur->shortId == s.shortId;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetTextLineHeightWithSpacing();
        const bool shown = s.audio ? (isCur && S.dataSel < 0) : S.dataSel == s.shortId;
        if (ImGui::Selectable("##svc", shown, 0, ImVec2(0, lh * 2 + 2 * gUi))) { if (s.audio) { S.sel = s.shortId; S.dataSel = -1; } else S.dataSel = s.shortId; }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(p.x + 4 * gUi, p.y + 1), ImGui::GetColorU32(ImGuiCol_Text), serviceName(s).c_str());
        dl->AddText(ImVec2(p.x + 4 * gUi, p.y + lh + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), serviceFacts(s).c_str());
        if (!s.audio && ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s", s.appName.empty() ? "data service" : s.appName.c_str(), s.appMode.empty() ? "" : ": ", (s.appMode + (s.appDecoded ? "" : " (not decoded)")).c_str());
        if (s.audio && ImGui::IsItemHovered() && !s.codecText.empty()) ImGui::SetTooltip("%s", s.codecText.c_str());
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- the analysis row

void squarePlot(const char* title, const char* id, const std::vector<cf32>& pts, float side, double lim) {
    ImGui::BeginGroup();
    captionFit(side, "%s (%zu)", title, pts.size());   // never wider than the plot: the row keeps to the pane
    scatter(id, pts, ImVec2(side, side), lim, pal::accent(0.7f));
    ImGui::EndGroup();
}

void panels(App& a) {
    const DrmTelemetry& t = a.rx.drm;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(70.f, H - ImGui::GetTextLineHeightWithSpacing() - 8);
    const float side = std::min(plotH, std::max(80.f, W * 0.13f));
    const float colW = std::max(90.f, (W - 7 * gap - 3 * side) / 3.6f);
    const ImVec2 sz(colW, plotH);
    const std::vector<cf32> none;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    squarePlot("FAC cells", "##drm_fac", on ? t.facConst : none, side, 1.6);
    ImGui::SameLine(0, gap);
    squarePlot("SDC cells", "##drm_sdc", on ? t.sdcConst : none, side, 1.6);
    ImGui::SameLine(0, gap);
    squarePlot("MSC cells", "##drm_msc", on ? t.mscConst : none, side, t.mscQam == 64 ? 1.6 : 1.8);
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Channel response (kHz)");
    if (plt::BeginPlot("##drm_tf", ImVec2(colW, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, nullptr, 0, 0);
        const bool have = on && !t.chanDb.empty() && t.chanSpacingHz > 0;
        std::vector<float> x;
        if (have) {
            const int n = (int)t.chanDb.size();
            x.resize((size_t)n);
            for (int i = 0; i < n; i++) x[(size_t)i] = (float)((t.chanFirstCarrier + i * t.chanCarrierStep) * t.chanSpacingHz / 1000.0);
            float lo = 1e9f, hi = -1e9f;
            for (float v : t.chanDb) { lo = std::min(lo, v); hi = std::max(hi, v); }
            plt::SetupAxisLimits(plt::X1, x.front(), x.back(), plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, lo - 5, std::max(hi, lo + 10.f) + 5, plt::Cond_Always);
            plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
            plt::PlotLine("H", x.data(), t.chanDb.data(), n, sp);
        } else { plt::SetupAxisLimits(plt::X1, -5, 5, plt::Cond_Once); plt::SetupAxisLimits(plt::Y1, -20, 10, plt::Cond_Once); }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Impulse response (ms)");
    if (plt::BeginPlot("##drm_ir", ImVec2(colW, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, nullptr, 0, 0);
        if (on && !t.cirDb.empty() && t.cirStepMs > 0) {
            const int n = (int)t.cirDb.size();
            std::vector<float> x((size_t)n);
            for (int i = 0; i < n; i++) x[(size_t)i] = t.cirStartMs + i * t.cirStepMs;
            float mx = -200;
            for (float v : t.cirDb) mx = std::max(mx, v);
            plt::SetupAxisLimits(plt::X1, x.front(), x.back(), plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, mx - 50, mx + 5, plt::Cond_Always);
            plt::Spec sp; sp.LineColor = ImVec4(0.95f, 0.65f, 0.30f, 1); sp.LineWeight = 1.2f;
            plt::PlotLine("h", x.data(), t.cirDb.data(), n, sp);
        } else { plt::SetupAxisLimits(plt::X1, -1, 5, plt::Cond_Once); plt::SetupAxisLimits(plt::Y1, -50, 5, plt::Cond_Once); }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW * 0.8f, "SNR (dB)");
    historyPlot("##drm_snr", "dB", S.snr, ImVec2(colW * 0.8f, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW * 0.8f, "Audio frames ok (%%)");
    historyPlot("##drm_aud", "%", S.audio, ImVec2(colW * 0.8f, plotH));
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- status bar and summary

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const DrmTelemetry& t = a.rx.drm;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const double now = ImGui::GetTime();
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signal", !on ? 0 : t.mode >= 0 ? 1 : 0); flowNext(12 * gUi);
    lamp("Frame", !on ? 0 : t.state >= 1 ? 1 : t.mode >= 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("FAC", !on ? 0 : blockLamp(S.facOkAt, S.facBadAt, 3.0, now)); flowNext(12 * gUi);
    lamp("SDC", !on ? 0 : blockLamp(S.sdcOkAt, S.sdcBadAt, 6.0, now)); flowNext(12 * gUi);
    lamp("Audio", !on ? 0 : t.audioState == 2 ? blockLamp(S.audOkAt, S.audBadAt, 3.0, now) : t.audioState == 1 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[80];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    if (t.mode < 0) { ro("State", "searching"); snprintf(b, sizeof b, "%.0f dBFS", t.levelDbfs); ro("Level", b); return; }
    snprintf(b, sizeof b, "%s  %s", modeText(t.mode), t.occupancy >= 0 ? khz(t.bandwidthKhz).c_str() : "-"); ro("Mode", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
    snprintf(b, sizeof b, "%+.1f Hz", t.cfoHz); ro("CFO", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Audio ok/bad", b);
    if (t.audioRealSecs > 0) { snprintf(b, sizeof b, "%.1f %%", t.audioConcealedPct); ro("Concealed", b); }
    if (t.holding) ro("State", "fade, holding");
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const DrmTelemetry& t = a.rx.drm;
    l1 = "DRM";
    if (!live(a)) return;
    char b[160];
    if (t.mode < 0) { snprintf(b, sizeof b, "searching  %.0f dBFS", t.levelDbfs); l2 = b; return; }
    const DrmService* s = playing(t);
    snprintf(b, sizeof b, "%s  mode %s %s  SNR %.0f dB", s && !s->label.empty() ? s->label.c_str() : stateText(t.state), modeText(t.mode), t.occupancy >= 0 ? khz(t.bandwidthKhz).c_str() : "", t.snrDb);
    l2 = b;
}

// ---------------------------------------------------------------- the Receiver tab

// The xHE-AAC decoder (libfdk-aac, user-installed, never shipped: docs/CODECS.md) in plain words (#266, #247): loaded from where, or not
// found with the exact file to put where, or found but refused and why. "Look again" loads it without a restart.
static void xheDecoderState() {
    using dect2::drm::UsacLib;
    (void)UsacLib::shared();   // the first look (the decoder makes the same call)
    const UsacLib::Report r = UsacLib::lastReport();
    ImGui::PushTextWrapPos(0);
    if (r.loaded) {
        ImGui::TextColored(kGood, "xHE-AAC decoder: libfdk-aac loaded");
        ImGui::SameLine(); ImGui::TextDisabled("(%s)", r.path.c_str());
    } else {
#if defined(_WIN32)
        const char* file = "libfdk-aac-2.dll (64-bit)";
#elif defined(__APPLE__)
        const char* file = "libfdk-aac.2.dylib (or brew install fdk-aac)";
#else
        const char* file = "libfdk-aac.so.2 (or the fdk-aac package of your distribution)";
#endif
        ImGui::TextColored(kWarn, "xHE-AAC decoder: not installed. Most DRM stations (AAC) play without it; xHE-AAC ones (e.g. CNR-1) need it.");
        ImGui::TextDisabled("To add it: put %s in the codecs folder, then press Look again. OnAir never ships it (patent licence): you install it yourself.", file);
        ImGui::TextDisabled("codecs folder: %s", appcodec::codecsFolder().c_str());
        for (const std::string& f : r.failed) ImGui::TextColored(kBad, "found but not loaded: %s", f.c_str());
    }
    appcodec::openCodecFolderButtons();
    ImGui::SameLine();
    if (ImGui::Button("Look again##xhe")) UsacLib::rescan();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look for libfdk-aac in the codecs folders again (after copying it there), no restart needed");
    ImGui::PopTextWrapPos();
}

void receiver(App& a) {
    const DrmTelemetry& t = a.rx.drm;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    ImGui::BeginChild("##drm_rcv", ImVec2(0, 0));
    char b[200];
    auto kv = [&](const char* k, const std::string& v) { kvRow(a, k, v, 150); };
    auto kf = [&](const char* k, const char* fmt, auto... v) { snprintf(b, sizeof b, fmt, v...); kvRow(a, k, b, 150); };
    sectionHeader(Ic::Chip, "Receiver");
    kv("state", t.holding ? "holding through a fade (frequency, timing and clock frozen)" : stateText(t.state));
    kv("status", t.status.empty() ? "-" : t.status);
    kf("level", "%.1f dBFS in the %s channel", t.levelDbfs, t.mode == 4 ? "192 kHz" : "48 kHz");
    kf("quality", "%.0f %%", t.quality * 100.0);
    sectionHeader(Ic::Wave, "Signal");
    kv("robustness mode", t.mode >= 0 ? modeText(t.mode) : "-");
    kv("occupancy", t.occupancy >= 0 ? khz(t.bandwidthKhz) + "  (code " + std::to_string(t.occupancy) + ")" : "-");
    kv("interleaver", t.longInterleave < 0 ? "-" : t.longInterleave ? "long (2 s)" : "short (400 ms)");
    kf("MSC / SDC mapping", "%d-QAM / %d-QAM", t.mscQam, t.sdcQam);
    if (t.protRateB > 0) kf("protection", "A %d (%d%%)  B %d (%d%%)", t.protA, t.protRateA, t.protB, t.protRateB);
    else kf("protection", "level A %d, level B %d  (part A is %s)", t.protA, t.protB, t.streamLenA[0] > 0 ? "used" : "empty");
    snprintf(b, sizeof b, "%d:", t.numStreams);
    {
        std::string s = b;
        for (int i = 0; i < t.numStreams && i < 4; i++) { snprintf(b, sizeof b, " [%d] A %d B %d", i, t.streamLenA[i], t.streamLenB[i]); s += b; }
        kv("streams (bytes)", t.numStreams ? s + "  per logical frame" : "-");
    }
    kf("frame identity", "%d", t.identity);
    kv("reconfiguration", t.reconfiguration ? "announced" : "none");
    kv("time", timeText(t));
    sectionHeader(Ic::Gauge, "Measurements");
    kf("SNR", "%.1f dB (pilot MER)", t.snrDb);
    kf("carrier offset", "%+.2f Hz", t.cfoHz);
    kf("sample clock offset", "%+.1f ppm", t.sroPpm);
    kf("Doppler spread", "%.2f Hz", t.dopplerHz);
    kf("delay spread", "%.3f ms", t.delaySpreadMs);
    kf("strongest path at", "%+.3f ms", t.timingMs);
    kf("guard correlation", "%.2f", t.correlation);
    kf("fades held", "%llu  (re-synchronised at once %llu times)", (unsigned long long)t.fades, (unsigned long long)t.resyncs);
    sectionHeader(Ic::Layers, "Counters");
    kf("FAC blocks", "%llu ok, %llu bad", (unsigned long long)t.facOk, (unsigned long long)t.facBad);
    kf("SDC blocks", "%llu ok, %llu bad", (unsigned long long)t.sdcOk, (unsigned long long)t.sdcBad);
    kf("multiplex frames", "%llu ok, %llu bad", (unsigned long long)t.mscFramesOk, (unsigned long long)t.mscFramesBad);
    kf("audio frames", "%llu ok, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kf("text segments", "%llu ok, %llu bad", (unsigned long long)t.textSegmentsOk, (unsigned long long)t.textSegmentsBad);
    sectionHeader(Ic::Speaker, "Audio");
    kv("decoder", t.audioState == 2 ? "decoding" : t.audioState == 1 ? "frames received, no usable decoder" : "no audio stream");
    kv("detail", t.audioInfo.empty() ? "-" : t.audioInfo);
    kf("samples out", "%llu  (%.1f s at 48 kHz)", (unsigned long long)t.audioSamples, t.audioSamples / 48000.0);
    kf("concealed", "%.1f %% of the last 10 s;  %.1f s in all (%.1f s faded to silence), %llu times", t.audioConcealedPct, t.audioConcealedSecs + t.audioMutedSecs, t.audioMutedSecs,
       (unsigned long long)t.audioConcealEvents);
    kf("playout", "%.2f s queued, sound card %+.0f ppm from the radio, %d underruns", t.audioQueueSecs, t.audioDriftPpm, t.audioUnderruns);
    xheDecoderState();
    sectionHeader(Ic::Layers, "Services");
    if (t.services.empty()) ImGui::TextDisabled("none yet");
    for (const auto& s : t.services) {
        snprintf(b, sizeof b, "%d  %s", s.shortId + 1, serviceName(s).c_str());
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(b); ImGui::PopFont();
        ImGui::SameLine(0, 10 * gUi);
        // the facts wrap in a narrow pane (they ran 15 px past it at 150 %)
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%06X  %s%s%s", s.id & 0xFFFFFF, serviceFacts(s).c_str(), s.codecText.empty() ? "" : ", ", s.codecText.c_str());
        ImGui::PopTextWrapPos();
        if (s.conditionalAccess) { ImGui::SameLine(0, 6 * gUi); ImGui::TextColored(kWarn, "CA"); }
    }
    if (a.engine.drm().hasJournaline()) {
        static JlViewState jv;
        sectionHeader(Ic::Doc, "Journaline");
        journalineView("drmjl", a.engine.drm().journaline(), jv, -1);
    }
    sectionHeader(Ic::Warning, "Not supported");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("AAC with SBR or parametric stereo: only the AAC core plays (no SBR for 960-sample frames in the audio library), so the sound is band-limited and mono for PS services.");
    ImGui::TextDisabled("xHE-AAC: some streams need the optional libfdk-aac (install it yourself, OnAir loads it at run time and never ships it); the state above says whether it is used. See docs/CODECS.md, section AAC, xHE-AAC.");
    ImGui::TextDisabled("Data services: the MOT slide show, MOT broadcast website, EPG, TPEG and Journaline applications are decoded (the website as text, EPG and TPEG listed and counted only). Not decoded: stream mode data, single packet mode, compressed MOT directories, scrambled MOT bodies, other applications (named only), alternative frequency applications and hierarchical mappings. DRM+ with unequal error protection is untested.");
    ImGui::TextDisabled("DRM+ (mode E, 100 kHz) needs a radio rate of at least 100 ksps. Below 30 MHz a HackRF needs an upconverter or a long antenna.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the test signal

bool combo(const char* id, float w, const char* const* items, int n, int cur, int& out, const bool* disabled = nullptr) {
    bool ch = false;
    float tw = 0;   // never narrower than its longest choice (at 125 or 150 % the text grows faster than the box)
    for (int i = 0; i < n; i++) tw = std::max(tw, ImGui::CalcTextSize(items[i]).x);
    ImGui::SetNextItemWidth(std::max(w * gUi, tw + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x));
    if (ImGui::BeginCombo(id, items[std::max(0, std::min(n - 1, cur))])) {
        for (int i = 0; i < n; i++) {
            if (disabled && disabled[i]) ImGui::BeginDisabled();
            if (ImGui::Selectable(items[i], cur == i)) { out = i; ch = true; }
            if (disabled && disabled[i]) ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    return ch;
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* modes[] = {"mode A", "mode B", "mode C", "mode D", "mode E (DRM+)"};
    static const char* occ[] = {"usual (10 kHz)", "4.5 kHz", "5 kHz", "9 kHz", "10 kHz", "18 kHz", "20 kHz"};
    static const char* occE[] = {"100 kHz"};
    static const char* qam[] = {"64-QAM", "16-QAM"};
    static const char* qamE[] = {"16-QAM", "4-QAM"};
    static const char* prot[] = {"usual protection", "protection 0", "protection 1", "protection 2", "protection 3"};
    static const char* il[] = {"long interleaver", "short interleaver"};
    static const char* aud[] = {"melody", "1 kHz tone", "silence", "noise-like", "not AAC"};
    static const char* chn[] = {"no channel", "AWGN", "Rice, delay", "US consortium", "CCIR poor", "channel 5", "channel 6"};
    const int mode = sc.modeOpt[0] == 0 ? 1 : std::max(0, std::min(4, sc.modeOpt[0] - 1));
    const bool E = mode == 4;
    int v = 0;
    ImGui::TextDisabled("test signal");
    flowNext();
    if (combo("##dm", 78, modes, 5, mode, v)) { sc.modeOpt[0] = v + 1; if (v >= 2 && sc.modeOpt[1] != 0 && sc.modeOpt[1] != 4 && sc.modeOpt[1] != 6) sc.modeOpt[1] = 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Modes A to D: DRM30 (below 30 MHz). Mode E: DRM+ (VHF), a 100 kHz channel; the radio needs 100 ksps or more.");
    flowNext(6 * gUi);
    bool dis[7] = {};
    if (mode >= 2) for (int i : {1, 2, 3, 5}) dis[i] = true;   // modes C and D only have 10 and 20 kHz
    if (E) { int z = 0; combo("##doE", 110, occE, 1, 0, z); }   // mode E has one occupancy
    else if (combo("##do", 110, occ, 7, sc.modeOpt[1], v, dis)) { sc.modeOpt[1] = v; changed = true; }
    flowNext(6 * gUi);
    if (combo("##dq", 76, E ? qamE : qam, 2, sc.modeOpt[2], v)) { sc.modeOpt[2] = v; changed = true; }
    flowNext(6 * gUi);
    if (combo("##dp", 120, prot, 5, sc.modeOpt[3], v)) { sc.modeOpt[3] = v; changed = true; }
    flowNext(6 * gUi);
    if (combo("##di", 130, il, 2, sc.modeOpt[4], v)) { sc.modeOpt[4] = v; changed = true; }
    flowNext(6 * gUi);
    if (combo("##da", 100, aud, 5, sc.modeOpt[5], v)) { sc.modeOpt[5] = v; changed = true; }
    flowNext(6 * gUi);
    if (E) ImGui::BeginDisabled();
    if (combo("##dc", 110, chn, 7, sc.modeOpt[6], v)) { sc.modeOpt[6] = v; changed = true; }
    if (E) { ImGui::EndDisabled(); if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The channels of Annex B.1 are short wave channels (DRM30 only)"); }
    flowNext(8 * gUi);
    bool text = sc.modeOpt[7] == 0;
    if (ImGui::Checkbox("text message", &text)) { sc.modeOpt[7] = text ? 0 : 1; changed = true; }
}

// ---------------------------------------------------------------- the meter bank

void meters(const App& a, std::vector<ModeMeter>& out) {
    const DrmTelemetry& t = a.rx.drm;
    const uint64_t n = t.blocksOk + t.blocksBad;
    const double okPct = n ? 100.0 * (double)t.blocksOk / (double)n : 0;
    out.push_back({"SNR  dB", "%.1f", t.snrDb, 0, 40, t.state == 0 ? 0 : t.snrDb >= 15 ? 1 : t.snrDb >= 8 ? 2 : 3});
    out.push_back({"QUALITY  %", "%.0f", t.quality * 100.0, 0, 100, t.state == 0 ? 0 : t.quality > 0.9 ? 1 : t.quality > 0.5 ? 2 : 3});
    out.push_back({"AUDIO OK  %", "%.0f", okPct, 0, 100, n == 0 ? 0 : okPct > 95 ? 1 : okPct > 70 ? 2 : 3});
    out.push_back({"CONCEALED  %", "%.1f", t.audioConcealedPct, 0, 100, t.audioRealSecs <= 0 ? 0 : t.audioConcealedPct < 2 ? 1 : t.audioConcealedPct < 15 ? 2 : 3});
    out.push_back({"LEVEL  dBFS", "%.0f", t.levelDbfs, -100, 0, t.levelDbfs > -6 ? 3 : 0});
}

} // namespace

extern const ModeUi kDrmUi;
const ModeUi kDrmUi = {
    .sideTitle = "SERVICES",
    .tabName = "Radio",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .stream = false,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
