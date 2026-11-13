// Several receivers at once: see receivers.h and docs/multi-sdr.md.
#include "receivers.h"
#include "rxbar.h"
#include "tune_worker.h"
#include "listen_freq.h"
#include "adsb/adsb_acars.h"
#include "transcribe/transcribe_app.h"
#include "dect2/engine/device_claims.h"
#include <memory>
#ifndef _WIN32
#include <sys/resource.h>
#endif

float gCpuPct = 0;

namespace {
std::unique_ptr<App> gExtra[kMaxReceivers - 1];   // RX 2 ... ; a closed receiver leaves its place empty (the numbers stay)
App* gFirst = nullptr;
int gSel = 1, gAudio = 1;
std::vector<int> gCloseQ;
bool gWarnAsk = false;

struct PrefScope {
    int old;
    explicit PrefScope(int r) : old(plat::gPrefRx) { plat::gPrefRx = r; }
    ~PrefScope() { plat::gPrefRx = old; }
};

int openMask() {
    int m = 0;
    for (int i = 0; i < kMaxReceivers - 1; i++) if (gExtra[i]) m |= 1 << (i + 2);
    return m;
}
void saveOpen() { plat::prefs().setI("rxOpen", openMask()); }

const char* modeName(const App& a) {
    const SubMode* sm = curSub(a);
    for (int i = 0; i < kNumModes; i++) {
        if (kModes[i].family != a.family) continue;
        if (sm ? (kModes[i].subId && !strcmp(kModes[i].subId, sm->id)) : !kModes[i].subId) return kModes[i].name;
    }
    return "";
}

void sampleCpu() {
#ifndef _WIN32
    static double lastCpu = 0, lastWall = 0;
    const double wall = ImGui::GetTime();
    if (wall - lastWall < 1.0) return;
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
    if (lastWall > 0) gCpuPct = (float)((cpu - lastCpu) / (wall - lastWall) * 100);
    lastCpu = cpu; lastWall = wall;
#endif
}

App* create(int id) {
    if (id < 2 || id > kMaxReceivers || gExtra[id - 2]) return nullptr;
    App& first = *gFirst;
    auto up = std::make_unique<App>();
    App& a = *up;
    a.rxId = id;
    a.engine.setReceiverId(id);
    a.ui = first.ui; a.mono = first.mono;
    a.wf.init(gGfx);
    a.tune.synth.demoTv = true;
    a.engine.player().setHardwareDecode(plat::prefs().getB("hwVideo", true));
    {
        PrefScope ps(id);
        loadPrefs(a);
    }
    a.newUi = first.newUi; a.lightUi = first.lightUi; a.highContrast = first.highContrast; a.uiTheme = first.uiTheme; a.uiVariant = first.uiVariant; a.touchUi = first.touchUi; a.touchScale = first.touchScale; a.touchSize = first.touchSize;   // one look for the whole window
    a.volume = first.volume; a.muted = first.rxForcedMute ? first.rxSavedMuted : first.muted;
    a.engine.setResultsDir(plat::dataDir() + "/results_rx" + std::to_string(id));
    a.engine.log("receiver RX " + std::to_string(id));
    refreshDevices(a);
    {   // the radio it had, else the first one nobody has, else the test signal
        int pick = 0;
        PrefScope ps(id);
        const std::string name = plat::prefs().getS("devName", "");
        for (int i = 2; i < (int)a.devices.size() && pick == 0; i++)
            if (!name.empty() && a.devices[i].name == name && !deviceHolder(a.devices[i])) pick = i;
        for (int i = 2; i < (int)a.devices.size() && pick == 0; i++)
            if (a.devices[i].isRadio() && !deviceHolder(a.devices[i])) pick = i;
        a.devIdx = pick;
    }
    gExtra[id - 2] = std::move(up);
    return gExtra[id - 2].get();
}

void drawWarning(App& first) {
    if (gWarnAsk) { ImGui::OpenPopup("Add receiver##rxwarn"); gWarnAsk = false; }
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Add receiver##rxwarn", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    static bool dontShow = false;
    ImGui::PushTextWrapPos(430 * gUi);
    ImGui::TextUnformatted(tr("Running several radios at the same time needs a lot of processing power. Performance may limit what is possible: with too many or too fast receivers samples are dropped and decoding gets worse."));
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("A high-end computer is recommended. Lower sample rates help."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Text(tr("CPU load now: %.0f%% (100%% = one core)"), gCpuPct);
    ImGui::Checkbox(tr("Don't show again"), &dontShow);
    ImGui::Spacing();
    if (ImGui::Button(tr("Add receiver"))) {
        if (dontShow) plat::prefs().setB("rxWarnDone", true);
        if (App* n = addReceiver()) (void)n;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    (void)first;
}

float stripHeight(const App& first) { return ImGui::GetFrameHeight() * (first.touchUi ? 1.35f : 1.f) + (first.touchUi ? 10.f : 6.f) * gUi; }   // 4.0: thinner, flat like the status bar

float gScroll = 0;   // horizontal scroll of the chips when many receivers are open

// the loudspeaker: a box and a cone, with waves when audible and a slash when not
void drawSpeaker(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool on) {
    dl->AddRectFilled(ImVec2(c.x - s * 0.9f, c.y - s * 0.3f), ImVec2(c.x - s * 0.35f, c.y + s * 0.3f), col);
    dl->AddTriangleFilled(ImVec2(c.x - s * 0.35f, c.y - s * 0.3f), ImVec2(c.x + s * 0.25f, c.y - s * 0.75f), ImVec2(c.x + s * 0.25f, c.y + s * 0.75f), col);
    dl->AddTriangleFilled(ImVec2(c.x - s * 0.35f, c.y + s * 0.3f), ImVec2(c.x - s * 0.35f, c.y - s * 0.3f), ImVec2(c.x + s * 0.25f, c.y + s * 0.75f), col);
    if (on) {
        dl->PathArcTo(c, s * 0.8f, -0.7f, 0.7f, 6); dl->PathStroke(col, 0, 1.3f * gUi);
        dl->PathArcTo(c, s * 1.2f, -0.7f, 0.7f, 8); dl->PathStroke(col, 0, 1.3f * gUi);
    } else dl->AddLine(ImVec2(c.x + s * 0.4f, c.y - s * 0.55f), ImVec2(c.x + s * 1.1f, c.y + s * 0.55f), col, 1.5f * gUi);
}

void strip(App& first, ImVec2 disp, float h) {
    ImGuiIO& io = ImGui::GetIO();
    const std::vector<App*> all = receiversAll();
    if (!io.WantTextInput && io.KeyCtrl && !io.KeyAlt && !io.KeySuper)   // Ctrl+1..9 switch receiver
        for (int k = 1; k <= 9; k++)
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + k), false)) { const int ix = rxbar::indexForDigit(k, (int)all.size()); if (ix >= 0) gSel = all[ix]->rxId; }
    ImGui::SetNextWindowPos(ImVec2(0, disp.y - h));
    ImGui::SetNextWindowSize(ImVec2(disp.x, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * gUi, 3 * gUi));
    ImGui::Begin("##rxstrip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    tourMark("receivers", ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + disp.x, ImGui::GetWindowPos().y + h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool compact = disp.x < 720 * gUi;
    const float chipH = h - 6 * gUi;
    const float x0 = ImGui::GetWindowPos().x + 6 * gUi, y0 = ImGui::GetWindowPos().y + 3 * gUi;
    const float btn = chipH, plusW = chipH + 2 * gUi;
    char cpu[24];
    snprintf(cpu, sizeof cpu, "CPU %.0f%%", gCpuPct);
    const float cpuW = 0.f;   // the CPU level is a cell of the status strip now
    const float viewR = ImGui::GetWindowPos().x + disp.x - cpuW - plusW - 10 * gUi;   // chips are clipped here
    const float viewW = std::max(40.f, viewR - x0);
    const float maxChip = (compact ? 190.f : 330.f) * gUi, minChip = 96 * gUi;
    const ImVec4 grey = pal::grey();
    // layout pass: widths
    struct Chip { App* r; std::string txt; float x, w; };
    static std::vector<Chip> chips;   // reused: no per-frame allocation once warm
    chips.clear();
    float x = 0;
    for (App* r : all) {
        const char* mn = modeName(*r);
        std::string t = all.size() == 1 ? "RX " + std::to_string(r->rxId) : rxbar::label(r->rxId, mn, listenMhz(*r));   // one receiver: its mode and frequency are in the status strip
        const float tw = ImGui::CalcTextSize(t.c_str()).x;
        const float w = rxbar::chipWidth(tw, 12 * gUi + 14 * gUi + btn * (r->rxId > 1 ? 2.f : 1.f), minChip, maxChip);
        chips.push_back({r, std::move(t), x, w});
        x += w + 4 * gUi;
    }
    // mouse wheel over the strip scrolls; the selected chip is kept in view
    const bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (over && io.MouseWheel != 0 && x > viewW) gScroll -= io.MouseWheel * 60 * gUi;
    for (const Chip& c : chips) if (c.r->rxId == gSel && ImGui::IsMouseDown(0) == false && x > viewW) {
        static int lastSel = 0;
        if (lastSel != gSel) { gScroll = rxbar::scrollToShow(gScroll, c.x, c.x + c.w, viewW); lastSel = gSel; }
    }
    gScroll = std::clamp(gScroll, 0.f, std::max(0.f, x - 4 * gUi - viewW));
    int closeId = 0;
    dl->PushClipRect(ImVec2(x0, y0 - 2 * gUi), ImVec2(viewR, y0 + chipH + 2 * gUi), true);
    for (const Chip& c : chips) {
        App* r = c.r;
        const bool sel = r->rxId == gSel, run = r->engine.running(), snd = r->rxId == gAudio;
        const ImVec2 a(x0 + c.x - gScroll, y0), b(a.x + c.w, y0 + chipH);
        ImGui::PushID(r->rxId);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("chip", ImVec2(c.w, chipH));
        const bool hov = ImGui::IsItemHovered() && ImGui::IsMouseHoveringRect(ImVec2(std::max(a.x, x0), a.y), ImVec2(std::min(b.x, viewR), b.y));
        const ImVec2 mp = io.MousePos;
        const bool showX = r->rxId > 1 && (hov || sel);
        const ImVec2 xa(b.x - btn, a.y), sa(b.x - btn * (r->rxId > 1 ? 2.f : 1.f), a.y);
        const bool onX = hov && showX && mp.x >= xa.x, onS = hov && !onX && mp.x >= sa.x;
        // flat tabs, as the status bar above them: no frames; the selected one on the frame colour with an accent line on top, hover a faint fill,
        // a thin rule between tabs
        if (sel) {
            dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg));
            dl->AddRectFilled(a, ImVec2(b.x, a.y + 2 * gUi), ImGui::GetColorU32(pal::accent()));
        } else if (hov) dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBgHovered, 0.5f));
        dl->AddLine(ImVec2(b.x + 2 * gUi, a.y + 4 * gUi), ImVec2(b.x + 2 * gUi, b.y - 4 * gUi), ImGui::GetColorU32(ImGuiCol_Separator));
        const ImVec4 dot = run ? pal::okGreen() : ImVec4(0.42f, 0.44f, 0.46f, 1);
        dl->AddCircleFilled(ImVec2(a.x + 10 * gUi, a.y + chipH * 0.5f), 3.f * gUi, ImGui::GetColorU32(dot));
        // text: shorten the mode name first so the frequency stays readable
        const float room = c.w - 12 * gUi - 14 * gUi - btn * (r->rxId > 1 ? 2.f : 1.f);
        std::string t = c.txt;
        if (ImGui::CalcTextSize(t.c_str()).x > room) {
            const std::string fq = rxbar::label(r->rxId, "", listenMhz(*r));
            t = std::string(c.txt);
            int n = (int)t.size();
            while (n > 4 && ImGui::CalcTextSize(rxbar::ellipsize(t, n).c_str()).x > room) n--;
            t = rxbar::ellipsize(t, n);
            (void)fq;
        }
        dl->AddText(ImVec2(a.x + 18 * gUi, a.y + (chipH - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(sel ? ImGuiCol_Text : ImGuiCol_TextDisabled), t.c_str());
        // speaker: lit when this receiver is the audible one
        const ImVec2 sc(sa.x + btn * 0.5f, a.y + chipH * 0.5f);
        if (onS) dl->AddRectFilled(ImVec2(sa.x + 1, a.y + 2 * gUi), ImVec2(sa.x + btn - 1, b.y - 2 * gUi), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f)), 3 * gUi);
        drawSpeaker(dl, ImVec2(sc.x - 1 * gUi, sc.y), 5 * gUi, ImGui::GetColorU32(snd ? pal::accent() : ImVec4(grey.x, grey.y, grey.z, onS ? 0.9f : 0.5f)), snd);
        if (showX) {
            if (onX) dl->AddRectFilled(ImVec2(xa.x + 1, a.y + 2 * gUi), ImVec2(xa.x + btn - 1, b.y - 2 * gUi), ImGui::GetColorU32(pal::badRed()), 3 * gUi);
            const ImVec2 xc(xa.x + btn * 0.5f, a.y + chipH * 0.5f);
            const float d = 3.2f * gUi;
            const ImU32 xcol = ImGui::GetColorU32(onX ? ImVec4(1, 1, 1, 1) : ImVec4(grey.x, grey.y, grey.z, 0.9f));
            dl->AddLine(ImVec2(xc.x - d, xc.y - d), ImVec2(xc.x + d, xc.y + d), xcol, 1.4f * gUi);
            dl->AddLine(ImVec2(xc.x - d, xc.y + d), ImVec2(xc.x + d, xc.y - d), xcol, 1.4f * gUi);
        }
        if (hov && ImGui::IsMouseReleased(0)) { if (onX) closeId = r->rxId; else if (onS) gAudio = r->rxId; else gSel = r->rxId; }
        if (hov && ImGui::IsMouseClicked(2) && r->rxId > 1) closeId = r->rxId;
        if (hov && io.MouseWheel == 0) {
            if (onX) ImGui::SetTooltip("%s", tr("Close this receiver: it stops and lets go of its radio"));
            else if (onS) ImGui::SetTooltip("%s", tr("Only one receiver plays its sound at a time: click to hear this one"));
            else {
                const DeviceInfo* di = r->devIdx >= 0 && r->devIdx < (int)r->devices.size() ? &r->devices[r->devIdx] : nullptr;
                ImGui::BeginTooltip();
                ImGui::Text("RX %d: %s, %.3f MHz", r->rxId, modeName(*r), listenMhz(*r));
                ImGui::TextColored(run ? pal::okGreen() : grey, "%s", run ? tr("Receiving") : tr("Stopped"));
                if (di) ImGui::Text(tr("Radio: %s"), di->name.c_str());
                ImGui::EndTooltip();
            }
        }
        if (hov && ImGui::IsMouseClicked(1)) ImGui::OpenPopup("chipmenu");
        if (ImGui::BeginPopup("chipmenu")) {
            if (ImGui::MenuItem(tr("Hear this receiver"), nullptr, snd)) gAudio = r->rxId;
            if (r->rxId > 1 && ImGui::MenuItem(tr("Close receiver"))) closeId = r->rxId;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    dl->PopClipRect();
    {   // the + button, right after the chips (or at the right edge when they overflow)
        const bool room = (int)all.size() < kMaxReceivers;
        const float px = std::min(x0 + x - gScroll, viewR) + 4 * gUi;
        ImGui::SetCursorScreenPos(ImVec2(px, y0));
        ImGui::BeginDisabled(!room);
        ImGui::InvisibleButton("##add", ImVec2(plusW, chipH));
        const bool hv = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::EndDisabled();
        const ImVec2 a(px, y0), b(px + plusW, y0 + chipH), c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        if (hv && room) dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBgHovered, 0.5f));   // flat: a fill only under the pointer
        const ImU32 pc = ImGui::GetColorU32(room ? ImVec4(0.8f, 0.82f, 0.84f, 1) : ImVec4(0.4f, 0.4f, 0.4f, 1));
        dl->AddLine(ImVec2(c.x - 4.5f * gUi, c.y), ImVec2(c.x + 4.5f * gUi, c.y), pc, 1.6f * gUi);
        dl->AddLine(ImVec2(c.x, c.y - 4.5f * gUi), ImVec2(c.x, c.y + 4.5f * gUi), pc, 1.6f * gUi);
        if (room && hv && ImGui::IsMouseReleased(0)) {
            if (plat::prefs().getB("rxWarnDone", false)) addReceiver(); else gWarnAsk = true;
        }
        if (hv) ImGui::SetTooltip("%s", room ? tr("Add a receiver: another radio with its own mode") : tr("The most receivers are open"));
    }
    if (cpuW > 0) {
        const float cw = ImGui::CalcTextSize(cpu).x;
        dl->AddText(ImVec2(ImGui::GetWindowPos().x + disp.x - cw - 12 * gUi, y0 + (chipH - ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(gCpuPct > 250 * (float)all.size() / 2 ? pal::warnAmber() : ImVec4(0.6f, 0.6f, 0.6f, 1)), cpu);
    }
    ImGui::End();
    if (closeId) closeReceiver(closeId);
    (void)first;
}
}  // namespace

