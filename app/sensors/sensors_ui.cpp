// Sub-GHz sensors screens: the live list of sensors with a history graph, the message log, the unknown signal analyser with a flex decoder editor,
// and the decoder list. Receive only. The decoders are rtl_433's (docs/SENSORS-3.7.md says which ones are supported and how far they were tested).
#include "app.h"
#include "ease/ease_ui.h"
#include "i18n/i18n.h"
#include "dect2/common/time_fmt.h"
#include "dect2/sensors/sensors_rx.h"
#include "dect2/sensors/sensors_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;
    bool loaded = false;
    int removeIdx = 0;                  // index into kRemove
    std::string selKey;                 // the sensor shown in the graph
    int selUnknown = 0;
    std::string spec, specNote;         // the flex editor
    char specBuf[640] = {};
    int specFor = -1;                   // the unknown entry the editor was filled from (its utc + index)
    int64_t specUtc = 0;
    std::vector<std::string> custom;
    std::set<unsigned> diff;            // protocol numbers whose state differs from rtl_433's default
    char filter[64] = {};
    std::string note;
    std::string graphKey;               // which value of the selected sensor the graph shows ("" or kBothKey: the two main values together)
    std::set<std::string> pinned;       // sensors that never expire (saved as sensorsPinned)
};
State S;

struct RemoveOpt { const char* label; int sec; };
const RemoveOpt kRemove[] = {{N_("Never"), 0}, {N_("1 minute"), 60}, {N_("5 minutes"), 300}, {N_("15 minutes"), 900}, {N_("1 hour"), 3600}, {N_("2 hours"), 7200}, {N_("6 hours"), 21600}, {N_("1 day"), 86400}};
const int kRemoveDefaultSec = 3600;   // a car's tyre sensors pass once; an hour is long enough for every normal sensor (they send every 30-180 s) and short enough to tidy the list
const char* const kBothKey = "*both*";

bool live(const App& a) { return a.rx.standard == 242; }   // the engine reports its standard code minus one

std::string join(const std::vector<std::string>& v, char sep) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) { if (i) o += sep; o += v[i]; }
    return o;
}

void applyDiff(App& a) {
    for (const auto& p : dect2::SensorsReceiver::protocols()) {
        const bool want = S.diff.count(p.num) ? !p.defaultEnabled : p.defaultEnabled;
        a.engine.sensors().setProtocolEnabled(p.num, want);
    }
}

void load(App& a) {
    if (S.loaded) return;
    S.loaded = true;
    {   // saved as seconds (the old index key counted a shorter list); a profile without the new key gets the default
        const int sec = (int)plat::prefs().getI("sensorsRemoveSec", kRemoveDefaultSec);
        S.removeIdx = 0;
        for (int i = 0; i < (int)(sizeof kRemove / sizeof *kRemove); i++) if (kRemove[i].sec == sec) S.removeIdx = i;
        a.engine.sensors().setRemoveAfterSec(kRemove[S.removeIdx].sec);
        std::stringstream ps(plat::prefs().getS("sensorsPinned", ""));
        std::string k;
        while (std::getline(ps, k, '\n')) if (!k.empty()) S.pinned.insert(k);
        a.engine.sensors().setPinned(std::vector<std::string>(S.pinned.begin(), S.pinned.end()));
    }
    std::stringstream ss(plat::prefs().getS("sensorsFlex", ""));
    std::string l;
    while (std::getline(ss, l, '\n')) if (!l.empty()) S.custom.push_back(l);
    a.engine.sensors().setCustomDecoders(S.custom);
    std::stringstream ds(plat::prefs().getS("sensorsDiff", ""));
    while (std::getline(ds, l, ',')) if (!l.empty()) S.diff.insert((unsigned)std::atoi(l.c_str()));
    if (!S.diff.empty()) applyDiff(a);
}

void saveCustom(App& a) {
    plat::prefs().setS("sensorsFlex", join(S.custom, '\n'));
    a.engine.sensors().setCustomDecoders(S.custom);
}

void saveDiff() {
    std::string s;
    for (unsigned n : S.diff) { if (!s.empty()) s += ","; s += std::to_string(n); }
    plat::prefs().setS("sensorsDiff", s);
}

