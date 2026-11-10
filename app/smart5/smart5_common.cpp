// Shared helpers of the round-5 smart cards (app/smart5/smart5_ui.h; docs/SMART5-UI-3.9.md).
#include "smart5_ui.h"
#include "../ui/typography.h"
#include "dect2/insight/live.h"
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

namespace s5ui {

double wall() { return (double)std::time(nullptr); }

bool every(double& last, double sec) {
    const double now = ImGui::GetTime();
    if (last > 0 && now - last < sec) return false;
    last = now;
    return true;
}

std::string utc(double t, bool date) {
    const time_t tt = (time_t)t;
    tm g = *gmtime(&tt);
    char b[32];
    strftime(b, sizeof b, date ? "%Y-%m-%d %H:%M" : "%H:%M", &g);
    return b;
}

void post(const char* area, const char* kind, int severity, const std::string& key, const std::string& text) {
    // the same finding about the same thing at most once in 10 minutes (a flapping input would flood the list)
    static std::map<std::string, double> last;
    const double now = wall();
    double& l = last[std::string(kind) + "|" + key];
    if (l > 0 && now - l < 600) return;
    l = now;
    dect2::insight::Insight i;
    i.time = now; i.area = area; i.kind = kind; i.severity = severity; i.key = key; i.text = text;
    dect2::insight::live().post(i);
}

void edge(bool& state, bool on, const char* area, const char* kind, int severity, const std::string& key, const std::string& text) {
    if (on && !state) post(area, kind, severity, key, text);
    state = on;
}

void line(int severity, const std::string& text) {
    if (severity >= 2) ImGui::TextColored(pal::badRed(), "%s", text.c_str());
    else if (severity == 1) ImGui::TextColored(pal::warnAmber(), "%s", text.c_str());
    else ImGui::TextUnformatted(text.c_str());
}

bool card(const char* id) {
    ImGui::PushID(id);
    const bool open = typo::collapsingHeader(trw("Smart checks"), ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopID();
    if (open && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Automatic: these run on what the receiver already measures. Findings also go to View > Insights."));
    return open;
}

bool details(const char* id) {
    ImGui::PushID(id);
    const bool open = ImGui::TreeNodeEx(tr("Details"), ImGuiTreeNodeFlags_None);
    ImGui::PopID();
    return open;
}

std::string readData(const char* file) {
    std::ifstream f(plat::dataDir() + "/" + file);
    if (!f) return {};
    std::stringstream b;
    b << f.rdbuf();
    return b.str();
}

void writeData(const char* file, const std::string& text) {
    std::ofstream f(plat::dataDir() + "/" + file, std::ios::trunc);
    if (f) f << text;
}

}  // namespace s5ui

void smart5Tick(App& a) {
    smart5BroadcastTick(a);
    smart5TvTick(a);
    smart5SatTick(a);
    smart5SciTick(a);
}
