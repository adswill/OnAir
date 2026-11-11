// Pagers: capcodes that are paged together are one group (a station's pagers). docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/pager/pager_watch.h"
#include <cstdio>

namespace {
dect2::PagerGroups groups;
int64_t lastWall = 0;
uint64_t lastTotal = 0;
}  // namespace

void d40PagerFeed(const PagerTelemetry& t) {
    if (!t.messages || t.messages->empty()) return;
    if (t.messagesTotal < lastTotal) { groups = dect2::PagerGroups(); lastWall = 0; }   // restarted
    lastTotal = t.messagesTotal;
    const auto& v = *t.messages;
    int64_t newest = lastWall;
    for (auto it = v.rbegin(); it != v.rend(); ++it) {   // oldest first
        if (it->wallTime <= lastWall) continue;
        groups.add(it->address, (double)it->wallTime);
        newest = std::max(newest, it->wallTime);
    }
    lastWall = newest;
}

void d40PagerGroupsCard() {
    if (!typo::collapsingHeader(trw("Capcode groups"), 0)) return;
    const auto g = groups.groups();
    if (g.empty()) { ImGui::TextDisabled("%s", tr("None yet: capcodes paged together at least 3 times within 20 seconds form a group.")); return; }
    for (size_t i = 0; i < g.size() && i < 10; i++) {
        std::string s;
        for (size_t k = 0; k < g[i].capcodes.size() && k < 12; k++) { char b[16]; snprintf(b, sizeof b, "%s%u", k ? ", " : "", g[i].capcodes[k]); s += b; }
        if (g[i].capcodes.size() > 12) s += ", ...";
        ImGui::PushTextWrapPos(0);
        ImGui::Text("%zu %s, %d %s: %s", g[i].capcodes.size(), tr("capcodes"), g[i].incidents, tr("incidents"), s.c_str());
        ImGui::PopTextWrapPos();
    }
}
