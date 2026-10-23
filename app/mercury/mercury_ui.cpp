// Mercury screens (monitor only): the band presets, Listen and the known callsigns, then five views: the frames heard (UTC, mode, frame,
// session, from, to, SNR, FEC, details), the sessions (both callsigns, the state, the seed, each direction's rung, polls and data with
// its text and a Save button), the stations, the broadcast messages and files (a text file shown, a PNG picture shown, any file saved
// on the user's click only, nothing opened or run), and which modes are searched. Encrypted sessions are listed, never decoded.
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "dect2/common/png.h"
#include "dect2/mercury/mercury_rx.h"
#include "dect2/mercury/mercury_tel.h"
#include "dect2/mercury/mercury_phy.h"
#include "dect2/mercury/mercury_gen.h"
#include "dect2/mercury/mercury_mfsk.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct State {
    bool wasRunning = false, loaded = false;
    bool listen = false; int pushedListen = -1; float pushedVol = -1; bool pushedMute = false;   // "Listen": off until switched on
    int view = 0;                        // 0 frames, 1 sessions, 2 stations, 3 broadcast
    int demod = dect2::HfDemodAuto;
    char calls[512] = {};                // known callsigns, comma separated
    std::string pushedCalls = "\x01";
    uint32_t mask = 0xFFFFFFFFu, pushedMask = 0;
    int selSession = -1, selFile = -1;
    std::string msg;
    // the picture of the selected file
    int texFile = -1;
    gfx::Image* tex = nullptr;
    int tw = 0, th = 0;
};
State S;

bool live(const App& a) { return a.rx.standard == 231; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.listen = plat::prefs().getB("mercuryListen", false);
    S.demod = hfDemodLoad("mercuryDemod");
    const std::string c = plat::prefs().getS("mercuryCalls", "");
    snprintf(S.calls, sizeof S.calls, "%s", c.c_str());
    S.mask = (uint32_t)plat::prefs().getI("mercuryModes", 0xFFF);
    if (!S.mask) S.mask = 0xFFF;
}

const ImVec4 kCallCol(1.0f, 0.82f, 0.36f, 1);

void utcText(double t, char* out, size_t n) {
    const time_t tt = (time_t)std::floor(t);
    struct tm g {};
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    strftime(out, n, "%H:%M:%S", &g);
}

ImVec4 kindColour(const dect2::MercuryFrameLog& f) {
    if (!f.fecOk) return ImVec4(0.95f, 0.55f, 0.45f, 1);
    if (f.encrypted) return ImVec4(0.75f, 0.60f, 0.95f, 1);
    if (f.kind == "DATA") return ImVec4(0.55f, 0.85f, 0.95f, 1);
    if (f.kind == "POLL" || f.kind == "REQ" || f.kind == "STATUS" || f.kind == "CONTROL") return ImVec4(0.40f, 0.95f, 0.55f, 1);
    if (f.kind == "CALL" || f.kind == "ACCEPT" || f.kind == "CQ" || f.kind == "DISCONNECT") return kCallCol;
    if (f.kind == "MESSAGE" || f.kind == "FILE") return ImVec4(0.95f, 0.80f, 0.55f, 1);
    return ImVec4(0.80f, 0.84f, 0.88f, 1);
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    std::ofstream o(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!o) return false;
    o.write((const char*)d.data(), (std::streamsize)d.size());
    return (bool)o;
}

