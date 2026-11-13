// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB: the Analyzer tab, a stream analyser panel: the contents of the ensemble as a tree (ensemble, services
// with their components, sub-channels, frequency information, links, transmitters), the details of the selected node, and the allocation of the
// 864 capacity units of the main service channel. Everything comes from the FIC (dab_fig.h): the receiver's ensemble tables plus the FIGs it
// keeps for the rest (programme types, languages, announcements, time, frequencies ...).
#include "app.h"
#include "dect2/dab/dab_fig.h"
#include "dect2/dab/dab_pad.h"
#include "dect2/dab/dab_announce.h"
#include "dect2/dsp/fftutil.h"
#include <cmath>
#include <ctime>

namespace {

struct View {
    DabEnsemble ens;
    DabFicAnalysis fic;
    DabFicInfo info;
    double stamp = -1e9;
    bool have = false;
    std::string sel = "ens";
    std::string clicked;      // the node clicked this frame
    int lastPlaying = -2;
};
View& view() { static View v; return v; }

constexpr int kCu = 864;

std::string protection(const DabSubchannel& s) {
    char b[48];
    if (s.eep) snprintf(b, sizeof b, "EEP %d-%c", s.level + 1, s.option ? 'B' : 'A');
    else snprintf(b, sizeof b, "UEP %d (index %d)", s.level + 1, s.uepIndex);
    return b;
}

std::string ascty(int t) {
    switch (t) {
        case 0: return "MPEG-1/2 Layer II (DAB)";
        case 1: return "MPEG background sound";
        case 2: return "multi-channel sound";
        case 63: return "HE-AAC (DAB+)";
        default: return "audio type " + std::to_string(t);
    }
}

std::string dscty(int t) {
    switch (t) {
        case 0: return "unspecified data"; case 1: return "TMC"; case 2: return "EWS"; case 3: return "ITTS"; case 4: return "paging";
        case 5: return "transparent data channel"; case 24: return "MPEG-2 transport stream (DMB)"; case 59: return "embedded IP packets";
        case 60: return "MOT"; default: return "data type " + std::to_string(t);
    }
}

std::string compKind(const DabComponent& c) {
    char b[96];
    if (c.tmid == 0) return ascty(c.ascty);
    if (c.tmid == 1) { snprintf(b, sizeof b, "stream data: %s", dscty(c.dscty).c_str()); return b; }
    return c.tmid == 3 ? "packet data" : "FIDC";
}

std::string freqText(const DabFicInfo::Freq& f) {
    std::string s;
    char b[48];
    for (double k : f.kHz) {
        if (f.rm == 8 || f.rm == 9) snprintf(b, sizeof b, "%.1f MHz", k / 1000.0);
        else if (f.rm == 0 || f.rm == 1) snprintf(b, sizeof b, "%.3f MHz", k / 1000.0);
        else snprintf(b, sizeof b, "%.0f kHz", k);
        s += (s.empty() ? "" : ", ") + std::string(b);
    }
    return s.empty() ? "-" : s;
}

const char* freqKind(int rm) {
    switch (rm) {
        case 0: case 1: return "DAB ensemble"; case 6: return "DRM"; case 8: return "FM with RDS"; case 9: return "FM";
        case 10: return "AM (MW 9 kHz, LW)"; case 12: return "AM (MW 5 kHz, SW)"; case 14: return "AMSS"; default: return "other";
    }
}

std::string svcName(const DabEnsemble& e, uint32_t sid) {
    char b[80];
    const auto it = e.services.find(sid);
    snprintf(b, sizeof b, sid > 0xFFFF ? "%s (%08X)" : "%s (%04X)", it != e.services.end() && !it->second.label.empty() ? it->second.label.c_str() : "?", sid);
    return b;
}

std::string subUsers(const DabEnsemble& e, int id) {
    std::string who;
    for (const auto& sv : e.services) for (const auto& c : sv.second.comps) if (c.subId == id) who += (who.empty() ? "" : ", ") + (sv.second.label.empty() ? std::string("?") : sv.second.label);
    return who;
}

// ---- tree
bool node(const char* label, const std::string& key, View& v, bool leaf, bool defOpen = false) {
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
    if (leaf) fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (defOpen) fl |= ImGuiTreeNodeFlags_DefaultOpen;
    if (v.sel == key) fl |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx(key.c_str(), fl, "%s", label);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { v.sel = key; v.clicked = key; }
    return open && !leaf;
}

void tree(View& v) {
    const DabEnsemble& e = v.ens;
    char b[160];
    snprintf(b, sizeof b, "Ensemble  %s  (%04X)", e.label.empty() ? "?" : e.label.c_str(), e.eid);
    if (node(b, "ens", v, false, true)) {
        snprintf(b, sizeof b, "Services (%zu)", e.services.size());
        if (node(b, "svcs", v, false, true)) {
            for (const auto& kv : e.services) {
                const DabService& sv = kv.second;
                const std::string k = "svc:" + std::to_string(sv.sid);
                const std::string t = svcName(e, sv.sid) + (sv.dmb() ? "  DMB" : sv.dabPlus() ? "  DAB+" : sv.audio() ? "  DAB" : "  data");
                if (node(t.c_str(), k, v, sv.comps.empty())) {
                    for (size_t i = 0; i < sv.comps.size(); i++) {
                        const DabComponent& c = sv.comps[i];
                        snprintf(b, sizeof b, "%s component, sub-channel %d: %s", c.primary ? "Primary" : "Secondary", c.subId, compKind(c).c_str());
                        node(b, k + ":" + std::to_string(i), v, true);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        snprintf(b, sizeof b, "Sub-channels (%zu)", e.subs.size());
        if (node(b, "subs", v, false)) {
            for (const auto& kv : e.subs) {
                const DabSubchannel& s = kv.second;
                snprintf(b, sizeof b, "Sub-channel %d   CU %d..%d   %s   %d kbit/s", s.id, s.start, s.start + s.size - 1, protection(s).c_str(), s.bitrate);
                node(b, "sub:" + std::to_string(s.id), v, true);
            }
            ImGui::TreePop();
        }
        if (!v.info.freqs.empty()) {
            snprintf(b, sizeof b, "Frequency information (%zu)", v.info.freqs.size());
            if (node(b, "freqs", v, false)) {
                for (size_t i = 0; i < v.info.freqs.size(); i++) {
                    const auto& f = v.info.freqs[i];
                    snprintf(b, sizeof b, "%s %04X: %s", freqKind(f.rm), f.id, freqText(f).c_str());
                    node(b, "freq:" + std::to_string(i), v, true);
                }
                ImGui::TreePop();
            }
        }
        if (!v.info.otherEnsembles.empty()) {
            snprintf(b, sizeof b, "Services in other ensembles (%zu)", v.info.otherEnsembles.size());
            node(b, "other", v, true);
        }
        if (!v.info.links.empty()) {
            snprintf(b, sizeof b, "Service links (%zu)", v.info.links.size());
            node(b, "links", v, true);
        }
        if (!v.info.transmitters.empty()) {
            snprintf(b, sizeof b, "Transmitter database (%zu)", v.info.transmitters.size());
            node(b, "tx", v, true);
        }
        snprintf(b, sizeof b, "FIC contents (%zu FIGs)", v.fic.figs.size());
        node(b, "fic", v, true);
        ImGui::TreePop();
    }
}

// ---- details
void kvBegin() {
    if (ImGui::BeginTable("kv", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
    }
}
void kv(App& a, const char* k, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void kv(App& a, const char* k, const char* fmt, ...) {
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
    ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string utcText(int64_t t) {
    if (!t) return "not received";
    const time_t tt = (time_t)t;
    struct tm m;
#ifdef _WIN32
    gmtime_s(&m, &tt);
#else
    gmtime_r(&tt, &m);
#endif
    char b[48];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S UTC", &m);
    return b;
}

std::string ltoText(int half) { char b[24]; snprintf(b, sizeof b, "UTC%c%d:%02d", half < 0 ? '-' : '+', std::abs(half) / 2, std::abs(half) % 2 * 30); return b; }

void announceText(App& a, unsigned flags) {
    std::string s;
    for (int i = 0; i < 16; i++) if (flags & (1u << i)) s += (s.empty() ? "" : ", ") + std::string(dabAnnouncementName(i));
    kv(a, "Announcements", "%s", s.empty() ? "none" : s.c_str());
}

void details(App& a, View& v) {
    const DabEnsemble& e = v.ens;
    const DabFicInfo& in = v.info;
    const std::string& s = v.sel;
    kvBegin();
    if (s == "ens" || s == "svcs" || s == "subs") {
        int used = 0, kbit = 0;
        for (const auto& kvp : e.subs) { used += kvp.second.size; kbit += kvp.second.bitrate; }
        kv(a, "Label", "%s", e.label.c_str());
        kv(a, "Ensemble id", "0x%04X", e.eid);
        if (in.country >= 0) kv(a, "Country id", "%d", in.country);
        if (in.hasEcc) kv(a, "Extended country code", "0x%02X", in.ecc);
        if (in.eref >= 0) kv(a, "Ensemble reference", "%d", in.eref);
        kv(a, "International table", "%d", in.intlTable);
        if (in.hasLto) kv(a, "Local time offset", "%s", ltoText(in.ltoHalfHours).c_str());
        kv(a, "Time", "%s", utcText(e.utc).c_str());
        if (in.cifHigh >= 0) kv(a, "CIF counter", "%d / %d", in.cifHigh, in.cifLow);
        kv(a, "Alarm flag", "%s", in.alarm ? "alarm" : "no");
        kv(a, "Services", "%zu", e.services.size());
        if (in.services >= 0) kv(a, "Services (FIG 0/7)", "%d", in.services);
        if (in.reconfCount >= 0) kv(a, "Reconfiguration count", "%d", in.reconfCount);
        kv(a, "Sub-channels", "%zu", e.subs.size());
        kv(a, "MSC capacity used", "%d of %d CU (%.0f %%)", used, kCu, 100.0 * used / kCu);
        kv(a, "Sub-channel bit rates", "%d kbit/s of %d kbit/s", kbit, kCu * 64 / 24);
        for (const DabAnnSwitch& sw : e.annActive) kv(a, "Announcement on", "cluster %d: %s, sub-channel %d", sw.cluster, [&] { std::string t; for (int i = 0; i < 16; i++) if (sw.asw & (1u << i)) t += (t.empty() ? "" : ", ") + std::string(dabAnnouncementTypeName(i)); return t; }().c_str(), sw.subId);
        kv(a, "FIC blocks", "%llu ok, %llu bad", (unsigned long long)v.fic.fibOk, (unsigned long long)v.fic.fibBad);
    } else if (s.rfind("svc:", 0) == 0) {
        const uint32_t sid = (uint32_t)std::strtoul(s.c_str() + 4, nullptr, 10);
        const auto it = e.services.find(sid);
        if (it != e.services.end()) {
            const DabService& sv = it->second;
            size_t colon = s.find(':', 4);
            const DabComponent* only = nullptr;
            if (colon != std::string::npos) { const size_t i = (size_t)std::atoi(s.c_str() + colon + 1); if (i < sv.comps.size()) only = &sv.comps[i]; }
            if (only) {
                kv(a, "Service", "%s", svcName(e, sid).c_str());
                kv(a, "Role", "%s", only->primary ? "primary component" : "secondary component");
                kv(a, "Transport", "%s", only->tmid == 0 ? "MSC stream audio" : only->tmid == 1 ? "MSC stream data" : only->tmid == 3 ? "MSC packet data" : "FIDC");
                kv(a, "Type", "%s", compKind(*only).c_str());
                kv(a, "Sub-channel", "%d", only->subId);
                const auto sub = e.subs.find(only->subId);
                if (sub != e.subs.end()) {
                    kv(a, "Start / size", "CU %d, %d CU", sub->second.start, sub->second.size);
                    kv(a, "Protection", "%s", protection(sub->second).c_str());
                    kv(a, "Bit rate", "%d kbit/s", sub->second.bitrate);
                }
            } else {
                kv(a, "Label", "%s", sv.label.c_str());
                if (dabLogoImage(a, e, sid, 48 * gUi)) ImGui::TextDisabled("station logo (SPI)");
                kv(a, "Service id", sid > 0xFFFF ? "0x%08X  (data service)" : "0x%04X  (programme service)", sid);
                kv(a, "Country / reference", "%u / %u", sid > 0xFFFF ? (sid >> 20) & 15 : (sid >> 12) & 15, sid > 0xFFFF ? sid & 0xFFFFF : sid & 0xFFF);
                if (sid > 0xFFFF) kv(a, "ECC", "0x%02X", sid >> 24); else if (in.hasEcc) kv(a, "ECC (ensemble)", "0x%02X", in.ecc);
                kv(a, "Format", "%s", sv.dmb() ? "DMB video" : sv.dabPlus() ? "DAB+ audio (HE-AAC)" : sv.audio() ? "DAB audio (MP2)" : "data");
                const auto pt = in.pty.find(sid);
                if (pt != in.pty.end()) kv(a, "Programme type", "%d  %s", pt->second, dabPtyName(in.intlTable, pt->second));
                const auto lg = in.language.find(sid);
                if (lg != in.language.end()) kv(a, "Language", "%d  %s", lg->second, dabLanguageName(lg->second));
                const auto an = in.announce.find(sid);
                if (an != in.announce.end()) announceText(a, an->second);
                const auto ua = in.userApps.find(sid);
                if (ua != in.userApps.end()) for (int t : ua->second) kv(a, "User application", "0x%03X  %s", t, dabUserAppName(t).c_str());
                const auto oe = in.otherEnsembles.find(sid);
                if (oe != in.otherEnsembles.end()) { std::string l; for (uint16_t x : oe->second) { char b[12]; snprintf(b, sizeof b, "%s%04X", l.empty() ? "" : ", ", x); l += b; } kv(a, "Also in ensembles", "%s", l.c_str()); }
                kv(a, "Components", "%zu", sv.comps.size());
                const DabAudioStats& as = a.rx.dab.audio;
                const DabComponent* pc = sv.playable();
                if (pc && as.sub == pc->subId && as.aac.valid) {
                    kv(a, "Audio profile", "%s", as.aac.profile.c_str());
                    kv(a, "AAC core", "%.0f kHz, %s", as.aac.coreRate / 1000.0, as.aac.coreChannels == 2 ? "stereo" : "mono");
                    kv(a, "SBR", "%s", as.aac.sbr ? "yes: output rate is twice the core rate" : "no");
                    kv(a, "Parametric stereo", "%s", as.aac.ps ? "yes: the mono core is made stereo" : "no");
                    kv(a, "Output", "%.0f kHz, %s", as.aac.outRate / 1000.0, as.aac.outChannels == 2 ? "stereo" : "mono");
                    kv(a, "MPEG Surround", "%s", as.aac.surround ? "configuration present" : "none");
                    kv(a, "Access units", "%d per superframe (120 ms)", as.aac.auPerSuperframe);
                    if (as.aac.sbr && as.sbrDecoded) kv(a, "Decoder", "%s: SBR%s applied", as.decoder.c_str(), as.psDecoded ? " and PS" : "");
                    else if (as.aac.sbr) kv(a, "Decoder", "%s, core only (%.0f kHz): FFmpeg cannot apply SBR%s to DAB+'s 960-sample frames; install faad2 (libfaad)", as.decoder.c_str(), as.aac.coreRate / 1000.0, as.aac.ps ? " or PS" : "");
                    else kv(a, "Decoder", "%s", as.decoder.c_str());
                }
                if (!sv.dls.empty()) kv(a, "Dynamic label", "%s", sv.dls.c_str());
                for (const DabDlpTag& t : sv.dlPlus) kv(a, (std::string("DL Plus ") + dabDlpTypeName(t.type)).c_str(), "%s", t.text.c_str());
            }
        }
    } else if (s.rfind("sub:", 0) == 0) {
        const int id = std::atoi(s.c_str() + 4);
        const auto it = e.subs.find(id);
        if (it != e.subs.end()) {
            const DabSubchannel& sc = it->second;
            kv(a, "Sub-channel id", "%d", sc.id);
            kv(a, "Start address", "CU %d", sc.start);
            kv(a, "Size", "%d CU (CU %d..%d)", sc.size, sc.start, sc.start + sc.size - 1);
            kv(a, "Protection", "%s%s", protection(sc).c_str(), sc.eep ? "  (equal error protection)" : "  (unequal error protection)");
            kv(a, "Bit rate", "%d kbit/s", sc.bitrate);
            const auto fe = in.subFec.find(id);
            if (fe != in.subFec.end()) kv(a, "Packet FEC", "%s", fe->second == 1 ? "applied (EN 300 401 5.3.5)" : fe->second == 0 ? "none" : "reserved");
            const auto lg = in.subLanguage.find(id);
            if (lg != in.subLanguage.end()) kv(a, "Language", "%d  %s", lg->second, dabLanguageName(lg->second));
            kv(a, "Carries", "%s", subUsers(e, id).c_str());
        }
    } else if (s.rfind("freq:", 0) == 0) {
        const size_t i = (size_t)std::atoi(s.c_str() + 5);
        if (i < in.freqs.size()) {
            const auto& f = in.freqs[i];
            kv(a, "Network", "%s", freqKind(f.rm));
            kv(a, f.rm == 0 || f.rm == 1 ? "Ensemble id" : "Identifier", "0x%04X", f.id);
            kv(a, "Frequencies", "%s", freqText(f).c_str());
            if (f.rm == 0 || f.rm == 1)
                for (double khz : f.kHz) {
                    const char* ch = dabChannelName(khz / 1000.0);
                    if (!ch) continue;
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TableNextColumn();
                    char l[48]; snprintf(l, sizeof l, "Tune to %s  %.3f MHz##%s", ch, khz / 1000.0, ch);
                    if (runningButton(a, l, true)) dabTuneMhz(a, khz / 1000.0);
                }
        }
    } else if (s == "freqs") {
        for (const auto& f : in.freqs) kv(a, freqKind(f.rm), "%04X  %s", f.id, freqText(f).c_str());
    } else if (s == "other") {
        for (const auto& kvp : in.otherEnsembles) { std::string l; for (uint16_t x : kvp.second) { char b[12]; snprintf(b, sizeof b, "%s%04X", l.empty() ? "" : ", ", x); l += b; } kv(a, svcName(e, kvp.first).c_str(), "%s", l.c_str()); }
    } else if (s == "links") {
        for (const auto& l : in.links) {
            std::string ids;
            for (uint32_t x : l.ids) { char b[16]; snprintf(b, sizeof b, "%s%X", ids.empty() ? "" : ", ", x); ids += b; }
            char k[40]; snprintf(k, sizeof k, "Link %d", l.lsn);
            kv(a, k, "%s, %s, %s: %s", l.active ? "active" : "inactive", l.hard ? "hard" : "soft", l.intl ? "international" : "national", ids.c_str());
        }
    } else if (s == "tx") {
        for (const auto& t : in.transmitters) { char k[24]; snprintf(k, sizeof k, "Main id %d", t.mainId); kv(a, k, "%.5f, %.5f", t.lat, t.lng); }
    } else if (s == "fic") {
        kv(a, "Frames", "%llu", (unsigned long long)v.fic.frames);
        kv(a, "Distinct FIGs", "%zu", v.fic.figs.size());
        kv(a, "FIC fill", "%.0f %%", v.fic.fibCount ? 100.0 * (double)v.fic.fillBytes / (30.0 * (double)v.fic.fibCount) : 0.0);
        for (const auto& st : v.fic.stats) { char k[16]; snprintf(k, sizeof k, "FIG %d/%d", st.type, st.ext); kv(a, k, "%llu  %s", (unsigned long long)st.count, dabFigName(st.type, st.ext)); }
    }
    ImGui::EndTable();
}

// ---- audio spectrum of the service that plays
void audioSpectrum(App& a, float h) {
    static dect2::Fft fft(2048);
    static float smooth[1024], peak[1024];
    static bool init = false;
    if (!init) { for (int i = 0; i < 1024; i++) smooth[i] = peak[i] = -120.f; init = true; }
    constexpr int N = 2048;
    std::vector<float> st;
    const int got = a.engine.dabAudio().recent(st, N);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(80.f, ImGui::GetContentRegionAvail().x - 2 * gUi);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##aspec", ImVec2(w, h));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(16, 18, 20, 255));
    const float db0 = 0.f, db1 = -100.f, lo = 30.f, hi = 22000.f;
    auto yOf = [&](float db) { return p.y + h * (db0 - std::max(db1, std::min(db0, db))) / (db0 - db1); };
    for (int d = -20; d > -100; d -= 20) {
        dl->AddLine(ImVec2(p.x, yOf((float)d)), ImVec2(p.x + w, yOf((float)d)), IM_COL32(60, 62, 66, 255));
        char t[12]; snprintf(t, sizeof t, "%d", d);
        dl->AddText(ImVec2(p.x + 2, yOf((float)d) - ImGui::GetFontSize()), IM_COL32(120, 120, 120, 255), t);
    }
    for (float f : {100.f, 1000.f, 10000.f}) {
        const float x = p.x + w * std::log(f / lo) / std::log(hi / lo);
        dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), IM_COL32(60, 62, 66, 255));
        char t[12]; snprintf(t, sizeof t, f >= 1000 ? "%.0fk" : "%.0f", f >= 1000 ? f / 1000 : f);
        dl->AddText(ImVec2(x + 2, p.y + h - ImGui::GetFontSize() - 1), IM_COL32(120, 120, 120, 255), t);
    }
    if (got < N) {
        for (int i = 0; i < 1024; i++) { smooth[i] = peak[i] = -120.f; }
        dl->AddText(ImVec2(p.x + 8, p.y + h * 0.5f - ImGui::GetFontSize() * 0.5f), IM_COL32(150, 150, 150, 255), "no audio: click a service to listen to it");
        return;
    }
    std::vector<dect2::cf32> x((size_t)N);
    float pkL = 0, pkR = 0;
    for (int i = 0; i < N; i++) {
        const float wn = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * (float)i / (float)N);
        x[(size_t)i] = dect2::cf32(0.5f * (st[(size_t)i * 2] + st[(size_t)i * 2 + 1]) * wn, 0.f);
        pkL = std::max(pkL, std::fabs(st[(size_t)i * 2])); pkR = std::max(pkR, std::fabs(st[(size_t)i * 2 + 1]));
    }
    fft.forward(x.data());
    for (int k = 0; k < 1024; k++) {
        const float m = std::abs(x[(size_t)k]) / ((float)N / 4.f);   // a full scale sine reads 0 dB
        const float db = 20.f * std::log10(std::max(m, 1e-7f));
        smooth[k] = db > smooth[k] ? db : smooth[k] - 2.5f;
        peak[k] = db > peak[k] ? db : peak[k] - 0.4f;
    }
    const int cols = (int)w;
    ImVec2 prev(0, 0), prevP(0, 0);
    const ImU32 fill = ImGui::GetColorU32(pal::accent(0.35f)), line = ImGui::GetColorU32(pal::accent());
    for (int c = 0; c < cols; c++) {
        const float f0 = lo * std::pow(hi / lo, (float)c / cols), f1 = lo * std::pow(hi / lo, (float)(c + 1) / cols);
        const int k0 = std::max(1, (int)(f0 / 24000.f * 1024)), k1 = std::min(1023, std::max(k0, (int)(f1 / 24000.f * 1024)));
        float s = -120.f, pk = -120.f;
        for (int k = k0; k <= k1; k++) { s = std::max(s, smooth[k]); pk = std::max(pk, peak[k]); }
        const ImVec2 cur(p.x + (float)c, yOf(s)), curP(p.x + (float)c, yOf(pk));
        dl->AddRectFilled(ImVec2(cur.x, cur.y), ImVec2(cur.x + 1, p.y + h), fill);
        if (c) { dl->AddLine(prev, cur, line, 1.2f); dl->AddLine(prevP, curP, IM_COL32(230, 200, 90, 160), 1.f); }
        prev = cur; prevP = curP;
    }
    char t[64];
    snprintf(t, sizeof t, "L %.1f  R %.1f dBFS", 20 * std::log10(std::max(pkL, 1e-6f)), 20 * std::log10(std::max(pkR, 1e-6f)));
    dl->AddText(ImVec2(p.x + w - ImGui::CalcTextSize(t).x - 6, p.y + 3), IM_COL32(190, 190, 190, 255), t);
    if (ImGui::IsItemHovered()) {
        const float fr = lo * std::pow(hi / lo, (ImGui::GetMousePos().x - p.x) / w);
        ImGui::SetTooltip("%.0f Hz", fr);
    }
}

void measures(App& a, View& v) {
    const DabAudioStats& au = a.rx.dab.audio;
    ImGui::Spacing();
    std::string who = "no service playing";
    if (au.sub >= 0) {
        who = "sub-channel " + std::to_string(au.sub);
        for (const auto& kvp : v.ens.services) { const DabComponent* c = kvp.second.playable(); if (c && c->subId == au.sub) { who = kvp.second.label; break; } }
        char b[80]; snprintf(b, sizeof b, "   %s  %d kbit/s  %s", au.codec.c_str(), au.bitrate, au.decoding ? "" : "(waiting for audio)");
        who += b;
    }
    sectionHeader(Ic::Speaker, ("Audio spectrum: " + who).c_str());
    audioSpectrum(a, 100 * gUi);
    ImGui::Spacing();
    sectionHeader(Ic::Camera, "Latest picture");
    dabSlidePanel(a, v.ens, std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - 6 * gUi));
}

// ---- MSC allocation
ImU32 subColour(int id) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(std::fmod((float)id * 0.137f + 0.05f, 1.f), 0.55f, 0.85f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, 1));
}

void allocation(App& a, View& v) {
    const DabEnsemble& e = v.ens;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(100.f, ImGui::GetContentRegionAvail().x - 4 * gUi), h = 34 * gUi;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(34, 36, 40, 255));
    ImGui::InvisibleButton("##alloc", ImVec2(w, h + 14 * gUi));
    const ImVec2 mp = ImGui::GetMousePos();
    int hover = -1, used = 0, overlap = 0;
    std::vector<int> owner(kCu, -1);
    for (const auto& kvp : e.subs) {
        const DabSubchannel& s = kvp.second;
        if (s.size <= 0 || s.start < 0 || s.start + s.size > kCu) continue;
        for (int c = s.start; c < s.start + s.size; c++) { if (owner[(size_t)c] >= 0) overlap++; else used++; owner[(size_t)c] = s.id; }
        const float x0 = p.x + w * (float)s.start / kCu, x1 = p.x + w * (float)(s.start + s.size) / kCu;
        const bool sel = v.sel == "sub:" + std::to_string(s.id);
        dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1 - 1, p.y + h), subColour(s.id));
        if (sel) dl->AddRect(ImVec2(x0, p.y), ImVec2(x1 - 1, p.y + h), IM_COL32(255, 255, 255, 255), 0, 0, 2.f);
        char t[16]; snprintf(t, sizeof t, "%d", s.id);
        if (x1 - x0 > ImGui::CalcTextSize(t).x + 4) dl->AddText(ImVec2(x0 + 3, p.y + h * 0.5f - ImGui::GetFontSize() * 0.5f), IM_COL32(20, 20, 20, 255), t);
        if (ImGui::IsItemHovered() && mp.x >= x0 && mp.x < x1 && mp.y >= p.y && mp.y <= p.y + h) hover = s.id;
    }
    for (int i = 0; i <= 8; i++) {   // scale: every 108 CU
        const float x = p.x + w * (float)i / 8;
        dl->AddLine(ImVec2(x, p.y + h), ImVec2(x, p.y + h + 4 * gUi), IM_COL32(150, 150, 150, 255));
        char t[16]; snprintf(t, sizeof t, "%d", i * kCu / 8);
        dl->AddText(ImVec2(std::min(x, p.x + w - 24 * gUi), p.y + h + 3 * gUi), IM_COL32(150, 150, 150, 255), t);
    }
    if (hover >= 0) {
        if (ImGui::IsItemClicked()) v.sel = "sub:" + std::to_string(hover);
        const DabSubchannel& s = e.subs.at(hover);
        ImGui::SetTooltip("Sub-channel %d\nCU %d..%d (%d CU)\n%s, %d kbit/s\n%s", s.id, s.start, s.start + s.size - 1, s.size, protection(s).c_str(), s.bitrate, subUsers(e, s.id).c_str());
    }
    ImGui::TextDisabled("MSC: %d of %d CU in use, %d free%s", used, kCu, kCu - used, overlap ? "" : "");
    if (overlap) { ImGui::SameLine(); ImGui::TextColored(pal::badRed(), "   %d CU claimed by two sub-channels", overlap); }
    (void)a;
}

} // namespace