App* receiverHeard() { return receiverById(gAudio); }
void receiverHear(int id) { if (receiverById(id)) gAudio = id; }

App* receiverById(int id) {
    if (id == 1) return gFirst;
    return id >= 2 && id <= kMaxReceivers ? gExtra[id - 2].get() : nullptr;
}

std::vector<App*> receiversAll() {
    std::vector<App*> v;
    if (gFirst) v.push_back(gFirst);
    for (auto& e : gExtra) if (e) v.push_back(e.get());
    return v;
}

App* addReceiver() {
    for (int id = 2; id <= kMaxReceivers; id++) if (!gExtra[id - 2]) {
        App* a = create(id);
        if (a) { gSel = id; saveOpen(); }
        return a;
    }
    return nullptr;
}

void closeReceiver(int id) {
    if (id >= 2) gCloseQ.push_back(id);
}

void receiversInit(App& first) {
    gFirst = &first;
    first.rxId = 1;
    first.engine.setReceiverId(1);
    const int m = (int)plat::prefs().getI("rxOpen", 0);
    for (int id = 2; id <= kMaxReceivers; id++) if (m & (1 << id)) create(id);
}

bool receiverModeBusy(const App& a, std::string* why) {
    for (App* b : receiversAll()) {
        if (b == &a || !b->engine.running() || b->family != a.family || b->sub != a.sub) continue;
        if (a.family == 50 || a.family == 180) continue;   // P25: the follower belongs to one receiver, the other is its voice radio (p25_ui.cpp keeps per-receiver state)
        if (why) *why = "RX " + std::to_string(b->rxId) + " already uses this mode: per-receiver state for this mode is not separated yet";
        return true;
    }
    return false;
}

