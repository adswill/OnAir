// onair-videogen (vg_state.h): the window. Easy mode: one tab (one source, sent to OnAir's transmitter at the rate it can carry).
// Advanced: a tab per source ("+" adds one), "Muxing" (the programmes, the channel's capacity, the output) and "Preview" (the
// picture of a source and its overlay editor). Start / Stop, the Advanced box and the link to OnAir stay above the tabs.
#include "vg_window.h"
#include "vg_state.h"
#include "platform.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace vgui {

Gen& G() {
    static Gen g;
    return g;
}

namespace {
std::string gSelectTab;
int gSelectFrames = 0;
// the flags of a tab: selected on the first frames when --tab asked for it
ImGuiTabItemFlags tabFlags(const std::string& name) { return gSelectFrames > 0 && gSelectTab == name ? ImGuiTabItemFlags_SetSelected : 0; }

const char* const kMuxOutputNames[] = {N_("Transmitter (OnAir)"), N_("UDP stream"), N_("RTP stream"), N_("MPEG-TS file (.ts)")};
const Output kMuxOutputs[] = {Output::Transmitter, Output::Udp, Output::Rtp, Output::File};
const ImVec4 kRed(0.95f, 0.4f, 0.35f, 1), kOrange(0.95f, 0.65f, 0.2f, 1), kGreen(0.45f, 0.85f, 0.5f, 1);

int outputIndex(Output o) {
    for (int i = 0; i < 4; i++) if (kMuxOutputs[i] == o) return i;
    return 0;
}

std::string muxPath() { return plat::dataDir() + "/videogen_mux.txt"; }

std::string intList(const std::vector<int>& v) {
    std::string o;
    for (int x : v) o += (o.empty() ? "" : ",") + std::to_string(x);
    return o;
}

std::vector<int> parseIntList(const std::string& t) {
    std::vector<int> v;
    size_t at = 0;
    while (at < t.size() && v.size() < 8) {
        size_t e = t.find(',', at);
        if (e == std::string::npos) e = t.size();
        v.push_back(std::clamp(atoi(t.substr(at, e - at).c_str()), 0, 0x1FFE));
        at = e + 1;
    }
    return v;
}

std::string saveMux(const Gen& g) {
    VgKv kv{{"sources", std::to_string(g.src.size())}, {"advanced", g.advanced ? "1" : "0"}, {"out", std::to_string(outputIndex(g.mux.output))},
            {"ts", g.mux.tsPath}, {"host", g.mux.host}, {"port", std::to_string(g.mux.port)}, {"ttl", std::to_string(g.mux.ttl)},
            {"caplink", g.mux.capFromLink ? "1" : "0"}, {"cap", std::to_string(g.mux.capKbps)}, {"pad", g.mux.padding ? "1" : "0"},
            {"first", std::to_string(g.mux.firstServiceId)}, {"tsid", std::to_string(g.mux.tsid)}, {"provider", g.mux.provider},
            {"preview", std::to_string(g.previewSrc)}};
    for (const auto& s : g.src) {
        const std::string k = "n" + std::to_string(s->idx + 1) + ".";
        kv[k + "sid"] = std::to_string(s->num.serviceId);
        kv[k + "pmt"] = std::to_string(s->num.pmtPid);
        kv[k + "es"] = intList(s->num.esPids);
    }
    return vgKvText(kv);
}

void loadMux(Gen& g, const VgKv& kv) {
    g.advanced = vgKvNum(kv, "advanced", 0) != 0;
    g.mux.output = kMuxOutputs[std::clamp((int)vgKvNum(kv, "out", 0), 0, 3)];
    if (!vgKvStr(kv, "ts").empty()) g.mux.tsPath = vgKvStr(kv, "ts");
    if (!vgKvStr(kv, "host").empty()) g.mux.host = vgKvStr(kv, "host");
    g.mux.port = std::clamp((int)vgKvNum(kv, "port", 5000), 1, 65535);
    g.mux.ttl = std::clamp((int)vgKvNum(kv, "ttl", 8), 1, 255);
    g.mux.capFromLink = vgKvNum(kv, "caplink", 1) != 0;
    g.mux.capKbps = std::clamp((int)vgKvNum(kv, "cap", 10000), 100, 200000);
    g.mux.padding = vgKvNum(kv, "pad", 1) != 0;
    g.mux.firstServiceId = std::clamp((int)vgKvNum(kv, "first", 1), 1, 65535 - kMaxSources);
    g.mux.tsid = std::clamp((int)vgKvNum(kv, "tsid", 1), 0, 65535);
    g.mux.provider = vgKvStr(kv, "provider", g.mux.provider).substr(0, 60);
    g.previewSrc = std::clamp((int)vgKvNum(kv, "preview", 0), 0, kMaxSources - 1);
}

void loadNumbering(SourceTab& s, const VgKv& kv) {
    const std::string k = "n" + std::to_string(s.idx + 1) + ".";
    s.num.serviceId = std::clamp((int)vgKvNum(kv, (k + "sid").c_str(), 0), 0, 65535);
    s.num.pmtPid = std::clamp((int)vgKvNum(kv, (k + "pmt").c_str(), 0), 0, 0x1FFE);
    s.num.esPids = parseIntList(vgKvStr(kv, (k + "es").c_str()));
}

// a source tab from its saved settings (videogen.txt, videogen_2.txt ...: the files of the old in-OnAir generators are taken as they are)
std::unique_ptr<SourceTab> makeSource(Gen& g, int idx, const VgKv& mux) {
    auto s = std::make_unique<SourceTab>();
    s->idx = idx;
    s->cfg.instanceId = idx + 1;
    s->cfg.output = Output::Queue;
    if (idx > 0) s->cfg.enc.serviceName = "OnAir video " + std::to_string(idx + 1);
    const std::string c = readText(cfgPath(idx));
    bool adv = false;
    if (!c.empty()) { loadCfg(c, s->cfg, adv); s->lengthEdited = true; }
    s->cfg.output = Output::Queue;
    const std::string l = readText(layoutPath(idx));
    if (!l.empty()) s->cfg.overlay = loadLayout(l);
    s->savedCfg = saveCfg(s->cfg, false);
    s->savedLayout = saveLayout(s->cfg.overlay);
    s->ovr.setFont(g.font);
    loadNumbering(*s, mux);
    return s;
}

void saveSoon(Gen& g) {
    const double now = ImGui::GetTime();
    if (now - g.lastSave < 1.0) return;
    g.lastSave = now;
    for (auto& s : g.src) {
        const std::string c = saveCfg(s->cfg, g.advanced), l = saveLayout(s->cfg.overlay);
        if (c != s->savedCfg) { writeText(cfgPath(s->idx), c); s->savedCfg = c; }
        if (l != s->savedLayout) { writeText(layoutPath(s->idx), l); s->savedLayout = l; }
    }
    const std::string m = saveMux(g);
    if (m != g.savedMux) { writeText(muxPath(), m); g.savedMux = m; }
}

// ---- rates -------------------------------------------------------------------------------------------------------------------

double usableBps(const Gen& g) {
    double u = 0;
    g.link.txInfo(u);
    return g.link.connected() ? u : 0;
}

Output muxOutput(const Gen& g) { return g.advanced ? g.mux.output : Output::Transmitter; }

// what the multiplex may use: the transmitter's usable rate for this generator, or what was typed in; 0: unknown
double capacityBps(const Gen& g) {
    if (!g.advanced) return usableBps(g);
    if (g.mux.capFromLink) return g.mux.output == Output::Transmitter ? usableBps(g) : 0;
    return g.mux.capKbps * 1000.0;
}

int activeSources(const Gen& g) { return g.advanced ? (int)g.src.size() : 1; }

// Easy mode: the source's encoding fitted to the transmitter's usable rate (2 % left for the multiplex's own PAT and SDT)
Settings easySettings(const Gen& g, const SourceTab& s, bool& fits) {
    Settings e = s.cfg.enc;
    fits = true;
    const double u = usableBps(g);
    if (u > 0) fits = fitVideoToMux(e, (int)(u * 0.98 / 1000));
    return e;
}

GenConfig runConfig(const Gen& g, const SourceTab& s) {
    GenConfig r = s.cfg;
    if (!g.advanced) { bool fits = true; r.enc = easySettings(g, s, fits); }
    r.enc.videoEncoder = pickVideoEncoder(r.enc.videoEncoder, videoEncoders());
    r.font = g.font;
    r.instanceId = s.idx + 1;
    return r;
}

bool sourceSending(const Gen& g, int k) { return g.ses.running() && g.ses.sourceRunning(k) && !g.ses.sourcePreviewOnly(k); }

// ---- start / stop ------------------------------------------------------------------------------------------------------------

void startMux(Gen& g) {
    const Output out = muxOutput(g);
    if (out == Output::Transmitter && !g.link.connected()) {
        g.msg = tr("OnAir's transmitter is not linked: start OnAir (or check the address under Muxing); the generator finds it by itself.");
        return;
    }
    std::vector<Session::SourceSpec> specs;
    for (int k = 0; k < (int)g.src.size(); k++) {
        Session::SourceSpec sp;
        sp.enabled = k < activeSources(g);
        sp.cfg = runConfig(g, *g.src[(size_t)k]);
        if (g.advanced) sp.num = g.src[(size_t)k]->num;
        specs.push_back(sp);
    }
    MuxConfig m;
    m.output = out;
    m.tsPath = g.mux.tsPath;
    m.host = g.mux.host;
    m.port = g.mux.port;
    m.ttl = g.mux.ttl;
    m.channelBps = capacityBps(g);
    m.padding = g.advanced ? g.mux.padding : true;
    m.firstServiceId = g.mux.firstServiceId;
    m.tsid = g.mux.tsid;
    m.provider = g.advanced ? g.mux.provider : std::string();
    m.link = &g.link;
    std::string err;
    if (!g.ses.start(specs, m, err)) { g.msg = err; return; }
    g.msg.clear();
    g.startedAt = ImGui::GetTime();
    for (auto& s : g.src) { s->msg.clear(); s->note.clear(); s->sentLayout = saveLayout(s->cfg.overlay); }
}

// ---- the preview: the preview thread shows one source; a source that is not streaming runs for the picture only -------------

void previewTick(Gen& g, int k) {
    if (k < 0 || k >= (int)g.src.size()) return;
    SourceTab& s = *g.src[(size_t)k];
    if (!g.previewOn) {
        if (g.ses.previewRunning()) g.ses.stopPreview();
        for (int i = 0; i < kMaxSources; i++) g.ses.stopSourcePreview(i);
        return;
    }
    for (int i = 0; i < kMaxSources; i++) if (i != k) g.ses.stopSourcePreview(i);   // only the shown one runs for its picture
    if (!sourceSending(g, k)) {
        const std::string key = pictureKey(s.cfg);
        if (!g.ses.sourceRunning(k) || key != s.previewKey) {
            std::string err;
            if (g.ses.startPreviewOnly(k, runConfig(g, s), err)) { s.previewKey = key; s.sentLayout = saveLayout(s.cfg.overlay); s.msg.clear(); }
            else { s.msg = err; g.previewOn = false; return; }
        }
    }
    g.ses.startPreview(k);
    static std::vector<uint8_t> rgba;
    static uint64_t serial = 0;
    int w = 0, h = 0;
    if (g.ses.previewFrame(rgba, w, h, serial)) {
        if (!s.prevImg || w != s.prevW || h != s.prevH) { delete s.prevImg; s.prevImg = gGfx->createImage(w, h, 0xFF000000u); s.prevW = w; s.prevH = h; }
        s.prevImg->update(0, 0, w, h, reinterpret_cast<const uint32_t*>(rgba.data()));
    }
}

bool previewing(const Gen& g, int k) { return g.previewOn && g.previewSrc == k && g.ses.previewRunning() && g.src[(size_t)k]->prevImg; }

// ---- above the tabs ----------------------------------------------------------------------------------------------------------

std::string clock(double s) {
    const int t = (int)s;
    std::array<char, 32> b{};
    snprintf(b.data(), b.size(), "%d:%02d", t / 60, t % 60);
    return b.data();
}

void linkLine(Gen& g) {
    double u = 0;
    const VgTxInfo t = g.link.txInfo(u);
    ImGui::AlignTextToFramePadding();
    if (!g.link.connected()) {
        ImGui::TextColored(kOrange, "%s", tr("Not linked to OnAir"));
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", g.link.status().c_str());
        return;
    }
    ImGui::TextColored(kGreen, "%s", g.link.status().c_str());
    ImGui::SameLine();
    if (!t.digitalTv) ImGui::TextDisabled("%s", tr("- the transmitter has no digital TV mode chosen"));
    else ImGui::TextDisabled("- %s %s, %s %s%s", t.system.c_str(), t.params.c_str(), tr("usable for this generator:"), kbpsText(u / 1000).c_str(),
                             t.running ? "" : tr(" (transmitter stopped)"));
}

void topBar(Gen& g) {
    const bool running = g.ses.running();
    const Output out = muxOutput(g);
    if (!running) {
        ImGui::BeginDisabled(!g.probed);
        if (ImGui::Button(out == Output::File ? tr("Start encoding") : tr("Start streaming"))) startMux(g);
        ImGui::EndDisabled();
    } else if (ImGui::Button(tr("Stop"))) g.ses.stop();
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    ImGui::Checkbox(tr("Advanced"), &g.advanced);
    ImGui::EndDisabled();
    helpMark(N_("Off (Easy): one source, sent to OnAir's transmitter at the bitrate it can carry. On: several sources as programmes of one multiplex, the numbering, the output (transmitter, UDP, RTP or a .ts file) and every encoder setting. Changing it keeps every setting."));
    ImGui::SameLine();
    helpMark(kToolHelp);
    ImGui::SameLine();
    const MuxStatus ms = g.ses.muxStatus();
    if (running) {
        if (out == Output::File) {
            double done = 1;
            for (int k = 0; k < activeSources(g); k++) done = std::min(done, g.ses.sourceStatus(k).gen.progress);
            ImGui::ProgressBar((float)done, ImVec2(200 * gUi, 0));
            ImGui::SameLine();
            ImGui::Text("%s: %.1f s, %s", tr("Encoding"), ms.seconds, kbpsText(ms.rateBps / 1000).c_str());
        } else
            ImGui::Text("%s: %s, %s, %d %s", tr("Streaming"), clock(ImGui::GetTime() - g.startedAt).c_str(), kbpsText(ms.rateBps / 1000).c_str(),
                        (int)ms.programmes.size(), tr("programme(s)"));
        if (ms.overload) { ImGui::SameLine(); ImGui::TextColored(kOrange, "%s", tr("overload: the sources need more than the multiplex carries")); }
    } else if (!g.msg.empty()) ImGui::TextColored(kRed, "%s", g.msg.c_str());
    else if (!ms.error.empty()) ImGui::TextColored(kRed, "%s", ms.error.c_str());
    else if (ms.finished && out == Output::File && ms.packets > 0)
        ImGui::TextDisabled("%s: %s (%.1f s). %s", tr("Written"), g.mux.tsPath.c_str(), ms.seconds, tr("The transmitter takes it as its \"MPEG-TS / video file\"."));
    else if (!g.probed) ImGui::TextDisabled("%s", tr("Looking for encoders and cameras..."));
    else ImGui::NewLine();
    linkLine(g);
}

// ---- Easy ----------------------------------------------------------------------------------------------------------------------

void easyTab(Gen& g) {
    SourceTab& s = *g.src[0];
    const bool sending = sourceSending(g, 0);
    const float right = std::min(340 * gUi, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("##vgeasy", ImVec2(ImGui::GetContentRegionAvail().x - right - ImGui::GetStyle().ItemSpacing.x, 0));
    const float col = 175 * gUi, w = std::max(160 * gUi, std::min(360 * gUi, ImGui::GetContentRegionAvail().x - col - 8 * gUi));
    ImGui::BeginDisabled(sending);
    sourceSection(s, col, w, false);
    ImGui::SeparatorText(tr("Video"));
    frameRows(s.cfg.enc, col, w);
    ImGui::EndDisabled();
    ImGui::SeparatorText(tr("Preview"));
    g.previewSrc = 0;
    if (ImGui::Button(g.previewOn ? tr("Stop preview") : tr("Start preview"))) g.previewOn = !g.previewOn;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("Advanced > Preview edits the overlay (logo, text)."));
    const float ph = std::max(120 * gUi, std::min(ImGui::GetContentRegionAvail().y - 8 * gUi, 300 * gUi));
    ImGui::BeginChild("##vgeasyprev", ImVec2(ph * (float)s.cfg.enc.width / (float)std::max(1, s.cfg.enc.height), ph + 2 * ImGui::GetTextLineHeightWithSpacing()));
    canvas(s, false, sending, previewing(g, 0));
    ImGui::EndChild();
    if (!s.msg.empty()) ImGui::TextColored(kRed, "%s", s.msg.c_str());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##vgeasyest", ImVec2(0, 0));
    bool fits = true;
    const Settings e = easySettings(g, s, fits);
    ImGui::SeparatorText(tr("Transmitter"));
    ImGui::PushTextWrapPos(0);
    if (!g.link.connected()) ImGui::TextColored(kOrange, "%s", tr("No transmitter linked: start OnAir; the generator links by itself. Easy mode sends to OnAir's transmitter only (Advanced also writes files and sends UDP / RTP)."));
    else if (usableBps(g) <= 0) ImGui::TextColored(kOrange, "%s", tr("Choose a digital TV mode in OnAir's Transmit panel: the bitrate follows what it can carry."));
    else {
        ImGui::Text("%s %s.", tr("The bitrate follows the transmitter:"), kbpsText(usableBps(g) / 1000).c_str());
        if (!fits) ImGui::TextColored(kRed, "%s", tr("Even the lowest video bitrate does not fit: choose a smaller resolution, a lower frame rate or another mode in the transmitter."));
    }
    if (g.src.size() > 1) ImGui::TextColored(kOrange, tr("Easy mode sends Source 1 only; the other %d source(s) are kept for Advanced."), (int)g.src.size() - 1);
    ImGui::PopTextWrapPos();
    estimatePanel(e);
    ImGui::EndChild();
}

// ---- Advanced: a source --------------------------------------------------------------------------------------------------------

void sourceTab(Gen& g, SourceTab& s) {
    const bool sending = sourceSending(g, s.idx);
    const SourceStatus st = g.ses.sourceStatus(s.idx);
    if (g.ses.running()) {   // one source while the multiplex runs: its programme is padded while it is stopped
        if (sending) { if (ImGui::Button(tr("Stop this source"))) g.ses.stopSource(s.idx); }
        else if (ImGui::Button(tr("Start this source"))) {
            std::string err;
            if (g.ses.startSource(s.idx, runConfig(g, s), s.num, err)) s.msg.clear();
            else s.msg = err;
        }
        ImGui::SameLine();
    }
    if (sending) {
        ImGui::Text("%s: %llu %s, %.0f kbit/s, %s %.0f %%", tr("Sending"), (unsigned long long)st.gen.frames, tr("frames"), st.gen.kbps, tr("CPU"), st.cpuBusy);
        if (st.dropped) { ImGui::SameLine(); ImGui::TextColored(kOrange, tr("%llu packets dropped: the multiplex was behind"), (unsigned long long)st.dropped); }
        if (st.gen.lateFrames > 0) { ImGui::SameLine(); ImGui::TextColored(kOrange, tr("%d frames late: the encoder is too slow for real time"), st.gen.lateFrames); }
        if (!st.gen.notice.empty()) { ImGui::SameLine(); ImGui::TextColored(kOrange, "%s", tr("(simplified)")); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.gen.notice.c_str()); }
    } else if (!st.gen.error.empty()) ImGui::TextColored(kRed, "%s", st.gen.error.c_str());
    else if (!s.msg.empty()) ImGui::TextColored(kRed, "%s", s.msg.c_str());
    else if (!s.note.empty()) ImGui::TextDisabled("%s", s.note.c_str());
    else ImGui::TextDisabled("%s", g.ses.running() ? tr("Not in the multiplex now: its programme is padded") : tr("Sent with the others when you press Start"));
    const float right = std::min(340 * gUi, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("##vgsettings", ImVec2(ImGui::GetContentRegionAvail().x - right - ImGui::GetStyle().ItemSpacing.x, 0));
    ImGui::BeginDisabled(sending);
    const float col = 175 * gUi, w = std::max(160 * gUi, std::min(360 * gUi, ImGui::GetContentRegionAvail().x - col - 8 * gUi));
    sourceSection(s, col, w, true);
    if (g.mux.output == Output::File) {
        lengthRow(s, col, true);
    }
    encodingSection(s, col, w);
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##vgestimate", ImVec2(0, 0));
    estimatePanel(s.cfg.enc);
    ImGui::EndChild();
}

// ---- Advanced: Muxing ------------------------------------------------------------------------------------------------------------

void numberingRow(Gen& g, SourceTab& s, bool running) {
    const int k = s.idx;
    const Settings& e = s.cfg.enc;
    const int tracks = audioTrackCount(e);
    const int streams = (e.content == TsContent::VideoOnly ? 0 : tracks) + 1;
    bool autoNum = s.num.serviceId == 0 && s.num.pmtPid == 0 && s.num.esPids.empty();
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("%s %d%s", tr("Source"), k + 1, k < activeSources(g) ? "" : "");
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(running);
    ImGui::PushID(k);
    if (ImGui::Checkbox(tr("Automatic"), &autoNum)) {
        if (autoNum) s.num = MuxNumbering();
        else {
            s.num.serviceId = g.mux.firstServiceId + k;
            s.num.pmtPid = dect2::TsRemux::pmtPid(k);
            s.num.esPids.clear();
            for (int j = 0; j < streams; j++) s.num.esPids.push_back(dect2::TsRemux::esPidBase(k) + j);
        }
    }
    ImGui::TableNextColumn();
    if (autoNum) ImGui::TextDisabled("%d", g.mux.firstServiceId + k);
    else { ImGui::SetNextItemWidth(80 * gUi); if (ImGui::InputInt("##sid", &s.num.serviceId, 0)) s.num.serviceId = std::clamp(s.num.serviceId, 1, 65535); }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1);
    inputText("##sname", s.cfg.enc.serviceName, ImGui::GetContentRegionAvail().x);
    ImGui::TableNextColumn();
    if (autoNum) ImGui::TextDisabled("0x%04X", dect2::TsRemux::pmtPid(k));
    else { ImGui::SetNextItemWidth(80 * gUi); if (ImGui::InputInt("##pmt", &s.num.pmtPid, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) s.num.pmtPid = std::clamp(s.num.pmtPid, 0x20, 0x1FFE); }
    ImGui::TableNextColumn();
    if (autoNum) {
        std::string t;
        for (int j = 0; j < streams; j++) { std::array<char, 16> b{}; snprintf(b.data(), b.size(), "0x%04X", dect2::TsRemux::esPidBase(k) + j); t += (j ? " " : "") + std::string(b.data()); }
        ImGui::TextDisabled("%s", t.c_str());
    } else {
        s.num.esPids.resize((size_t)streams, 0);
        for (int j = 0; j < streams; j++) {
            ImGui::PushID(j);
            if (j) ImGui::SameLine();
            ImGui::SetNextItemWidth(62 * gUi);
            if (ImGui::InputInt("##es", &s.num.esPids[(size_t)j], 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) s.num.esPids[(size_t)j] = std::clamp(s.num.esPids[(size_t)j], 0x20, 0x1FFE);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", j == 0 ? tr("Video PID") : tr("Audio PID"));
            ImGui::PopID();
        }
    }
    ImGui::PopID();
    ImGui::EndDisabled();
    ImGui::TableNextColumn();
    const double kbps = estimate(e).mux;
    const double cap = capacityBps(g) / 1000;
    if (cap > 0) ImGui::Text("%s (%.0f %%)", kbpsText(kbps).c_str(), 100 * kbps / cap);
    else ImGui::TextUnformatted(kbpsText(kbps).c_str());
}

void muxingTab(Gen& g) {
    const bool running = g.ses.running();
    ImGui::BeginChild("##vgmux", ImVec2(0, 0));
    ImGui::SeparatorText(tr("Programmes"));
    if (ImGui::BeginTable("##vgprogs", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn(tr("Source"));
        ImGui::TableSetupColumn(tr("Numbers"));
        ImGui::TableSetupColumn(tr("Programme"));
        ImGui::TableSetupColumn(tr("Service name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("PMT PID"));
        ImGui::TableSetupColumn(tr("Stream PIDs"));
        ImGui::TableSetupColumn(tr("Share"));
        ImGui::TableHeadersRow();
        for (auto& s : g.src) numberingRow(g, *s, running);
        ImGui::EndTable();
    }
    if (running) {
        const MuxStatus ms = g.ses.muxStatus();
        std::string t;
        for (const auto& p : ms.programmes) {
            std::array<char, 96> b{};
            snprintf(b.data(), b.size(), "%d (PMT 0x%04X, %s %d)", p.serviceId, p.pmtPid, tr("source"), p.input + 1);
            t += (t.empty() ? "" : ", ") + std::string(b.data());
        }
        ImGui::TextDisabled("%s: %s", tr("On air now"), t.empty() ? tr("(none yet)") : t.c_str());
    }
    const float col = 190 * gUi;
    ImGui::BeginDisabled(running);
    rowLabel(N_("First programme number"), col);
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::InputInt("##vgfirst", &g.mux.firstServiceId, 0)) g.mux.firstServiceId = std::clamp(g.mux.firstServiceId, 1, 65535 - kMaxSources);
    rowLabel(N_("Transport stream ID"), col);
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::InputInt("##vgtsid", &g.mux.tsid, 0)) g.mux.tsid = std::clamp(g.mux.tsid, 0, 65535);
    rowLabel(N_("Provider"), col);
    inputText("##vgprov", g.mux.provider, 260 * gUi);

    ImGui::SeparatorText(tr("Capacity"));
    rowLabel(N_("Channel capacity"), col);
    if (ImGui::RadioButton(tr("From the transmitter"), g.mux.capFromLink)) g.mux.capFromLink = true;
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Typed in"), !g.mux.capFromLink)) g.mux.capFromLink = false;
    if (!g.mux.capFromLink) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt("##vgcap", &g.mux.capKbps, 100, 1000)) g.mux.capKbps = std::clamp(g.mux.capKbps, 100, 200000);
        ImGui::SameLine();
        ImGui::TextUnformatted(tr("kbit/s"));
    }
    rowLabel(N_("Padding"), col);
    ImGui::Checkbox(tr("Fill up to the capacity with null packets"), &g.mux.padding);
    ImGui::EndDisabled();
    double need = 0;
    for (int k = 0; k < activeSources(g); k++) need += estimate(g.src[(size_t)k]->cfg.enc).mux;
    const double cap = capacityBps(g) / 1000;
    if (cap <= 0) ImGui::TextColored(kOrange, "%s", g.mux.capFromLink ? tr("The capacity is not known: link to OnAir's transmitter with a digital TV mode chosen, or type it in.") : "");
    else {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float bw = std::min(ImGui::GetContentRegionAvail().x, 600 * gUi), bh = 16 * gUi;
        const double full = std::max(need, cap);
        dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh), ImGui::GetColorU32(ImGuiCol_FrameBg));
        float x = p0.x;
        for (int k = 0; k < activeSources(g); k++) {
            const float pw = (float)(estimate(g.src[(size_t)k]->cfg.enc).mux / full) * bw;
            dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + pw, p0.y + bh), k % 2 ? ImGui::GetColorU32(ImGuiCol_PlotLines) : ImGui::GetColorU32(ImGuiCol_PlotHistogram));
            dl->AddLine(ImVec2(x + pw, p0.y), ImVec2(x + pw, p0.y + bh), IM_COL32(0, 0, 0, 160));
            x += pw;
        }
        dl->AddLine(ImVec2(p0.x + (float)(cap / full) * bw, p0.y - 2), ImVec2(p0.x + (float)(cap / full) * bw, p0.y + bh + 2), IM_COL32(230, 60, 60, 255), 2 * gUi);
        ImGui::Dummy(ImVec2(bw, bh));
        ImGui::Text("%s %s %s %s (%.0f %%)", tr("The sources need"), kbpsText(need).c_str(), tr("of"), kbpsText(cap).c_str(), 100 * need / cap);
        if (need > cap * 0.98)
            ImGui::TextColored(kRed, "%s", tr("Overload: the sources need more than the channel carries. Lower a bitrate (each source tab) or remove a source, or receivers lose pictures."));
    }

    ImGui::SeparatorText(tr("Output"));
    ImGui::BeginDisabled(running);
    rowLabel(N_("Send to"), col);
    int out = outputIndex(g.mux.output);
    if (combo("##vgmuxout", out, kMuxOutputNames, 4, 260 * gUi)) g.mux.output = kMuxOutputs[out];
    if (g.mux.output == Output::File) {
        rowLabel(N_("File"), col);
        inputText("##vgts", g.mux.tsPath, 360 * gUi);
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse...##vgts"))) { const std::string p = plat::saveFileDialog("onair_video.ts"); if (!p.empty()) g.mux.tsPath = p; }
        ImGui::TextDisabled("%s", tr("Each source's length is on its tab; the file ends with the longest."));
    } else if (g.mux.output == Output::Transmitter) {
        rowLabel(N_("Name for the transmitter"), col);
        const std::string spec = g.link.connected() ? generatorSpec(g.link.slot()) : std::string(tr("(not linked)"));
        ImGui::TextUnformatted(spec.c_str());
        ImGui::TextDisabled("%s", tr("In OnAir's Transmit panel choose \"Video generator\" as the video source: it lists the linked generators."));
    } else {
        rowLabel(N_("Address"), col);
        inputText("##vghost", g.mux.host, 200 * gUi);
        ImGui::SameLine();
        ImGui::TextUnformatted(tr("Port"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputInt("##vgport", &g.mux.port, 0)) g.mux.port = std::clamp(g.mux.port, 1, 65535);
        if (isMulticast(g.mux.host)) {
            rowLabel(N_("TTL"), col);
            ImGui::SetNextItemWidth(90 * gUi);
            if (ImGui::InputInt("##vgttl", &g.mux.ttl)) g.mux.ttl = std::clamp(g.mux.ttl, 1, 255);
        }
        const std::string vlc = vlcOpenUrl(g.mux.output, g.mux.host, g.mux.port);
        ImGui::TextDisabled("%s: %s > %s", tr("In VLC on the receiving PC"), tr("Media > Open Network Stream"), vlc.c_str());
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText(tr("Threads"));
    if (ImGui::BeginTable("##vgthreads", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        for (const char* h : {N_("Thread"), N_("State"), N_("Frames / packets"), N_("Dropped"), N_("Queue"), N_("CPU")}) ImGui::TableSetupColumn(tr(h));
        ImGui::TableHeadersRow();
        for (int k = 0; k < (int)g.src.size(); k++) {
            const SourceStatus st = g.ses.sourceStatus(k);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("vg-src-%d", k + 1);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(!st.running ? tr("stopped") : st.previewOnly ? tr("preview only") : tr("encoding"));
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)st.gen.frames);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)st.dropped);
            ImGui::TableNextColumn(); ImGui::Text("%zu", st.queued);
            ImGui::TableNextColumn(); ImGui::Text("%.0f %%", st.cpuBusy);
        }
        const MuxStatus ms = g.ses.muxStatus();
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("vg-mux");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ms.running ? tr("running") : tr("stopped"));
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)ms.packets);
        ImGui::TableNextColumn(); ImGui::Text("%llu %s", (unsigned long long)ms.nulls, tr("null"));
        ImGui::TableNextColumn(); ImGui::TextUnformatted("");
        ImGui::TableNextColumn(); ImGui::Text("%.0f %%", ms.cpuBusy);
        const PreviewStatus ps = g.ses.previewStatus();
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("vg-preview");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ps.running ? tr("running") : tr("stopped"));
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)ps.frames);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)ps.dropped);
        ImGui::TableNextColumn(); ImGui::TextUnformatted("");
        ImGui::TableNextColumn(); ImGui::Text("%.0f %%", ps.cpuBusy);
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// ---- Advanced: Preview -----------------------------------------------------------------------------------------------------------

