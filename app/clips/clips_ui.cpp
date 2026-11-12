// Radio clips dialog: choose a range on the history bar (or a preset), choose what goes in, save a folder + zip, copy the Discord text.
#include "dect2/common/thread_name.h"
#include "clips_ui.h"
#include "../ui/typography.h"
#include "clips.h"
#include "../app.h"
#include "../ui/icons.h"
#include "dect2/audio/clip_ring.h"
#include "dect2/engine/updater.h"
#include "dect2/source/iq_record.h"
#include <chrono>
#include <future>
#include "../ui/input_map.h"

using namespace clips;

namespace {
double wallNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

struct Cfg {
    bool wf = true, audio = true, rows = true;
    int wfMin = 5, wfMB = 8, audioSec = 60;
    bool loaded = false;
    bool appliedWf = false;
    int appliedAudio = -1;
} C;

void loadCfg() {
    if (C.loaded) return;
    plat::Prefs& p = plat::prefs();
    C.wf = p.getB("clipWf", true); C.audio = p.getB("clipAudio", true); C.rows = p.getB("clipRows", true);
    C.wfMin = (int)std::min(10L, std::max(1L, p.getI("clipWfMin", 5)));
    C.wfMB = (int)std::min(64L, std::max(2L, p.getI("clipWfMB", 8)));
    C.audioSec = (int)std::min(120L, std::max(30L, p.getI("clipAudioSec", 60)));
    C.loaded = true;
}
void saveCfg() {
    plat::Prefs& p = plat::prefs();
    p.setB("clipWf", C.wf); p.setB("clipAudio", C.audio); p.setB("clipRows", C.rows);
    p.setI("clipWfMin", C.wfMin); p.setI("clipWfMB", C.wfMB); p.setI("clipAudioSec", C.audioSec);
}
void applyCfg() {
    wfHistory().configure(C.wf, C.wfMin * 60.0, (size_t)C.wfMB << 20);
    tableLog().configure(C.rows, C.wfMin * 60.0 * 2, 8u << 20);
}

struct Dlg {
    bool open = false;
    Range sel{0, 0};
    double selFor = 0;               // `now` when the selection was last set by a preset (the presets follow the time while the dialog is open)
    int preset = 1;                  // 0..3 presets, -1 custom
    bool incImage = true, incAudio = true, incMsgs = true;
    char title[96] = "", notes[300] = "";
    std::string status;
    std::future<Written> job;
    bool busy = false;
    std::string lastDir;
    std::vector<float> act;          // history bar: activity per slot
    double actAt = 0, actFrom = 0, actTo = 0;
    bool dragging = false;
} D;

constexpr double kPre[4] = {15, 30, 60, 300};
const char* const kPreName[4] = {"15 s", "30 s", "1 min", "5 min"};

double earliestTime(double now) {
    double e = 0;
    const double w = wfHistory().oldest(), t = tableLog().oldest();
    if (w > 0) e = w;
    const double aa = dect2::AudioClipRing::instance().availableSeconds();
    if (aa > 0 && (e == 0 || now - aa < e)) e = now - aa;
    if (t > 0 && (e == 0 || t < e)) e = t;
    return e > 0 ? std::min(e, now - 5) : now - 60;
}

void rebuildActivity(double now, double earliest) {
    D.act.assign(240, 0.f);
    D.actAt = now; D.actFrom = earliest; D.actTo = now;
    std::vector<uint8_t> rows; std::vector<double> times;
    wfHistory().extract({earliest, now + 1}, rows, times);
    std::vector<int> cnt(240, 0);
    for (size_t i = 0; i < times.size(); i++) {
        const int s = std::min(239, (int)(fractionOf(earliest, now, times[i]) * 240));
        long sum = 0;
        const uint8_t* r = &rows[i * WfHistory::W];
        for (int x = 0; x < WfHistory::W; x += 4) sum += r[x];
        D.act[(size_t)s] += (float)sum / (WfHistory::W / 4); cnt[(size_t)s]++;
    }
    float lo = 1e9f, hi = -1e9f;
    for (int s = 0; s < 240; s++) if (cnt[(size_t)s]) { D.act[(size_t)s] /= (float)cnt[(size_t)s]; lo = std::min(lo, D.act[(size_t)s]); hi = std::max(hi, D.act[(size_t)s]); }
    for (int s = 0; s < 240; s++) D.act[(size_t)s] = cnt[(size_t)s] && hi > lo + 1 ? (D.act[(size_t)s] - lo) / (hi - lo) : cnt[(size_t)s] ? 0.3f : -1.f;
}

Content collect(App& a, Range r, double now) {
    Content c;
    c.wantImage = D.incImage; c.wantAudio = D.incAudio; c.wantMessages = D.incMsgs;
    Meta& m = c.meta;
    const int sm = a.engine.standardMode();
    std::string l1, l2;
    if (const ModeUi* mu = modeUi(a.family)) if (mu->summary) mu->summary(a, l1, l2);
    std::string id = dect2::recordingModeId(sm);
    for (auto& ch : id) ch = (char)toupper((unsigned char)ch);
    m.mode = !l1.empty() ? l1 : id;
    if (m.mode.size() > 40) m.mode.resize(40);
    m.summary = l2.size() > 80 ? l2.substr(0, 80) : l2;
    m.freqHz = a.freqMhz * 1e6; m.t0 = r.t0; m.t1 = r.t1;
    if (!a.hist.empty() && std::isfinite(a.hist.back().snr) && glfwGetTime() - a.hist.back().t < 5) m.snrDb = a.hist.back().snr;
    m.title = D.title; m.notes = D.notes; m.version = ONAIR_VERSION;
    if (D.incImage) {
        std::vector<uint8_t> rows; std::vector<double> times;
        const size_t n = wfHistory().extract(r, rows, times);
        if (n) c.spectro = renderSpectrogram(rows, n, a.wf.lut, a.wf.minDb, a.wf.maxDb, 1024, 600);
    }
    if (D.incAudio) dect2::AudioClipRing::instance().snapshot(now - r.t0, now - r.t1, c.audio, c.audioRate);
    if (D.incMsgs) c.table = tableLog().range(r);
    return c;
}

void memLine(bool disabledNote) {
    char b[96];
    const double mb = (wfHistory().bytes() + dect2::AudioClipRing::instance().bytes() + tableLog().bytes()) / 1e6;
    snprintf(b, sizeof b, tr("Uses about %.1f MB now"), mb);
    if (disabledNote) ImGui::TextDisabled("%s", b); else ImGui::TextUnformatted(b);
}

void draw(App& a) {
    ImGui::SetNextWindowSize(ImVec2(560 * gUi, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(trw("Radio clip"), &D.open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) { ImGui::End(); return; }
    const double now = wallNow();
    const double earliest = earliestTime(now);
    if (now - D.actAt > 2 || D.act.empty() || std::fabs(earliest - D.actFrom) > 1) rebuildActivity(now, earliest);
    if (D.sel.t1 == 0) { D.sel = lastSeconds(now, 30, earliest); D.selFor = now; }
    if (D.preset >= 0) D.sel = lastSeconds(now, kPre[D.preset], earliest);   // a preset follows the clock
    else D.sel = clampRange(D.sel, earliest, now);

    ImGui::TextUnformatted(tr("Last:"));
    for (int i = 0; i < 4; i++) {
        ImGui::SameLine();
        if (ImGui::RadioButton(kPreName[i], D.preset == i)) D.preset = i;
    }
    // the history bar: activity over time, drag to choose the range
    const float w = ImGui::GetContentRegionAvail().x, h = 44 * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##clipbar", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(20, 26, 34, 255), 4 * gUi);
    for (int s = 0; s < 240; s++) {
        if (D.act[(size_t)s] < 0) continue;
        const float x0 = p0.x + w * (float)s / 240, x1 = p0.x + w * (float)(s + 1) / 240, v = D.act[(size_t)s];
        dl->AddRectFilled(ImVec2(x0, p0.y + h - 3 - (h - 8) * v), ImVec2(x1, p0.y + h - 3), IM_COL32(70 + (int)(150 * v), 130 + (int)(90 * v), 220, 255));
    }
    const float xa = p0.x + w * (float)fractionOf(earliest, now, D.sel.t0), xb = p0.x + w * (float)fractionOf(earliest, now, D.sel.t1);
    dl->AddRectFilled(ImVec2(xa, p0.y), ImVec2(xb, p0.y + h), IM_COL32(255, 200, 60, 60));
    dl->AddRect(ImVec2(xa, p0.y), ImVec2(xb, p0.y + h), IM_COL32(255, 200, 60, 230), 2 * gUi, 0, 1.5f);
    if (ImGui::IsItemActivated()) { D.dragging = true; D.selFor = (ImGui::GetIO().MousePos.x - p0.x) / w; D.sel.t0 = D.sel.t1 = earliest + D.selFor * (now - earliest); }
    if (D.dragging && ImGui::IsItemActive()) {
        const double f = std::min(1.0, std::max(0.0, (double)((ImGui::GetIO().MousePos.x - p0.x) / w)));
        D.sel = fromFraction(earliest, now, D.selFor, f);
        D.preset = -1;
    }
    if (D.dragging && !ImGui::IsItemActive()) { D.dragging = false; D.sel = clampRange(D.sel, earliest, now); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What the receiver saw, oldest on the left. Brighter is a stronger signal. Drag to choose the part to save."));
    char b[160];
    snprintf(b, sizeof b, "%s  to  %s  (%.0f s)", utcText(D.sel.t0, true).c_str(), utcText(D.sel.t1, true).c_str(), D.sel.len());
    ImGui::TextUnformatted(b);
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Include:"));
    ImGui::SameLine(); ImGui::Checkbox(tr("Spectrogram picture"), &D.incImage);
    ImGui::SameLine(); ImGui::Checkbox(tr("Audio"), &D.incAudio);
    ImGui::SameLine(); ImGui::Checkbox(tr("Decoded messages"), &D.incMsgs);
    ImGui::SetNextItemWidth(w);
    ImGui::InputTextWithHint("##cliptitle", tr("Title"), D.title, sizeof D.title);
    ImGui::SetNextItemWidth(w);
    ImGui::InputTextWithHint("##clipnotes", tr("Notes"), D.notes, sizeof D.notes);
    ImGui::TextDisabled("%s", tr("Your station position is never included."));
    ImGui::Separator();
    if (D.busy && D.job.valid() && D.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const Written wr = D.job.get();
        D.busy = false;
        if (!wr.error.empty()) D.status = std::string(tr("Could not save:")) + " " + wr.error;
        else { D.lastDir = wr.dir; snprintf(b, sizeof b, tr("Saved: %s (zip %.1f MB)"), wr.dir.c_str(), wr.zipBytes / 1e6); D.status = b; a.engine.log("clip saved to " + wr.dir); }
    }
    ImGui::BeginDisabled(D.busy);
    if (ImGui::Button(tr("Save clip"))) {
        const Content c = collect(a, D.sel, now);
        if (c.spectro.w == 0 && c.audio.empty() && c.table.rows.empty()) D.status = tr("Nothing to save: no waterfall, audio or decoded rows in this range");
        else { const std::string dir = plat::dataDir() + "/clips"; D.busy = true; D.status = tr("Saving..."); D.job = std::async(std::launch::async, [c, dir] { dect2::setThreadName("onair-clip"); return writeClip(c, dir); }); }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Copy Discord post text"))) {
        const Content c = collect(a, D.sel, now);
        ImGui::SetClipboardText(discordText(c).c_str());
        D.status = tr("Copied: paste it into Discord and attach the files");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Open folder"))) dect2::openUrl(D.lastDir.empty() ? plat::dataDir() + "/clips" : D.lastDir);
    if (!D.status.empty()) ImGui::TextWrapped("%s", D.status.c_str());
    if (typo::collapsingHeader(tr("Memory kept for clips"))) {
        bool ch = false;
        ch |= ImGui::Checkbox(tr("Waterfall history"), &C.wf);
        if (C.wf) { ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::SliderInt("##cwm", &C.wfMin, 1, 10, tr("%d min")); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::SliderInt("##cwb", &C.wfMB, 2, 64, tr("%d MB max")); }
        ch |= ImGui::Checkbox(tr("Audio history"), &C.audio);
        if (C.audio) { ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::SliderInt("##cas", &C.audioSec, 30, 120, tr("%d s")); }
        ch |= ImGui::Checkbox(tr("Decoded rows history"), &C.rows);
        if (ch) { saveCfg(); applyCfg(); C.appliedAudio = -1; }
        memLine(false);
        ImGui::TextDisabled("%s", tr("Decoded rows are kept from the results tables you have open."));
    }
    ImGui::End();
}
}  // namespace

namespace clipsui {

void feed(const std::vector<float>& dbfs) {
    if (wfHistory().enabled()) wfHistory().feed(dbfs.data(), dbfs.size(), wallNow());
}

void open() { D.open = true; D.preset = 1; D.sel = {0, 0}; D.status.clear(); D.act.clear(); }

void tick(App& a) {
    loadCfg();
    if (!C.appliedWf) { applyCfg(); C.appliedWf = true; }
    dect2::AudioClipRing& r = dect2::AudioClipRing::instance();
    const int want = C.audio ? C.audioSec : 0;
    if (C.appliedAudio != want && (want == 0 || r.audioSeen())) { r.configure(want); C.appliedAudio = want; }   // allocated only when sound was heard
    ImGuiIO& io = ImGui::GetIO();
    if (inputmap::pressed(inputmap::Action::ClipsWin)) { if (D.open) D.open = false; else open(); }   // C by default
    (void)io;
    if (D.open) draw(a);
}

void button(App& a) {
    (void)a;
    if (ImGui::SmallButton(tr("Clip"))) open();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Radio clip: save the last moments (spectrogram, audio, decoded messages) as a file to share. Key: C"));
}

}  // namespace clipsui