void receiversFrame(App& first, ImVec2 disp) {
    gFirst = &first;
    for (int id : gCloseQ) {
        App* r = receiverById(id);
        if (!r) continue;
        { PrefScope ps(id); savePrefs(*r); }
        tunework::forget(r->engine);   // its tuning thread ends before the engine goes
        r->engine.stop();
        r->engine.flushResults();
        gExtra[id - 2].reset();
        if (gSel == id) gSel = 1;
        if (gAudio == id) gAudio = gSel;
        saveOpen();
    }
    gCloseQ.clear();
    sampleCpu();
    if (!receiverById(gSel)) gSel = 1;
    if (!receiverById(gAudio)) gAudio = gSel;
    const float h = stripHeight(first);
    const ImVec2 d(disp.x, disp.y - h);
    for (App* r : receiversAll()) {
        PrefScope ps(r->rxId);
        const bool play = r->rxId == gAudio;
        if (!play && !r->rxForcedMute) { r->rxSavedMuted = r->muted; r->muted = true; r->rxForcedMute = true; }
        else if (play && r->rxForcedMute) { r->muted = r->rxSavedMuted; r->rxForcedMute = false; }
        drawUI(*r, d, r->rxId == gSel);
        aeroToolsTick(*r);
    }
    strip(first, disp, h);
    drawWarning(first);
    acarslog::tick(first);
    tx::tick(first);
    mapSendTick(first);
}

void receiversShutdown(App& first) {
    tx::shutdown();
    mapSendShutdown();
    tunework::shutdown();   // every tuning thread ends (a retune still pending is dropped: the radio stops anyway)
    for (int id = 2; id <= kMaxReceivers; id++) if (App* r = gExtra[id - 2].get()) {
        { PrefScope ps(id); savePrefs(*r); }
        r->engine.stop();
        r->engine.flushResults();
    }
    for (auto& e : gExtra) e.reset();
    (void)first;
}