void previewTab(Gen& g) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Source"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * gUi);
    const std::string cur = std::string(tr("Source")) + " " + std::to_string(g.previewSrc + 1);
    if (ImGui::BeginCombo("##vgprevsrc", cur.c_str())) {
        for (int k = 0; k < (int)g.src.size(); k++) {
            const std::string n = std::string(tr("Source")) + " " + std::to_string(k + 1) + (sourceSending(g, k) ? " *" : "");
            if (ImGui::Selectable(n.c_str(), g.previewSrc == k)) g.previewSrc = k;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(g.previewOn ? tr("Stop preview") : tr("Start preview"))) g.previewOn = !g.previewOn;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Shows the moving picture with its overlay. A source that is not streaming runs for the picture only: nothing is encoded or sent, and the red cross stays."));
    SourceTab& s = *g.src[(size_t)g.previewSrc];
    if (!s.msg.empty()) { ImGui::SameLine(); ImGui::TextColored(kRed, "%s", s.msg.c_str()); }
    overlayEditor(s, sourceSending(g, s.idx), previewing(g, s.idx));
}

void closeSource(Gen& g, int k) {
    if ((int)g.src.size() <= 1 || g.ses.running()) return;
    g.ses.stopSourcePreview(k);
    delete g.src[(size_t)k]->prevImg;
    delete g.src[(size_t)k]->ovImg;
    g.src.erase(g.src.begin() + k);
    for (int i = 0; i < (int)g.src.size(); i++) {   // the ones after it move up: their files follow
        SourceTab& s = *g.src[(size_t)i];
        if (s.idx == i) continue;
        s.idx = i;
        s.cfg.instanceId = i + 1;
        s.savedCfg.clear();
        s.savedLayout.clear();
    }
    std::remove(cfgPath((int)g.src.size()).c_str());
    std::remove(layoutPath((int)g.src.size()).c_str());
    g.previewSrc = std::min(g.previewSrc, (int)g.src.size() - 1);
    g.cur = std::min(g.cur, (int)g.src.size() - 1);
    g.lastSave = 0;
}

} // namespace