const dect2::SensorField* field(const dect2::SensorDevice& d, const char* k) {
    for (const auto& f : d.latest) if (f.key == k) return &f;
    return nullptr;
}

std::string fmtNum(double v, int dec) { char b[40]; std::snprintf(b, sizeof b, "%.*f", dec, v); return b; }

std::string tempText(const dect2::SensorDevice& d) {
    if (auto* f = field(d, "temperature_C")) return fmtNum(f->num, 1) + " \xC2\xB0""C";
    if (auto* f = field(d, "temperature_F")) return fmtNum(f->num, 1) + " \xC2\xB0""F";
    if (auto* f = field(d, "temperature_1_C")) return fmtNum(f->num, 1) + " \xC2\xB0""C";
    return "";
}
std::string humText(const dect2::SensorDevice& d) {
    if (auto* f = field(d, "humidity")) return fmtNum(f->num, 0) + " %";
    if (auto* f = field(d, "moisture")) return fmtNum(f->num, 0) + " %";
    return "";
}
std::string presText(const dect2::SensorDevice& d) {
    if (auto* f = field(d, "pressure_hPa")) return fmtNum(f->num, 1) + " hPa";
    if (auto* f = field(d, "pressure_kPa")) return fmtNum(f->num, 1) + " kPa";
    if (auto* f = field(d, "pressure_PSI")) return fmtNum(f->num, 1) + " psi";
    if (auto* f = field(d, "pressure_bar")) return fmtNum(f->num, 2) + " bar";
    return "";
}
std::string windText(const dect2::SensorDevice& d) {
    std::string o;
    if (auto* f = field(d, "wind_avg_km_h")) o = fmtNum(f->num, 1) + " km/h";
    else if (auto* f2 = field(d, "wind_avg_m_s")) o = fmtNum(f2->num, 1) + " m/s";
    else if (auto* f3 = field(d, "wind_avg_mi_h")) o = fmtNum(f3->num, 1) + " mph";
    if (auto* g = field(d, "wind_dir_deg")) o += (o.empty() ? "" : "  ") + fmtNum(g->num, 0) + "\xC2\xB0";
    return o;
}
std::string rainText(const dect2::SensorDevice& d) {
    if (auto* f = field(d, "rain_mm")) return fmtNum(f->num, 1) + " mm";
    if (auto* f = field(d, "rain_in")) return fmtNum(f->num, 2) + " in";
    return "";
}
std::string batText(const dect2::SensorDevice& d) {
    return d.batteryOk < 0 ? "" : d.batteryOk ? tr("ok") : tr("low");
}
std::string otherText(const dect2::SensorDevice& d) {
    static const std::set<std::string> skip = {"model", "id", "channel", "mic", "battery_ok", "battery", "temperature_C", "temperature_F", "humidity", "pressure_hPa", "pressure_kPa", "pressure_PSI", "wind_avg_km_h", "wind_avg_m_s", "wind_avg_mi_h", "wind_dir_deg", "rain_mm", "rain_in", "subtype", "temperature_1_C"};
    std::string o;
    for (const auto& f : d.latest) {
        if (skip.count(f.key)) continue;
        if (!o.empty()) o += ", ";
        o += f.key + "=" + f.text;
        if (o.size() > 160) { o += " ..."; break; }
    }
    return o;
}

std::string ago(int64_t now, int64_t t) {
    const int64_t s = std::max<int64_t>(0, now - t);
    char b[32];
    if (s < 90) std::snprintf(b, sizeof b, "%llds", (long long)s);
    else if (s < 5400) std::snprintf(b, sizeof b, "%lldm", (long long)(s / 60));
    else std::snprintf(b, sizeof b, "%lldh", (long long)(s / 3600));
    return b;
}

void writeFile(const std::string& path, const std::string& text, std::string& note) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { note = std::string(tr("Could not write")) + ": " + path; return; }
    f << text;
    note = std::string(tr("Saved")) + ": " + path;
}

