// Spectrum ghost, the App side (state/spec_ghost.h): the chosen earlier snapshot drawn faintly behind the live trace, the signals that are new
// since then in an orange band with a "+N dB" tag, the controls in the spectrum menu, and the rolling auto-snapshot. The averaged trace is what
// is stored and compared. Nothing here touches the receiver or the other plots.
#include "../app.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../state/spec_ghost.h"
#include <cmath>
#include <ctime>

namespace {

struct G {
    std::string dir;
    std::vector<sghost::Meta> metas;
    bool metasValid = false;
    sghost::Snap cur;                 // the snapshot in use
    int64_t curWhen = -1;             // its time (-1: none loaded)
    std::vector<float> gc, lc;        // columns of the ghost and the live trace over the view
    std::vector<sghost::Span> news;
    double lastCalc = -1;
    int64_t lastAuto = 0;
    std::string note;
};
G g;

std::string dirNow() { return plat::dataDir() + "/ghost"; }
int64_t nowSec() { return (int64_t)std::time(nullptr); }

void rescan() { g.dir = dirNow(); g.metas = sghost::list(g.dir); g.metasValid = true; if (!g.metas.empty() && g.lastAuto == 0) g.lastAuto = g.metas[0].when; }

std::string whenText(int64_t t) {
    const std::time_t tt = (std::time_t)t; std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char b[40]; strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm); return b;
}

// the snapshot the settings ask for, for this view; the index into g.metas or -1
int chosen(const App& a, double lo, double hi) {
    if (!g.metasValid) return -1;
    const int64_t now = nowSec();
    switch (a.ghostWhich) {
        case 0: return sghost::pick(g.metas, lo, hi, now - 86400);
        case 1: return sghost::pick(g.metas, lo, hi, now - 7 * 86400);
        case 2: return sghost::pick(g.metas, lo, hi, now);
        default: return sghost::pick(g.metas, lo, hi, a.ghostPick);
    }
}

bool takeSnapshot(App& a, const std::string& label) {
    if (a.smooth.empty()) return false;
    const double fsMhz = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6, rc = radioCenterMhz(a);
    if (fsMhz <= 0) return false;
    const int64_t now = nowSec();
    if (!sghost::save(dirNow(), sghost::make(a.smooth.data(), a.smooth.size(), rc - fsMhz / 2, rc + fsMhz / 2, now, label))) { g.note = "could not write the snapshot folder"; return false; }
    g.lastAuto = now; g.curWhen = -1;
    sghost::prune(dirNow(), a.ghostKeep);
    rescan();
    return true;
}

}  // namespace

void ghostTick(App& a) {
    if (a.ghostAutoH <= 0 || !a.engine.running() || a.smooth.empty()) return;
    static double lastCheck = 0;
    const double t = (double)std::time(nullptr);
    if (t - lastCheck < 30) return;   // once in half a minute is plenty for an hours-long interval
    lastCheck = t;
    if (!g.metasValid) rescan();
    if (a.wf.paused) return;
    if (sghost::autoDue(g.lastAuto, nowSec(), (int64_t)a.ghostAutoH * 3600)) takeSnapshot(a, "auto");
}