// ---- the program's calls -----------------------------------------------------------------------------------------------------------

void init(const Options& o) {
    Gen& g = G();
    g.font = atlasFont();
    g.linkHost = o.host.empty() ? "127.0.0.1" : o.host;
    g.linkPort = o.port > 0 ? o.port : kVgLinkDefaultPort;
    g.token = o.token;
    g.slot = std::clamp(o.slot, 1, kMaxInstances);
    g.startedByOnAir = !o.token.empty();
    g.mux.tsPath = plat::dataDir() + "/onair_video.ts";
    const std::string mt = readText(muxPath());
    const VgKv mux = vgKvParse(mt);
    int n = 1;
    if (!mt.empty()) { loadMux(g, mux); n = std::clamp((int)vgKvNum(mux, "sources", 1), 1, kMaxSources); }
    else {   // the first start after the generator was part of OnAir: its output and Advanced box come from the first generator's settings
        GenConfig c;
        bool adv = false;
        const std::string t = readText(cfgPath(0));
        if (!t.empty()) {
            loadCfg(t, c, adv);
            g.advanced = adv;
            g.mux.output = c.output == Output::Queue ? Output::Transmitter : c.output;
            if (!c.tsPath.empty()) g.mux.tsPath = c.tsPath;
            g.mux.host = c.host; g.mux.port = c.port; g.mux.ttl = c.ttl;
            g.mux.provider = c.enc.provider;
            for (int i = 1; i < kMaxSources && !readText(cfgPath(i)).empty(); i++) n = i + 1;   // the other generators become sources
        }
    }
    if (o.easy) g.advanced = false;
    if (o.advanced) g.advanced = true;
    for (int i = 0; i < n; i++) g.src.push_back(makeSource(g, i, mux));
    g.savedMux = saveMux(g);
    startProbe(g);
    g.link.start(g.linkHost, g.linkPort, g.name, g.slot, g.token);
}