// "Save": the user picks the place; the name is offered without any path the sender put in it
void saveAs(const std::string& name, const std::vector<uint8_t>& d) {
    std::string n = name.empty() ? std::string("mercury.bin") : name;
    for (char& c : n) if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 0x20) c = '_';
    const std::string path = saveFileDialog(n.c_str());
    if (path.empty()) return;
    S.msg = writeFile(path, d) ? std::string(tr("saved")) + " " + path : std::string(tr("could not write")) + " " + path;
}

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Band"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Mercury has no calling frequency: every network and station chooses its own dial frequency (upper sideband). These buttons only jump to the digital part of each band."));
    flowNext(6 * gUi);
    int n = 0;
    const dect2::MercuryPreset* p = dect2::mercuryPresets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##mercp%d", p[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz", p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    listenControls(a, S.listen, "mercuryListen", "mercury", tr("Play the sideband (dial + 200 Hz to 3.8 kHz) on the sound card; decoding is the same either way"));
    flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##mercury", tr("Clear"));
    if (ImGui::Button(cl)) { a.engine.mercury().clearResults(); S.selSession = S.selFile = -1; }
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Known callsigns"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A CALL names the called station only by a CRC of its callsign. Callsigns listed here (comma separated), and every callsign heard, are matched against it; with both callsigns the session's data can be checked and shown."));
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(180 * gUi);
    if (ImGui::InputText("##mercalls", S.calls, sizeof S.calls)) plat::prefs().setS("mercuryCalls", S.calls);
    flowEnd();
    if (!S.msg.empty()) { ImGui::TextDisabled("%s", S.msg.c_str()); }
}

std::string sessionName(const dect2::MercuryTelemetry& t, int key) {
    for (const auto& s : t.sessions) {
        if (s.key != key) continue;
        char b[64];
        if (s.id >= 0) snprintf(b, sizeof b, "%02X", s.id); else snprintf(b, sizeof b, "%04X", s.seed);
        return b;
    }
    return "-";
}

