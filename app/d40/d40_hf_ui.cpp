// HF beacons: the beacon frequency that has been best at this hour of day at this site, learned from the receiver's own readings and kept between runs. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/d40/hf_e40.h"
#include "dect2/hfbeacon/hfbeacon_tel.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {
dect2::d40::BandClock clk;
bool loaded = false, dirty = false;
double lastSave = 0;
std::vector<double> lastHeard;
std::string path() { return plat::dataDir() + "/hf_bandclock.txt"; }
void ensureLoaded() {
    if (loaded) return;
    loaded = true;
    std::ifstream f(path(), std::ios::binary); std::ostringstream ss; ss << f.rdbuf();
    clk.load(ss.str());
}
}  // namespace

void d40HfbFeed(const std::vector<double>& khz, const dect2::HfbTelemetry& t) {
    ensureLoaded();
    if (!t.clockValid || t.beacons.size() != khz.size()) return;
    if (lastHeard.size() != khz.size()) lastHeard.assign(khz.size(), 0);
    for (size_t i = 0; i < khz.size(); i++) {
        const auto& s = t.beacons[i];
        if (!s.heard || s.conf < 1 || s.lastHeard <= lastHeard[i] || std::isnan(s.snr)) continue;
        lastHeard[i] = s.lastHeard;
        clk.add(khz[i] * 1e3, (int)std::fmod(std::floor(s.lastHeard / 3600.0), 24.0), s.snr);
        dirty = true;
    }
    const double now = (double)std::time(nullptr);
    if (dirty && now - lastSave > 300) { lastSave = now; dirty = false; std::ofstream f(path(), std::ios::binary | std::ios::trunc); f << clk.save(); }
}

void d40HfbBestCard() {
    ensureLoaded();
    if (!typo::collapsingHeader(trw("Best beacon frequency for this hour"), 0)) return;
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Learned from what this receiver heard at this hour of the day on each beacon frequency (averaged over days, kept between runs in hf_bandclock.txt). It needs a few days of listening; a missing hour is filled from the hours next to it, and says so. No propagation model, nothing is downloaded."));
    const auto fr = clk.frequencies();
    const int hour = (int)std::fmod(std::floor((double)std::time(nullptr) / 3600.0), 24.0);
    const auto b = clk.best(fr, hour);
    if (!b.ok) { ImGui::TextDisabled("%s", tr("Nothing learned yet for this hour: leave the beacon monitor running.")); return; }
    bool nb = false; clk.expected(b.freqHz, hour, &nb);
    ImGui::Text("%s: %.3f MHz, %s %.0f dB%s", tr("Best now"), b.freqHz / 1e6, tr("typically"), b.snrDb, nb ? tr(" (from the neighbouring hours)") : "");
    const int ch = clk.hoursToChange(fr, hour);
    if (ch > 0) ImGui::TextDisabled("%s %d %s", tr("A different frequency is best in"), ch, tr("h (UTC)")); else ImGui::TextDisabled("%s", tr("No change expected in the next 24 hours."));
}
