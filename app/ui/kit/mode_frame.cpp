#include "mode_frame.h"
#include "app.h"
#include "dect2/session/record_rules.h"
#include "dect2/session/session_store.h"
#include "dect2/session/track_book.h"
#include "i18n/i18n.h"
#include "ui/scale.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace kit {
namespace {
using namespace dect2::sess;

struct Counts { double at = -1e9; int active = 0, today = 0; int64_t last = 0; };
std::map<uint16_t, Counts>& cache() { static auto* m = new std::map<uint16_t, Counts>; return *m; }

const Counts& countsFor(uint16_t mode) {
    Counts& c = cache()[mode];
    const double t = ImGui::GetTime();
    if (t - c.at < 0.5) return c;
    c.at = t;
    c.active = c.today = 0;
    c.last = 0;
    const int64_t now = wallNowMs();
    time_t tt = (time_t)(now / 1000);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    const int64_t midnight = (int64_t)mktime(&lt) * 1000;
    for (const Session& s : global().window(mode, -1, 200)) {
        if (s.active()) c.active++;
        if (s.startWall >= midnight) c.today++;
        c.last = std::max(c.last, s.active() ? now : s.endWall);
    }
    return c;
}

std::string ago(int64_t ms) {
    if (ms <= 0) return "-";
    const int64_t s = std::max<int64_t>(0, (wallNowMs() - ms) / 1000);
    char b[32];
    if (s < 5) return tr("now");
    if (s < 90) snprintf(b, sizeof b, "%llds", (long long)s);
    else if (s < 5400) snprintf(b, sizeof b, "%lldmin", (long long)(s / 60));
    else if (s < 129600) snprintf(b, sizeof b, "%lldh", (long long)(s / 3600));
    else snprintf(b, sizeof b, "%lldd", (long long)(s / 86400));
    return std::string(b) + " " + tr("ago");
}
}  // namespace

int modeNav(const char* id, int& cur, std::initializer_list<NavItem> items) {
    {   // test aid (hidden GUI checks), same as subNav: DECT2_SUBNAV="dmrv=3"
        static std::map<std::string, bool> done;
        const char* e = getenv("DECT2_SUBNAV");
        const std::string key = id;
        if (e && !done[key]) { done[key] = true; const std::string v = e; const size_t q = v.find('='); if (q != std::string::npos && v.substr(0, q) == key) cur = atoi(v.c_str() + q + 1); }
    }
    ImGui::PushID(id);
    bool first = true, anyMore = false;
    const NavItem* curMore = nullptr;
    for (const NavItem& n : items) {
        if (n.more) { anyMore = true; if (n.view == cur) curMore = &n; continue; }
        if (!first) sameLineIf(ImGui::CalcTextSize(n.label).x + 22 * gUi, 6 * gUi);
        first = false;
        if (pillButton(n.label, cur == n.view)) cur = n.view;
    }
    if (anyMore) {
        std::string lab = std::string(tr("More")) + (curMore ? std::string(": ") + curMore->label : std::string(" v"));
        if (!first) sameLineIf(ImGui::CalcTextSize(lab.c_str()).x + 22 * gUi, 6 * gUi);
        if (pillButton(lab.c_str(), curMore != nullptr)) ImGui::OpenPopup("##more");
        if (ImGui::BeginPopup("##more")) {
            for (const NavItem& n : items) if (n.more && ImGui::MenuItem(n.label, nullptr, cur == n.view)) cur = n.view;
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
    ImGui::Spacing();
    return cur;
}

void modeHeader(const HeaderInfo& h) {
    const float u = gUi;
    const Counts& c = countsFor(h.mode);
    RecordRules& rr = rules();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec4 col = h.lock == 1 ? ImVec4(0.35f, 0.8f, 0.45f, 1) : h.lock == 2 ? ImVec4(0.9f, 0.7f, 0.2f, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1);
    // fields in priority order: [text, tooltip-only when it does not fit]
    std::vector<std::string> f;
    f.push_back(h.name);
    f.push_back(rr.enabled() ? std::string(tr("Record: ON")) : std::string(tr("Record: off")));
    f.push_back(std::string(tr("heard")) + " " + ago(c.last));
    { char b[64]; snprintf(b, sizeof b, "%d %s, %d %s", c.active, tr("active"), c.today, tr("today")); f.push_back(b); }
    if (h.snrDb > -900.f) { char b[32]; snprintf(b, sizeof b, "SNR %.0f dB", h.snrDb); f.push_back(b); }
    if (h.freqHz > 0) { char b[32]; snprintf(b, sizeof b, "%.4f MHz", h.freqHz / 1e6); f.push_back(b); }
    // below 360 px only the first three remain; otherwise drop from the end until the line fits
    size_t keep = avail < 360 * u ? 3 : f.size();
    auto width = [&](size_t n) { float w = 16 * u; for (size_t i = 0; i < n; i++) w += ImGui::CalcTextSize(f[i].c_str()).x + 14 * u; return w; };
    while (keep > 1 && width(keep) > avail) keep--;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 5 * u;
    dl->AddCircleFilled(ImVec2(p.x + r + 1, p.y + ImGui::GetTextLineHeight() * 0.5f), r, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(2 * r + 6 * u, ImGui::GetTextLineHeight()));
    for (size_t i = 0; i < keep; i++) {
        ImGui::SameLine(0, i ? 14 * u : 0);
        if (i == 0) ImGui::TextUnformatted(f[i].c_str()); else ImGui::TextDisabled("%s", f[i].c_str());
    }
    if (ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(p, ImVec2(p.x + avail, p.y + ImGui::GetTextLineHeight()))) {
        std::string tip = h.lock == 1 ? tr("Locked on a signal") : h.lock == 2 ? tr("Searching") : tr("Receiver off");
        for (size_t i = keep; i < f.size(); i++) tip += "\n" + f[i];
        ImGui::SetTooltip("%s", tip.c_str());
    }
}

}  // namespace kit