void framesView(App& a) {
    const auto& t = a.rx.mercury;
    if (!ImGui::BeginTable("##merf", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Mode")); ImGui::TableSetupColumn(tr("Frame")); ImGui::TableSetupColumn(tr("Session"));
    ImGui::TableSetupColumn(tr("From")); ImGui::TableSetupColumn(tr("To")); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn("FEC");
    ImGui::TableSetupColumn(tr("Detail"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[48];
    for (const auto& f : t.frames) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); utcText((double)f.utc, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(f.modeName.c_str());
        ImGui::TableNextColumn(); ImGui::TextColored(kindColour(f), "%s", f.kind.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%.2f s on the air, offset %+.1f Hz, %d LDPC iterations, %d channel bit errors corrected%s%s", f.hex.empty() ? "" : f.hex.c_str(),
                                                      f.airSec, f.cfoHz, f.ldpcIters, f.rawErrors, f.seeded ? ", seeded CRC" : "", f.encrypted ? ", encrypted" : "");
        ImGui::TableNextColumn();
        if (f.sessionKey >= 0) ImGui::Text("%s", sessionName(t, f.sessionKey).c_str());
        else if (f.sessionId >= 0) ImGui::Text("%d", f.sessionId); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (f.from.empty()) ImGui::TextDisabled("-"); else ImGui::TextColored(kCallCol, "%s", f.from.c_str());
        ImGui::TableNextColumn(); if (f.to.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(f.to.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", f.snrDb);
        ImGui::TableNextColumn();
        if (!f.fecOk) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1), "%s", tr("failed"));
        else ImGui::TextDisabled("OK %d", f.ldpcIters);
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(f.detail.c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (t.frames.empty()) ImGui::TextDisabled("%s", tr("no frames yet"));
}

void direction(App& a, const dect2::MercurySession& s, int d) {
    const dect2::MercuryDirection& x = s.d[d];
    const std::string& from = d == 0 ? s.caller : s.callee;
    const std::string& to = d == 0 ? s.callee : s.caller;
    ImGui::TextColored(kCallCol, "%s", from.c_str()); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("->"); ImGui::SameLine(0, 4 * gUi); ImGui::TextUnformatted(to.c_str());
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("%llu %s, %llu %s, %llu %s, %llu %s", (unsigned long long)x.dataFrames, tr("data frames"), (unsigned long long)x.blocksOk, tr("blocks"),
                        (unsigned long long)x.blocksFailed, tr("failed"), (unsigned long long)x.bytes, tr("bytes"));
    char b[160];
    std::string info;
    if (x.lastRung >= 0) { snprintf(b, sizeof b, "%s %s", tr("last data on"), dect2::mercury::rungName(x.lastRung)); info += b; }
    if (x.askedRung >= 0) { snprintf(b, sizeof b, "%s%s %d x %s", info.empty() ? "" : ", ", tr("asked for"), x.askedFrames, dect2::mercury::rungName(x.askedRung)); info += b; }
    if (x.reportedSnrDb > -999) { snprintf(b, sizeof b, "%s%s %+.0f dB", info.empty() ? "" : ", ", tr("heard by the peer at"), x.reportedSnrDb); info += b; }
    if (x.cfoKnown) { snprintf(b, sizeof b, "%s%s %+.1f Hz", info.empty() ? "" : ", ", tr("offset"), x.cfoHz); info += b; }
    if (!info.empty()) ImGui::TextDisabled("%s", info.c_str());
    if (s.encrypted) { ImGui::TextColored(ImVec4(0.75f, 0.60f, 0.95f, 1), "%s", tr("encrypted: the payload is not decoded")); return; }
    char id[48]; snprintf(id, sizeof id, "##mers%d_%d", s.key, d);
    if (ImGui::BeginChild(id, ImVec2(0, std::max(60.f * gUi, ImGui::GetContentRegionAvail().y * 0.45f)), ImGuiChildFlags_Borders)) {
        ImGui::PushFont(a.mono, 0);
        if (x.text.empty()) ImGui::TextDisabled("%s", tr("no data yet"));
        else { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(x.text.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::PopFont();
    }
    ImGui::EndChild();
    if (x.bytes) {
        char sb[48]; snprintf(sb, sizeof sb, "%s##mersv%d_%d", tr("Save"), s.key, d);
        if (ImGui::Button(sb)) {
            std::vector<uint8_t> data;
            if (a.engine.mercury().sessionBytes(s.key, d, data)) saveAs(from + "_to_" + to + ".bin", data);
        }
    }
}

void sessionsView(App& a) {
    const auto& t = a.rx.mercury;
    const float listH = std::min(ImGui::GetContentRegionAvail().y * 0.35f, 160.f * gUi);
    if (ImGui::BeginTable("##mers", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Session")); ImGui::TableSetupColumn(tr("Caller")); ImGui::TableSetupColumn(tr("Called")); ImGui::TableSetupColumn(tr("State"));
        ImGui::TableSetupColumn(tr("Start")); ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Seed"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::PushFont(a.mono, 0);
        char b[48];
        for (const auto& s : t.sessions) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char sel[48]; snprintf(sel, sizeof sel, "%s##mersel%d", sessionName(t, s.key).c_str(), s.key);
            if (ImGui::Selectable(sel, S.selSession == s.key, ImGuiSelectableFlags_SpanAllColumns)) S.selSession = s.key;
            ImGui::TableNextColumn(); ImGui::TextColored(kCallCol, "%s", s.caller.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.callee.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.encrypted ? (s.state + ", " + tr("encrypted")).c_str() : s.state.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", s.startLevel >= 0 ? dect2::mercury::rungName(s.ctlDeaf ? 0 : s.startLevel) : "-");
            ImGui::TableNextColumn(); utcText((double)s.lastUtc, b, sizeof b); ImGui::TextDisabled("%s", b);
            ImGui::TableNextColumn();
            if (s.seedKnown) ImGui::TextDisabled("%04X %s", s.seed, s.seedFromFrames ? tr("(from the frames)") : tr("(from the connect)"));
            else ImGui::TextDisabled("-");
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    if (t.sessions.empty()) { ImGui::TextDisabled("%s", tr("no sessions yet")); return; }
    const dect2::MercurySession* s = nullptr;
    for (const auto& x : t.sessions) if (x.key == S.selSession) s = &x;
    if (!s) s = &t.sessions.front();
    ImGui::Spacing();
    char b[200];
    snprintf(b, sizeof b, "%s %s, %d Hz, %llu %s", tr("bandwidth"), s->bwHz ? "" : "?", s->bwHz, (unsigned long long)s->frames, tr("frames"));
    ImGui::TextDisabled("%s", b);
    direction(a, *s, 0);
    direction(a, *s, 1);
}

void stationsView(App& a) {
    const auto& st = a.rx.mercury.stations;
    if (!ImGui::BeginTable("##merst", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Count"));
    ImGui::TableSetupColumn(tr("Called")); ImGui::TableSetupColumn(tr("Last"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[32];
    for (const auto& s : st) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(kCallCol, "%s", s.call.c_str());
        ImGui::TableNextColumn(); utcText((double)s.lastSeen, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s.count);
        ImGui::TableNextColumn(); if (s.heardTo.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(s.heardTo.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", s.last.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (st.empty()) ImGui::TextDisabled("%s", tr("no stations yet"));
}

// a PNG file is shown with OnAir's own decoder, from memory; other pictures are not decoded (they can be saved)
void picture(App& a, const dect2::MercuryFile& f) {
    if (S.texFile != f.id) {
        delete S.tex; S.tex = nullptr; S.tw = S.th = 0; S.texFile = f.id;
        std::vector<uint8_t> d;
        std::string name;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (gGfx && a.engine.mercury().fileBytes(f.id, d, name) && d.size() > 8 && d[0] == 0x89 && d[1] == 'P' &&
            dect2::decodePng(d.data(), d.size(), w, h, rgba) && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            std::vector<uint32_t> px((size_t)w * h);
            std::memcpy(px.data(), rgba.data(), px.size() * 4);
            S.tex = gGfx->createImage(w, h, 0xFF000000u);
            if (S.tex) S.tex->update(0, 0, w, h, px.data());
            S.tw = w; S.th = h;
        }
    }
    if (!S.tex) { ImGui::TextDisabled("%s", tr("picture: save it to view it (only PNG is shown here)")); return; }
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float sc = std::min({1.f, av.x / (float)S.tw, std::max(40.f, av.y - 30 * gUi) / (float)S.th});
    ImGui::Image(S.tex->texture(), ImVec2(S.tw * sc, S.th * sc));
}

void broadcastView(App& a) {
    const auto& t = a.rx.mercury;
    ImGui::TextDisabled("%s", tr("Messages"));
    if (ImGui::BeginChild("##mermsg", ImVec2(0, std::min(120.f * gUi, ImGui::GetContentRegionAvail().y * 0.3f)), ImGuiChildFlags_Borders)) {
        ImGui::PushFont(a.mono, 0);
        if (t.messages.empty()) ImGui::TextDisabled("%s", tr("no messages yet"));
        else { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.messages.c_str()); ImGui::PopTextWrapPos(); }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("%s", tr("Files (RaptorQ carousel)"));
    if (ImGui::BeginTable("##merfiles", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::min(120.f * gUi, ImGui::GetContentRegionAvail().y * 0.35f)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("File")); ImGui::TableSetupColumn(tr("Bytes")); ImGui::TableSetupColumn(tr("Received")); ImGui::TableSetupColumn(tr("Mode"));
        ImGui::TableSetupColumn(tr("Kind"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& f : t.files) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char sel[300]; snprintf(sel, sizeof sel, "%s##merfile%d", f.done ? f.name.c_str() : tr("(receiving)"), f.id);
            if (ImGui::Selectable(sel, S.selFile == f.id, ImGuiSelectableFlags_SpanAllColumns)) S.selFile = f.id;
            ImGui::TableNextColumn(); ImGui::Text("%u", f.size);
            ImGui::TableNextColumn();
            if (f.done) ImGui::TextDisabled("%s", tr("complete"));
            else ImGui::ProgressBar(f.needed ? std::min(1.f, (float)f.symbols / (float)f.needed) : 0.f, ImVec2(90 * gUi, 0));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", f.mode.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", f.kind.c_str());
        }
        ImGui::EndTable();
    }
    const dect2::MercuryFile* f = nullptr;
    for (const auto& x : t.files) if (x.id == S.selFile) f = &x;
    if (!f) { if (t.files.empty()) ImGui::TextDisabled("%s", tr("no files yet")); return; }
    if (!f->done) { ImGui::TextDisabled("%llu / %llu %s", (unsigned long long)f->symbols, (unsigned long long)f->needed, tr("symbols")); return; }
    char sb[48]; snprintf(sb, sizeof sb, "%s##merfsave", tr("Save"));
    if (ImGui::Button(sb)) {
        std::vector<uint8_t> d;
        std::string name;
        if (a.engine.mercury().fileBytes(f->id, d, name)) saveAs(name, d);
    }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%s", tr("received files are shown, never opened or run; Save writes one where you choose"));
    if (f->kind == "text") {
        if (ImGui::BeginChild("##merftext", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(f->preview.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
        }
        ImGui::EndChild();
    } else if (f->kind == "image") picture(a, *f);
}

void modesView(App&) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The OFDM data modes searched for (all at once). A mode switched off is not searched; fewer modes use less of the processor."));
    ImGui::PopTextWrapPos();
    bool ch = false;
    for (int m = 0; m < dect2::mercury::kNumModes; m++) {
        bool on = (S.mask >> m) & 1;
        char b[96]; snprintf(b, sizeof b, "%-8s##mermode%d", dect2::mercury::modeName(m), m);
        if (ImGui::Checkbox(b, &on)) { S.mask = on ? (S.mask | (1u << m)) : (S.mask & ~(1u << m)); ch = true; }
        ImGui::SameLine(110 * gUi);
        if (m < dect2::mercury::kNumOfdmModes) {
            const auto& md = dect2::mercury::ofdmMode(m);
            const auto& g = dect2::mercury::geometry(m);
            ImGui::TextDisabled("%4.0f Hz, %4d %s, %.1f s, %s", g.bandwidthHz, g.payloadBytes, tr("bytes"), g.burstSeconds, md.use);
        } else {
            const auto& g = dect2::mercury::mfskGeometry(m);
            ImGui::TextDisabled("%4.0f Hz, %4d %s, %.1f s, %s", 32 * g.spacingHz, 98, tr("bytes"), g.burstSamples / 8000.0,
                                m == dect2::mercury::kMfsk ? tr("ARQ rung 0 (the floor): CALLs, data and control below DATAC15") : tr("the floor of a BW500 session"));
        }
    }
    if (ch) { if (!S.mask) S.mask = 1u << dect2::mercury::kDatac16; plat::prefs().setI("mercuryModes", (int)S.mask); }
}

// ---------------------------------------------------------------- hooks

std::vector<std::string> splitCalls(const char* s) {
    std::vector<std::string> v;
    std::string cur;
    for (const char* p = s; ; p++) {
        if (*p == 0 || *p == ',' || *p == ' ' || *p == ';') { if (!cur.empty()) v.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return v;
}

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::MercuryReceiver& r = a.engine.mercury();
    if (!S.wasRunning || (int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    r.setDemod(dect2::hfDemodResolve(S.demod, a.freqMhz));   // Auto follows the dial
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    if (!S.wasRunning || S.pushedCalls != S.calls) { r.setKnownCalls(splitCalls(S.calls)); S.pushedCalls = S.calls; }
    if (!S.wasRunning || S.pushedMask != S.mask) { r.setModeMask(S.mask); S.pushedMask = S.mask; }
    S.wasRunning = true;
}

void tuner(App& a, bool&) {
    load();
    hfDemodControl(a, S.demod, "mercuryDemod", "mercury");
}

void tab(App& a) {
    load();
    presets(a);
    ImGui::Spacing();
    subNav("merv", S.view, {"Frames", "Sessions", "Stations", "Broadcast", "Modes"});
    if (S.view == 4) { modesView(a); return; }
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) framesView(a); else if (S.view == 1) sessionsView(a); else if (S.view == 2) stationsView(a); else broadcastView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (!ImGui::BeginTable("##merl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    char b[32];
    for (const auto& s : a.rx.mercury.stations) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(kCallCol, "%s", s.call.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); utcText((double)s.lastSeen, b, sizeof b); ImGui::TextDisabled("%s, %llux", b, (unsigned long long)s.count);
    }
    ImGui::EndTable();
    if (a.rx.mercury.stations.empty()) ImGui::TextDisabled("%s", tr("no stations yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::MercuryTelemetry& t = a.rx.mercury;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::mercurySummary(t).c_str());
    kv(tr("bursts"), "%llu, %llu %s", (unsigned long long)t.bursts, (unsigned long long)t.fecFailed, tr("not decoded"));
    kv(tr("frames"), "%llu %s, %llu %s, %llu %s", (unsigned long long)t.plainFrames, tr("plain"), (unsigned long long)t.seededFrames, tr("in sessions"),
       (unsigned long long)t.broadcastFrames, tr("broadcast"));
    if (t.encryptedFrames) kv(tr("encrypted"), "%llu %s", (unsigned long long)t.encryptedFrames, tr("frames, not decoded"));
    std::string pm;
    for (int m = 0; m < dect2::kMercuryModes; m++) {
        if (!t.perMode[m]) continue;
        char b[48]; snprintf(b, sizeof b, "%s%s %llu", pm.empty() ? "" : ", ", dect2::mercury::modeName(m), (unsigned long long)t.perMode[m]);
        pm += b;
    }
    kv(tr("per mode"), "%s", pm.empty() ? "-" : pm.c_str());
    kv(tr("last burst"), "%s, %+.1f dB, %+.1f Hz", t.lastMode >= 0 ? dect2::mercury::modeName(t.lastMode) : "-", t.snrDb, t.cfoHz);
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
    if (t.constellation.size() >= 2) {
        std::vector<cf32> pts;
        for (size_t i = 0; i + 1 < t.constellation.size(); i += 2) pts.push_back(cf32(t.constellation[i], t.constellation[i + 1]));
        const float s = std::min(220.f * gUi, ImGui::GetContentRegionAvail().x);
        scatter("##mercon", pts, ImVec2(s, s), t.lastMode == dect2::mercury::kQam16c2 ? 1.6 : 1.5, ImVec4(0.55f, 0.85f, 0.95f, 1));
    }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.mercury.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Frames"), !on ? 0 : a.rx.mercury.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::mercurySummary(a.rx.mercury).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Mercury";
    if (live(a)) l2 = dect2::mercurySummary(a.rx.mercury);
}

// the test signal's options (SynthConfig::modeOpt, see mercury_gen.h)
void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    const char* scripts[] = {"demo link + broadcast", "broadcast only", "one mode", "every mode once", "a link at the MFSK floor"};
    int w = std::max(0, std::min(4, sc.modeOpt[0]));
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::Combo("##mersyn", &w, scripts, 5)) { sc.modeOpt[0] = w; changed = true; }
    if (w == 2) {
        flowNext();
        int m = std::max(0, std::min(dect2::mercury::kNumModes - 1, sc.modeOpt[1]));
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::BeginCombo("##mersynm", dect2::mercury::modeName(m))) {
            for (int i = 0; i < dect2::mercury::kNumModes; i++) if (ImGui::Selectable(dect2::mercury::modeName(i), i == m)) { sc.modeOpt[1] = i; changed = true; }
            ImGui::EndCombo();
        }
    }
    flowNext();
    int ch = std::max(0, std::min((int)dect2::mercury::kNumFadings - 1, sc.modeOpt[2]));
    ImGui::SetNextItemWidth(190 * gUi);
    if (ImGui::BeginCombo("##mersynch", dect2::mercury::fadingName(ch))) {
        for (int i = 0; i < dect2::mercury::kNumFadings; i++) if (ImGui::Selectable(dect2::mercury::fadingName(i), i == ch)) { sc.modeOpt[2] = i; changed = true; }
        ImGui::EndCombo();
    }
    flowEnd();
}

} // namespace

extern const ModeUi kMercuryUi;
const ModeUi kMercuryUi = {
    .sideTitle = "STATIONS",
    .tabName = "Mercury",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
};