void dabAnalyzerTab(App& a) {
    View& v = view();
    const double now = ImGui::GetTime();
    if (!v.have || now - v.stamp > 0.5) {
        v.ens = a.engine.dabEnsemble();
        v.fic = a.engine.dabFic();
        v.info = dabFicInfo(v.fic);
        v.stamp = now;
        v.have = true;
    }
    if (!v.ens.valid && v.ens.services.empty()) { ImGui::TextDisabled(a.rx.dab.state == 2 ? "waiting for the ensemble" : "no signal: nothing to analyse yet"); return; }
    // a click on a service, a component or a sub-channel plays it; a service picked in another tab shows here
    if (!v.clicked.empty()) {
        int sub = -1;
        const std::string& k = v.clicked;
        if (k.rfind("sub:", 0) == 0) sub = std::atoi(k.c_str() + 4);
        else if (k.rfind("svc:", 0) == 0) {
            const auto it = v.ens.services.find((uint32_t)std::strtoul(k.c_str() + 4, nullptr, 10));
            if (it != v.ens.services.end()) {
                const size_t colon = k.find(':', 4);
                const size_t i = colon == std::string::npos ? it->second.comps.size() : (size_t)std::atoi(k.c_str() + colon + 1);
                if (i < it->second.comps.size()) sub = it->second.comps[i].subId;
                else if (const DabComponent* c = it->second.playable()) sub = c->subId;
            }
        }
        if (sub >= 0) { dabSelectStation(a, sub, true); v.lastPlaying = sub; }
        v.clicked.clear();
    } else if (a.engine.dabSelected() != v.lastPlaying) {
        v.lastPlaying = a.engine.dabSelected();
        for (const auto& kvp : v.ens.services) { const DabComponent* c = kvp.second.playable(); if (c && c->subId == v.lastPlaying) { v.sel = "svc:" + std::to_string(kvp.first); break; } }
    }
    const float bottom = 86 * gUi;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool narrow = avail.x < 620 * gUi;
    const float leftW = narrow ? avail.x : std::max(260.f * gUi, avail.x * 0.46f);
    const float hh = std::max(120.f * gUi, avail.y - bottom - (narrow ? 0 : 0));
    const float treeH = narrow ? hh * 0.5f : hh;
    if (ImGui::BeginChild("##dtree", ImVec2(leftW, treeH), ImGuiChildFlags_Borders)) tree(v);
    ImGui::EndChild();
    if (!narrow) ImGui::SameLine();
    if (ImGui::BeginChild("##ddet", ImVec2(0, narrow ? hh - treeH : hh), ImGuiChildFlags_Borders)) { details(a, v); measures(a, v); }
    ImGui::EndChild();
    sectionHeader(Ic::Layers, "Main service channel allocation");
    allocation(a, v);
}