void banner(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("433.92 MHz, 250 kHz. RTL-SDR or HackRF; 315, 868 and 915 MHz work the same way."));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Decoders from rtl_433 (OOK / ASK and FSK sensors: weather stations, thermometers, tyre pressure, door and smoke sensors, meters, remotes). Not every sensor on the air is covered: the list of supported protocols is in the Decoders view. A signal that no decoder takes is shown in Unknown signals, where you can turn it into your own decoder."));
    flowNext(10 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##sens", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.sensors().clearResults();
    flowEnd();
}

// ----- the sensor list -----
void sensorsView(App& a) {
    const dect2::SensorsTelemetry& t = a.rx.sensors;
    // Remove after, export
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Remove after")); ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::BeginCombo("##sensrm", tr(kRemove[S.removeIdx].label))) {
        for (int i = 0; i < (int)(sizeof kRemove / sizeof *kRemove); i++)
            if (ImGui::Selectable(tr(kRemove[i].label), i == S.removeIdx)) {
                S.removeIdx = i;
                plat::prefs().setI("sensorsRemoveSec", kRemove[i].sec);
                a.engine.sensors().setRemoveAfterSec(kRemove[i].sec);
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A sensor that has not been heard for this long leaves the list (a car passing with tyre sensors, a neighbour's thermometer). Sensors you pin keep their place. Most sensors send every 30 to 180 seconds."));
    ImGui::SameLine(0, 12 * gUi);
    if (ImGui::Button(tr("Export CSV...##sens"))) { const std::string p = saveFileDialog("sensors.csv"); if (!p.empty()) writeFile(p, dect2::sensorsCsv(t), S.note); }
    ImGui::SameLine();
    if (ImGui::Button(tr("JSON...##sens"))) { const std::string p = saveFileDialog("sensors.json"); if (!p.empty()) writeFile(p, dect2::sensorsJson(t), S.note); }
    ImGui::SameLine();
    if (ImGui::Button(tr("MQTT lines...##sens"))) { const std::string p = saveFileDialog("sensors-mqtt.txt"); if (!p.empty()) writeFile(p, dect2::sensorsMqtt(t), S.note); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("One line per value: topic and payload, topics as rtl_433's MQTT output (rtl_433/devices/<model>/<channel>/<id>/<value>), ready for mosquitto_pub."));
    if (!S.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.note.c_str()); }

    const float graphH = 170 * gUi;   // the lower box: the history of the selected row
    if (!ImGui::BeginTable("##sensl", 12, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                           ImVec2(0, std::max(60.f * gUi, ImGui::GetContentRegionAvail().y - graphH - 6 * gUi)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Pin"), ImGuiTableColumnFlags_WidthFixed, 30 * gUi); ImGui::TableSetupColumn(tr("Sensor")); ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn(tr("Ch")); ImGui::TableSetupColumn(tr("Temperature"));
    ImGui::TableSetupColumn(tr("Humidity")); ImGui::TableSetupColumn(tr("Pressure")); ImGui::TableSetupColumn(tr("Wind")); ImGui::TableSetupColumn(tr("Rain"));
    ImGui::TableSetupColumn(tr("Battery")); ImGui::TableSetupColumn(tr("dB")); ImGui::TableSetupColumn(tr("Last heard"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const int64_t now = (int64_t)std::time(nullptr);
    // pinned sensors first, then the others as the receiver has them: the most recently heard on top, so a sensor that has gone quiet sinks and expires
    std::vector<const dect2::SensorDevice*> order;
    for (const auto& d : t.devices) if (S.pinned.count(d.key)) order.push_back(&d);
    for (const auto& d : t.devices) if (!S.pinned.count(d.key)) order.push_back(&d);
    for (const dect2::SensorDevice* dp : order) {
        const dect2::SensorDevice& d = *dp;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        {
            bool pin = S.pinned.count(d.key) != 0;
            if (ImGui::Checkbox(("##pin" + d.key).c_str(), &pin)) {
                if (pin) S.pinned.insert(d.key); else S.pinned.erase(d.key);
                std::string v;
                for (const auto& k : S.pinned) v += k + "\n";
                plat::prefs().setS("sensorsPinned", v);
                a.engine.sensors().setPinned(std::vector<std::string>(S.pinned.begin(), S.pinned.end()));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pin: this sensor stays at the top and is never removed by Remove after."));
        }
        ImGui::TableNextColumn();
        const bool sel = d.key == S.selKey;
        if (ImGui::Selectable((d.model + "##" + d.key).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) { S.selKey = sel ? "" : d.key; S.graphKey.clear(); }
        if (ImGui::IsItemHovered()) {
            const std::string o = otherText(d);
            ImGui::SetTooltip("%s: %s  (%llu)\n%s", d.kind.c_str(), d.model.c_str(), (unsigned long long)d.count, o.c_str());
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.id.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.channel.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(tempText(d).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(humText(d).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(presText(d).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(windText(d).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(rainText(d).c_str());
        ImGui::TableNextColumn();
        if (d.batteryOk == 0) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.3f, 1), "%s", tr("low")); else ImGui::TextUnformatted(batText(d).c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", d.rssiDb);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s  x%llu", ago(now, d.lastSeen).c_str(), (unsigned long long)d.count);
    }
    ImGui::EndTable();
    if (t.devices.empty()) ImGui::TextDisabled("%s", t.packages ? tr("No sensor decoded yet. A pulse signal was heard: see Unknown signals.") : tr("No sensors yet. Sensors send every 30 to 180 seconds; wait a few minutes."));

    // the graph of the selected sensor
    if (S.selKey.empty()) { ImGui::TextDisabled("%s", tr("Select a sensor above to see its history here.")); return; }
    const dect2::SensorDevice* sd = nullptr;
    for (const auto& d : t.devices) if (d.key == S.selKey) { sd = &d; break; }
    if (!sd) { ImGui::TextDisabled("%s", tr("The selected sensor left the list.")); return; }
    if (sd->history.empty()) { ImGui::TextDisabled("%s", tr("No measured values to graph for this sensor.")); return; }
    // the two main values together (temperature + humidity, pressure + temperature ...), or any one value
    auto find = [&](const char* k) -> const dect2::SensorSeries* { for (const auto& x : sd->history) if (x.key == k) return &x; return nullptr; };
    auto isTemp = [](const std::string& k) { return k.rfind("temperature", 0) == 0; };
    const dect2::SensorSeries *s1 = nullptr, *s2 = nullptr;   // the pair shown in "both"
    for (const auto& x : sd->history) if (!s1 && isTemp(x.key)) s1 = &x;
    if (s1) for (const char* k : {"humidity", "pressure_hPa", "pressure_kPa", "pressure_PSI", "pressure_bar", "moisture"}) if (!s2 && (s2 = find(k)) != nullptr) break;
    if (s1 && !s2) for (const auto& x : sd->history) if (&x != s1 && !s2) s2 = &x;
    if (!s1) { s1 = &sd->history.front(); if (sd->history.size() > 1) s2 = &sd->history[1]; }
    const bool canBoth = s2 != nullptr;
    if (S.graphKey.empty() || (S.graphKey == kBothKey && !canBoth) || (S.graphKey != kBothKey && !find(S.graphKey.c_str()))) S.graphKey = canBoth ? kBothKey : s1->key;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(sd->model.c_str()); ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * gUi);
    const std::string cur = S.graphKey == kBothKey ? std::string(tr("Both values")) : S.graphKey;
    if (ImGui::BeginCombo("##sensg", cur.c_str())) {
        if (canBoth && ImGui::Selectable(tr("Both values"), S.graphKey == kBothKey)) S.graphKey = kBothKey;
        for (const auto& x : sd->history) if (ImGui::Selectable(x.key.c_str(), x.key == S.graphKey)) S.graphKey = x.key;
        ImGui::EndCombo();
    }
    const bool both = S.graphKey == kBothKey;
    const dect2::SensorSeries* a1 = both ? s1 : find(S.graphKey.c_str());
    const dect2::SensorSeries* a2 = both ? s2 : nullptr;
    if (!a1 || a1->pts.size() < 2) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("history builds up as messages arrive")); return; }
    int64_t t0 = a1->pts.front().utc, t1 = a1->pts.back().utc;
    if (a2 && a2->pts.size() >= 2) { t0 = std::min(t0, a2->pts.front().utc); t1 = std::max(t1, a2->pts.back().utc); }
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f %s", (double)(t1 - t0) / 60.0, tr("min span"));
    // the plot: one or two series, each with its own axis (left, right) and colour, and a legend
    const ImVec2 sz(ImGui::GetContentRegionAvail().x, std::max(60.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sensplot", sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 cText = ImGui::GetColorU32(ImGuiCol_TextDisabled), cGrid = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 col[2] = {IM_COL32(240, 150, 70, 255), IM_COL32(90, 180, 240, 255)};
    const float padL = 56 * gUi, padR = a2 ? 56 * gUi : 8 * gUi, padT = 18 * gUi, padB = 4 * gUi;
    const ImVec2 q0(p0.x + padL, p0.y + padT), q1(p0.x + sz.x - padR, p0.y + sz.y - padB);
    dl->AddRect(q0, q1, cGrid);
    const dect2::SensorSeries* sers[2] = {a1, a2};
    for (int si = 0; si < 2; si++) {
        const dect2::SensorSeries* sr = sers[si];
        if (!sr || sr->pts.empty()) continue;
        double lo = 1e300, hi = -1e300;
        for (const auto& p : sr->pts) { lo = std::min(lo, p.v); hi = std::max(hi, p.v); }
        double pad = (hi - lo) * 0.1; if (pad < 0.01) pad = 0.5;
        lo -= pad; hi += pad;
        auto X = [&](int64_t u) { return q0.x + (t1 > t0 ? (float)(u - t0) / (float)(t1 - t0) : 0.f) * (q1.x - q0.x); };
        auto Y = [&](double v) { return q1.y - (float)((v - lo) / (hi - lo)) * (q1.y - q0.y); };
        for (size_t k = 1; k < sr->pts.size(); k++) dl->AddLine(ImVec2(X(sr->pts[k - 1].utc), Y(sr->pts[k - 1].v)), ImVec2(X(sr->pts[k].utc), Y(sr->pts[k].v)), col[si], 1.8f * gUi);
        // the axis labels in the series' colour: left for the first, right for the second
        for (int g = 0; g <= 4; g++) {
            const double v = lo + (hi - lo) * g / 4.0;
            char b[24]; std::snprintf(b, sizeof b, "%.1f", v);
            const float y = Y(v);
            if (si == 0) dl->AddText(ImVec2(q0.x - ImGui::CalcTextSize(b).x - 4 * gUi, y - ImGui::GetFontSize() * 0.5f), col[0], b);
            else dl->AddText(ImVec2(q1.x + 4 * gUi, y - ImGui::GetFontSize() * 0.5f), col[1], b);
        }
        // the legend: colour, name, last value
        char lg[96]; std::snprintf(lg, sizeof lg, "%s  %s %.1f", sr->key.c_str(), tr("now"), sr->pts.back().v);
        const float lx = si == 0 ? q0.x : q0.x + (q1.x - q0.x) * 0.5f;
        dl->AddRectFilled(ImVec2(lx, p0.y + 5 * gUi), ImVec2(lx + 10 * gUi, p0.y + 13 * gUi), col[si]);
        dl->AddText(ImVec2(lx + 14 * gUi, p0.y + 1 * gUi), cText, lg);
    }
}

// ----- messages -----
void messagesView(App& a) {
    const auto& r = a.rx.sensors.recent;
    ImGui::TextDisabled("%s", tr("The messages as rtl_433 prints them (JSON), newest first."));
    ImGui::BeginChild("##sensm", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
    ImGui::PushFont(a.mono, 0);
    for (const auto& l : r) { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(l.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::PopFont();
    if (r.empty()) ImGui::TextDisabled("%s", tr("no messages yet"));
    ImGui::EndChild();
}

// ----- unknown signals -----
void unknownView(App& a) {
    const auto& un = a.rx.sensors.unknown;
    ImGui::TextDisabled("%s", tr("Pulse trains that no decoder took: pulse widths, a modulation guess, the bits and a decoder definition you can edit, test and save."));
    if (un.empty()) { ImGui::TextDisabled("%s", tr("none yet")); return; }
    S.selUnknown = std::clamp(S.selUnknown, 0, (int)un.size() - 1);
    const float listH = std::min(130.f * gUi, ImGui::GetContentRegionAvail().y * 0.35f);
    if (ImGui::BeginTable("##sensu", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("Pulses")); ImGui::TableSetupColumn("ms"); ImGui::TableSetupColumn("dB");
        ImGui::TableSetupColumn(tr("Guess"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)un.size(); i++) {
            const auto& u = un[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable((dect2::timefmt::clock(u.utc) + "##u" + std::to_string(i)).c_str(), i == S.selUnknown, ImGuiSelectableFlags_SpanAllColumns)) S.selUnknown = i;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(u.fsk ? "FSK" : "OOK");
            ImGui::TableNextColumn(); ImGui::Text("%d", u.pulses);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", u.durationMs);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", u.snrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(u.modName.empty() ? u.guess.c_str() : u.modName.c_str());
        }
        ImGui::EndTable();
    }
    const auto& u = un[(size_t)S.selUnknown];
    // fill the editor once per signal
    if (S.specFor != S.selUnknown || S.specUtc != u.utc) {
        S.specFor = S.selUnknown; S.specUtc = u.utc;
        std::snprintf(S.specBuf, sizeof S.specBuf, "%s", u.flexSpec.c_str());
        S.specNote.clear();
    }
    ImGui::BeginChild("##sensud", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(u.guess.c_str());
    ImGui::TextDisabled("%s %.3f MHz,  %.0f ms,  SNR %.0f dB", u.fsk ? tr("FSK mark frequency") : tr("centre"), u.freqMHz, u.durationMs, u.snrDb);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Pulse / gap widths (us)"));
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(u.widths.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    if (!u.bits.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Bits"));
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(u.bits.c_str()); ImGui::PopTextWrapPos();
        ImGui::TextDisabled("%s", u.hex.c_str());
        ImGui::PopFont();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Custom decoder (rtl_433 flex syntax)"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("n=name, m=modulation (OOK_PWM, OOK_PPM, OOK_PCM, OOK_MC_ZEROBIT, OOK_DMC, FSK_PCM, FSK_PWM, FSK_MC_ZEROBIT), s=short and l=long width in microseconds, r=reset gap, g=gap limit, t=tolerance; options bits>=N, rows>=N, repeats>=N, invert, reflect, match={N}hex, get=@0:{8}:id ... A saved decoder reports its rows as a sensor with the name you gave. Same syntax as rtl_433 -X."));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##sensspec", S.specBuf, sizeof S.specBuf);
    if (ImGui::Button(tr("Test##sens"))) {
        const std::string e = dect2::SensorsReceiver::checkFlexSpec(S.specBuf);
        S.specNote = e.empty() ? tr("The definition is valid.") : e;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Save decoder##sens"))) {
        const std::string e = dect2::SensorsReceiver::checkFlexSpec(S.specBuf);
        if (!e.empty()) S.specNote = e;
        else {
            const std::string sp = S.specBuf;
            if (std::find(S.custom.begin(), S.custom.end(), sp) == S.custom.end()) { S.custom.push_back(sp); saveCustom(a); }
            S.specNote = tr("Saved. It runs on every package from now on; see the Decoders view.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Reset to the suggestion##sens"))) std::snprintf(S.specBuf, sizeof S.specBuf, "%s", u.flexSpec.c_str());
    ImGui::SameLine();
    if (ImGui::Button(tr("Copy##sensspec"))) ImGui::SetClipboardText(S.specBuf);
    if (!S.specNote.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.specNote.c_str()); ImGui::PopTextWrapPos(); }
    if (u.flexSpec.empty()) ImGui::TextDisabled("%s", tr("No bit pattern was found to suggest a decoder from; widths above can still help."));
    ImGui::EndChild();
}

// ----- decoders -----
void decodersView(App& a) {
    const auto protos = dect2::SensorsReceiver::protocols();
    ImGui::TextDisabled("%s", tr("The decoders from rtl_433. Switch off what you do not need; decoders that rtl_433 keeps off by default (noisy, many false positives) are listed unticked."));
    if (!S.custom.empty()) {
        ImGui::TextUnformatted(tr("Your decoders"));
        for (size_t i = 0; i < S.custom.size(); i++) {
            ImGui::PushID((int)i);
            if (ImGui::SmallButton(tr("Remove"))) { S.custom.erase(S.custom.begin() + (long)i); saveCustom(a); ImGui::PopID(); break; }
            ImGui::SameLine();
            ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(S.custom[i].c_str()); ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::Spacing();
    }
    ImGui::SetNextItemWidth(180 * gUi);
    ImGui::InputTextWithHint("##sensf", tr("filter"), S.filter, sizeof S.filter);
    ImGui::SameLine();
    unsigned en = 0;
    for (const auto& p : protos) if (S.diff.count(p.num) ? !p.defaultEnabled : p.defaultEnabled) en++;
    ImGui::TextDisabled("%u / %zu %s", en, protos.size(), tr("decoders on"));
    ImGui::SameLine(0, 12 * gUi);
    if (ImGui::SmallButton(tr("Defaults##sens"))) { S.diff.clear(); saveDiff(); a.engine.sensors().enableAll(false); }
    std::string flt = S.filter;
    for (auto& c : flt) c = (char)std::tolower((unsigned char)c);
    ImGui::BeginChild("##sensdl", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
    for (const auto& p : protos) {
        if (!flt.empty()) {
            std::string n = p.name;
            for (auto& c : n) c = (char)std::tolower((unsigned char)c);
            if (n.find(flt) == std::string::npos) continue;
        }
        bool on = S.diff.count(p.num) ? !p.defaultEnabled : p.defaultEnabled;
        const char* mod = p.modulation >= 16 ? "FSK" : "OOK";
        if (ImGui::Checkbox((p.name + "  [" + std::to_string(p.num) + ", " + mod + "]##p" + std::to_string(p.num)).c_str(), &on)) {
            if (on == p.defaultEnabled) S.diff.erase(p.num); else S.diff.insert(p.num);
            saveDiff();
            a.engine.sensors().setProtocolEnabled(p.num, on);
        }
    }
    ImGui::EndChild();
}

void tab(App& a) {
    load(a);
    easeui::autoBandControl(a, "sensAutoBand", a.rx.sensors.blocksOk);
    banner(a);
    ImGui::Spacing();
    subNav("sensv", S.view, {"Sensors", "Messages", "Unknown signals", "Decoders"});
    if (!live(a) && S.view != 3) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) sensorsView(a); else if (S.view == 1) messagesView(a); else if (S.view == 2) unknownView(a); else decodersView(a);
}

void list(App& a) {
    load(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& dv = a.rx.sensors.devices;
    if (!ImGui::BeginTable("##sensside", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Sensor")); ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthStretch);
    for (const auto& d : dv) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted((d.model + (d.id.empty() ? "" : " " + d.id)).c_str());
        ImGui::TableNextColumn();
        std::string v = tempText(d);
        const std::string h = humText(d), p = presText(d);
        if (!h.empty()) v += (v.empty() ? "" : "  ") + h;
        if (!p.empty()) v += (v.empty() ? "" : "  ") + p;
        ImGui::TextDisabled("%s", v.c_str());
    }
    ImGui::EndTable();
    if (dv.empty()) ImGui::TextDisabled("%s", tr("no sensors yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::SensorsTelemetry& t = a.rx.sensors;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::sensorsSummary(t).c_str());
    kv(tr("messages"), "%llu", (unsigned long long)t.blocksOk);
    kv(tr("sensors"), "%zu", t.devices.size());
    kv(tr("pulse packages"), "%llu (OOK %llu, FSK %llu)", (unsigned long long)t.packages, (unsigned long long)t.ookPackages, (unsigned long long)t.fskPackages);
    kv(tr("not decoded"), "%llu", (unsigned long long)t.blocksBad);
    kv(tr("decoders"), "%u (+%u %s)", t.protocols, t.customProtocols, tr("own"));
    kv(tr("input"), "%.3f Msps (%.0f ksps)", t.inputRate / 1e6, t.workRate / 1e3);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Pulses"), !on ? 0 : a.rx.sensors.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Sensor"), !on ? 0 : a.rx.sensors.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::sensorsSummary(a.rx.sensors).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Sub-GHz sensors";
    if (live(a)) l2 = dect2::sensorsSummary(a.rx.sensors);
}

} // namespace

extern const ModeUi kSensorsUi;
const ModeUi kSensorsUi = {
    .sideTitle = "SENSORS",
    .tabName = "Sensors",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = nullptr,
};
