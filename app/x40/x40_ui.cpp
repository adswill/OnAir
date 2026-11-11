// The screens of the 4.0 signal modes (families 881 - 885, docs/X40-4.0.md): one table of what was decoded, one Receiver page. One click: the mode tunes and needs no settings.
#include "app.h"
#include "dect2/x40/x40_rx.h"
#include "dect2/x40/x40_tel.h"
#include <cstdio>
#include <string>

namespace {

using dect2::X40Row;
using dect2::X40Telemetry;

template <int V> bool live(const App& a) { return a.rx.standard == 882 + V; }

template <int V> void tab(App& a) {
    if (!live<V>(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "stopped")); return; }
    const X40Telemetry& t = a.rx.x40;
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(t.headline.c_str()); ImGui::PopFont();
    if (t.rows.empty()) { ImGui::TextDisabled("%s", tr("nothing decoded yet")); return; }
    if (ImGui::BeginTable("##x40", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Kind")); ImGui::TableSetupColumn(tr("ID")); ImGui::TableSetupColumn(tr("Decoded"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const X40Row& r : t.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.timeSec);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(r.kind.c_str()));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.id.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.text.c_str());
        }
        ImGui::EndTable();
    }
}

template <int V> void receiver(App& a) {
    if (!live<V>(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "stopped")); return; }
    const X40Telemetry& t = a.rx.x40;
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", tr(k)); kvColumn(150 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    char b[160];
    kv(N_("state"), t.headline);
    snprintf(b, sizeof b, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); kv(N_("frames"), b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); kv(N_("signal"), b);
    snprintf(b, sizeof b, "%.0f Hz", t.cfoHz); kv(N_("offset"), b);
    snprintf(b, sizeof b, "%.3f Msps", t.inputRate / 1e6); kv(N_("input"), b);
    for (const auto& f : t.facts) kv(f.first.c_str(), f.second);
}

template <int V> void list(App& a) {
    if (!live<V>(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "stopped")); return; }
    const X40Telemetry& t = a.rx.x40;
    int shown = 0;
    for (const X40Row& r : t.rows) { ImGui::Text("%s  %s", r.kind.c_str(), r.id.c_str()); if (++shown >= 40) break; }
    if (!shown) ImGui::TextDisabled("%s", tr("nothing yet"));
}

template <int V> void decoder(App& a, bool&) {
    if (ImGui::SmallButton(trw("Clear"))) a.engine.x40().clearResults(V);
}

template <int V> void status(App& a) {
    const bool run = a.engine.running(), on = live<V>(a);
    lamp("Decode", !on ? 0 : a.rx.x40.state >= 2 ? 1 : 0);
    flowNext(10 * gUi);
    ImGui::TextUnformatted(!on ? tr(run ? "starting" : "stopped") : a.rx.x40.headline.c_str());
}

template <int V> void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = dect2::x40::variantName(V);
    if (live<V>(a)) l2 = a.rx.x40.headline;
}

template <int V> constexpr ModeUi makeUi() {
    ModeUi u{};
    u.sideTitle = "HEARD"; u.tabName = "Decoded"; u.tabIcon = Ic::Signal;
    u.tab = tab<V>; u.receiver = receiver<V>; u.list = list<V>; u.status = status<V>; u.summary = summary<V>; u.decoder = decoder<V>;
    return u;
}

} // namespace

extern const ModeUi kX40Ui[10];
const ModeUi kX40Ui[10] = {makeUi<0>(), makeUi<1>(), makeUi<2>(), makeUi<3>(), makeUi<4>(), makeUi<5>(), makeUi<6>(), makeUi<7>(), makeUi<8>(), makeUi<9>()};
