// AM / short wave: carrier offset against the broadcast raster, best sideband, AM / DRM switch hint, station ID from the user's schedule CSV. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../smart5/smart5_ui.h"
#include "../ui/typography.h"
#include "dect2/d40/am_d40.h"
#include <cmath>
#include <cstdio>
#include <ctime>

namespace d4 = dect2::d40;

namespace {
d4::AmDrmSwitch sw;
d4::ScheduleTable sched;
bool schedLoaded = false;
int schedRows = 0;
}  // namespace

void d40AmCard(App& a) {
    if (!typo::collapsingHeader(trw("Carrier, sideband and station ID"), 0)) return;
    if (!schedLoaded) { schedLoaded = true; schedRows = sched.load(s5ui::readData("am_schedule.csv")); }
    const auto& db = a.spec.dbfs;
    if (!a.engine.running() || db.size() < 64) { ImGui::TextDisabled("%s", tr("Start the receiver.")); return; }
    const double fs = a.engine.sampleRate(), binHz = fs / (double)db.size();
    const double offHz = a.freqMhz * 1e6 - a.engine.captureCentreHz();
    const int c = (int)std::lround((double)db.size() / 2 + offHz / binHz);
    const int h = (int)std::lround(12000.0 / binHz);
    if (c - h < 0 || c + h >= (int)db.size()) { ImGui::TextDisabled("%s", tr("The listened channel is outside the capture.")); return; }
    const std::vector<float> win(db.begin() + (c - h), db.begin() + (c + h + 1));
    const double centreKhz = a.freqMhz * 1e3;
    const auto co = d4::carrierOffset(win, binHz, centreKhz);
    if (co.ok) {
        ImGui::Text("Carrier %.3f kHz: %+.0f Hz from the %d kHz raster (%.0f kHz)", co.carrierKhz, co.offsetHz, co.rasterKhz, co.nearestKhz);
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A steady offset of 50 to 200 Hz marks an off-raster or pirate transmitter; it is also what the synchronous detector has to pull in."));
    } else ImGui::TextDisabled("%s", tr("No carrier line found in the channel."));
    const auto sb = d4::bestSideband(win, binHz);
    const char* k = sb.kind == d4::SidebandChoice::Lsb ? "lower sideband" : (sb.kind == d4::SidebandChoice::Usb ? "upper sideband" : "both sidebands");
    ImGui::Text("Cleaner side for synchronous detection: %s (%.1f dB better)", k, sb.advantageDb);
    const auto kind = sw.feed(win, binHz);
    if (kind != d4::AmDrmSwitch::Unknown) ImGui::TextDisabled("%s: %s", tr("Looks like"), kind == d4::AmDrmSwitch::Drm ? "DRM (flat top, no carrier line)" : "AM (carrier line)");
    // station ID
    ImGui::Separator();
    if (schedRows == 0) ImGui::TextDisabled("%s", tr("Station ID: put a schedule file (khz,HHMM,HHMM,days,name,country; for example from an EiBi export) at am_schedule.csv in the data folder."));
    else {
        const std::time_t t = std::time(nullptr); const std::tm* g = std::gmtime(&t);
        const int wd = g->tm_wday == 0 ? 7 : g->tm_wday;
        const auto r = sched.lookup(centreKhz, wd, g->tm_hour * 60 + g->tm_min);
        if (r.empty()) ImGui::TextDisabled("%s", tr("Station ID: nothing in your schedule on this frequency now."));
        else for (size_t i = 0; i < r.size() && i < 3; i++) ImGui::Text("%s %s (%s)", i ? "   or" : "Schedule says:", r[i].name.c_str(), r[i].country.c_str());
    }
}