void ghostDraw(App& a, const std::vector<float>& live, double loMhz, double hiMhz, double vLo, double vHi, int npx) {
    if (!a.specGhost || live.empty() || npx < 2) return;
    if (!g.metasValid) rescan();
    const int idx = chosen(a, loMhz, hiMhz);
    if (idx < 0) return;
    if (g.curWhen != g.metas[(size_t)idx].when) {
        if (!sghost::load(g.metas[(size_t)idx].path, g.cur)) return;
        g.curWhen = g.cur.when; g.lastCalc = -1;
    }
    sghost::columns(g.cur.db.data(), g.cur.db.size(), g.cur.loMhz, g.cur.hiMhz, vLo, vHi, npx, g.gc);
    static std::vector<double> xs, ys;
    xs.clear(); ys.clear();
    const double colW = (vHi - vLo) / npx;
    for (int c = 0; c < npx; c++) if (std::isfinite(g.gc[(size_t)c])) { xs.push_back(vLo + (c + 0.5) * colW); ys.push_back(g.gc[(size_t)c]); }
    if (xs.size() >= 2) {
        plt::Spec ps; ps.LineColor = ImVec4(0.75f, 0.55f, 1.0f, 0.55f); ps.LineWeight = 1.2f;
        plt::PlotLine("##ghostline", xs.data(), ys.data(), (int)xs.size(), ps);
    }
    if (!a.ghostNew) return;
    const double t = ImGui::GetTime();
    if (g.lastCalc < 0 || t - g.lastCalc > 0.4) {   // a couple of times a second is enough for a comparison of two averaged lines
        g.lastCalc = t;
        sghost::columns(live.data(), live.size(), loMhz, hiMhz, vLo, vHi, npx, g.lc);
        g.news = sghost::newSignals(g.lc, g.gc, vLo, vHi, a.ghostThr, 2);
    }
    for (size_t i = 0; i < g.news.size(); i++) {
        plt::Spec bs; bs.FillColor = ImVec4(1.0f, 0.55f, 0.1f, 0.22f);
        char id[32]; snprintf(id, sizeof id, "##ghostnew%zu", i);
        plt::PlotVBand(id, g.news[i].loMhz, g.news[i].hiMhz, bs);
    }
}

void ghostMenu(App& a, bool& save) {
    if (ImGui::Checkbox(tr("Ghost: compare with an earlier spectrum"), &a.specGhost)) { save = true; rescan(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Draws a stored earlier spectrum faintly behind the live line and shades the signals that are new since then"));
    if (!a.specGhost) return;
    if (ImGui::SmallButton(tr("Save snapshot now"))) { if (takeSnapshot(a, "")) g.note = tr("snapshot saved"); else if (g.note.empty()) g.note = tr("nothing to save: start the receiver first"); }
    if (!g.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", g.note.c_str()); }
    if (ImGui::CollapsingHeader(trw("Advanced##ghostadv"))) {
        static const char* names[4] = {"Yesterday", "Last week", "Latest snapshot", "A saved snapshot"};
        ImGui::SetNextItemWidth(180 * gUi);
        if (ImGui::BeginCombo(tr("Compare with"), tr(names[std::max(0, std::min(3, a.ghostWhich))]))) {
            for (int i = 0; i < 4; i++) if (ImGui::Selectable(tr(names[i]), a.ghostWhich == i)) { a.ghostWhich = i; g.curWhen = -1; save = true; }
            ImGui::EndCombo();
        }
        if (a.ghostWhich == 3) {
            ImGui::SetNextItemWidth(260 * gUi);
            const std::string cur = a.ghostPick > 0 ? whenText(a.ghostPick) : std::string(tr("(choose)"));
            if (ImGui::BeginCombo(tr("Snapshot"), cur.c_str())) {
                for (const auto& m : g.metas) {
                    const std::string t = whenText(m.when) + (m.label.empty() ? "" : "  " + m.label) + "  (" + std::to_string((int)m.loMhz) + "-" + std::to_string((int)m.hiMhz) + " MHz)";
                    if (ImGui::Selectable(t.c_str(), m.when == a.ghostPick)) { a.ghostPick = m.when; g.curWhen = -1; save = true; }
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::Checkbox(tr("Highlight new signals"), &a.ghostNew)) save = true;
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::SliderFloat(tr("New above (dB)"), &a.ghostThr, 2.f, 30.f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) save = true;
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::SliderInt(tr("Auto snapshot every (h, 0 = off)"), &a.ghostAutoH, 0, 72)) save = true;
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::SliderInt(tr("Keep the newest"), &a.ghostKeep, 1, 200)) save = true;
        if (ImGui::SmallButton(tr("Refresh list"))) rescan();
        ImGui::SameLine();
        ImGui::TextDisabled(tr("%d snapshots"), (int)g.metas.size());
    }
}