FrameResult frame() {
    Gen& g = G();
    FrameResult r;
    if (g.link.takeRaise()) r.raise = true;
    if (g.link.takeQuit()) r.quit = true;
    if (!g.probed && g.probe.valid() && g.probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g.probe.get();
        g.cams = g.camsFound;
        g.mics = g.micsFound;
        g.probed = true;
        const auto& ae = audioEncoders();
        for (auto& s : g.src) {
            s->cfg.enc.videoEncoder = pickVideoEncoder(s->cfg.enc.videoEncoder, videoEncoders());
            if (!ae.empty() && std::none_of(ae.begin(), ae.end(), [&](const EncoderInfo& i) { return i.name == s->cfg.enc.audioEncoder; })) s->cfg.enc.audioEncoder = ae.front().name;
        }
    }
    {   // OnAir's transmit panel shows the programmes and whether this generator is in Easy mode (it then greys its service settings)
        std::vector<std::string> progs;
        for (int k = 0; k < activeSources(g); k++) {
            const SourceTab& s = *g.src[(size_t)k];
            progs.push_back(std::to_string(s.num.serviceId ? s.num.serviceId : g.mux.firstServiceId + k) + " " + s.cfg.enc.serviceName);
        }
        g.link.setStatus(!g.advanced, progs);
    }
    if (!g.advanced) g.previewSrc = 0;
    g.previewSrc = std::clamp(g.previewSrc, 0, (int)g.src.size() - 1);
    applyDrops(*g.src[(size_t)(g.advanced ? (g.cur < (int)g.src.size() ? g.cur : 0) : 0)], g.ses.running());
    previewTick(g, g.previewSrc);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##vgmain", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    topBar(g);
    ImGui::Separator();
    if (!g.advanced) {
        if (ImGui::BeginTabBar("##vgtabs")) {
            if (ImGui::BeginTabItem(trw("Easy"))) { easyTab(g); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    } else if (ImGui::BeginTabBar("##vgtabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        int close = -1;
        for (int k = 0; k < (int)g.src.size(); k++) {
            const std::string label = std::string(tr("Source")) + " " + std::to_string(k + 1) + (sourceSending(g, k) ? " *" : "") + "###vgsrc" + std::to_string(k);
            bool open = true;
            const bool closable = g.src.size() > 1 && !g.ses.running();
            if (ImGui::BeginTabItem(label.c_str(), closable ? &open : nullptr, tabFlags("source" + std::to_string(k + 1)))) { g.cur = k; sourceTab(g, *g.src[(size_t)k]); ImGui::EndTabItem(); }
            if (!open) close = k;
        }
        ImGui::BeginDisabled((int)g.src.size() >= kMaxSources);
        if (ImGui::TabItemButton("+")) {
            g.src.push_back(makeSource(g, (int)g.src.size(), VgKv()));
            g.lastSave = 0;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Another source: its own input, encoding and overlay, a programme of its own in the same multiplex (at most 8)."));
        if (ImGui::BeginTabItem(trw("Muxing"), nullptr, tabFlags("muxing"))) { muxingTab(g); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Preview"), nullptr, tabFlags("preview"))) { previewTab(g); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
        if (close >= 0) closeSource(g, close);
    }
    ImGui::End();
    if (gSelectFrames > 0) gSelectFrames--;
    saveSoon(g);
    return r;
}

void shutdown(bool userClosed) {
    Gen& g = G();
    g.lastSave = -10;
    saveSoon(g);
    g.previewOn = false;
    g.ses.stop();                    // the sources, then the mux (a file is closed properly)
    g.ses.stopPreview();
    for (int k = 0; k < kMaxSources; k++) g.ses.stopSourcePreview(k);
    if (userClosed) g.link.bye();    // OnAir clears its "generator open" setting
    g.link.stop();
    for (auto& s : g.src) { delete s->prevImg; s->prevImg = nullptr; delete s->ovImg; s->ovImg = nullptr; }
}

void ensureSources(int n) {
    Gen& g = G();
    while ((int)g.src.size() < std::min(n, kMaxSources)) g.src.push_back(makeSource(g, (int)g.src.size(), VgKv()));
}
void setPreview(bool on) { G().previewOn = on; }
void selectTab(const std::string& name) { gSelectTab = name; gSelectFrames = 3; }

} // namespace vgui
