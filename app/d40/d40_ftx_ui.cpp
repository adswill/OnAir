// FT8 / FT4 / WSPR / JS8: heard stations, spot summary per band, clear transmit frequency, band suggestion. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/ham/ham_bandhop.h"
#include "dect2/ham/ham_callsign.h"
#include "dect2/ham/ham_clearfreq.h"
#include "dect2/ham/ham_spotsum.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <unordered_set>

namespace {
dect2::HamSpotSummary spots;
dect2::HamCallHeard heard;
dect2::HamBandHop hop;
std::unordered_set<uint64_t> seen;
double lastLat = 1e9, lastLon = 1e9, lastHop = 0;
std::string suggested;
bool cqOnly = false, loaded = false;

const char* bandOf(double mhz) {
    struct B { const char* n; double lo, hi; };
    static const B b[] = {{"160m", 1.8, 2.0}, {"80m", 3.5, 4.0}, {"60m", 5.25, 5.45}, {"40m", 7.0, 7.3}, {"30m", 10.1, 10.15}, {"20m", 14.0, 14.35}, {"17m", 18.068, 18.168},
                          {"15m", 21.0, 21.45}, {"12m", 24.89, 24.99}, {"11m", 26.9, 27.5}, {"10m", 28.0, 29.7}, {"6m", 50.0, 54.0}, {"4m", 69.9, 70.5}, {"2m", 144.0, 148.0}, {"70cm", 430.0, 440.0}};
    for (const auto& x : b) if (mhz >= x.lo && mhz <= x.hi) return x.n;
    return "?";
}
uint64_t keyOf(const dect2::FtxDecode& d) {
    uint64_t h = std::hash<std::string>()(d.msg);
    h ^= (uint64_t)(d.slotUtc * 4) * 0x9E3779B97F4A7C15ull; h ^= (uint64_t)(int)(d.hz * 2) << 7; h += d.mode;
    return h;
}
}  // namespace

void d40FtxCard(App& a, const dect2::HfdigFtxTelemetry& f) {
    if (!loaded) { loaded = true; cqOnly = plat::prefs().getI("d40FtxHeardCq", 0) != 0; }
    if (!typo::collapsingHeader(trw("Heard stations, band summary and clear frequency"), 0)) return;
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Built from the decodes of this session. Heard: callsigns in the messages, newest first. Band summary: spots in the last 30 minutes per band, with the farthest one and the directions it is open to (needs your position and the stations' locators). Clear frequency: the widest gap in 200 - 2900 Hz for the even and the odd slot, from the last decodes: call there to avoid sitting on somebody. Suggested band: the busiest of the last hour, every band still revisited every 30 minutes."));
    double lat, lon; receiverLocation(lat, lon);
    if (lat != lastLat || lon != lastLon) { lastLat = lat; lastLon = lon; spots.setStation(lat, lon); }
    const double now = (double)std::time(nullptr);
    const double dialMhz = a.freqMhz;
    static bool bandsSet = false;
    if (!bandsSet) { bandsSet = true; hop.setBands({"160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "2m"}); }
    if (seen.size() > 6000) seen.clear();
    for (const auto& d : f.decodes) {
        if (d.early || d.mode > 3) continue;   // FT8, FT4, FT2, WSPR
        if (!seen.insert(keyOf(d)).second) continue;
        const std::string band = bandOf((d.dialHz > 0 ? d.dialHz : dialMhz * 1e6) / 1e6);
        dect2::HamSpot s; s.band = band; s.timeS = d.slotUtc > 0 ? d.slotUtc : now; s.snrDb = d.snrDb;
        if (!d.grid.empty() && dect2::ftxGridToLatLon(d.grid, s.lat, s.lon)) s.hasPos = true;
        spots.add(s);
        hop.spot(band, s.timeS);
        heard.feed(d.msg, s.timeS);
    }
    for (const auto& b : spots.summary(now, 1800)) ImGui::TextUnformatted(b.line.c_str());
    if (spots.summary(now, 1800).empty()) ImGui::TextDisabled("%s", tr("No decodes in the last 30 minutes."));
    // clear frequency of the newest mode in the list
    if (!f.decodes.empty()) {
        const int mode = f.decodes.back().mode;
        if (mode <= 1) {
            const double per = dect2::ftxPeriod(mode), newest = f.decodes.back().slotUtc;
            std::vector<dect2::SlotSignal> sg;
            for (const auto& d : f.decodes) if (d.mode == mode && !d.early && d.slotUtc >= newest - 3 * per) sg.push_back({d.hz, (int)std::fmod(std::floor(d.slotUtc / per), 2.0), mode == 0 ? 50.0 : 90.0});
            const auto c = dect2::findClearFreq(sg, 200, 2900, mode == 0 ? 50.0 : 90.0);
            ImGui::Text("%s: %s %.0f Hz (%.0f Hz free), %s %.0f Hz (%.0f Hz free); %s: %s", tr("Clear frequency"), tr("even slots"), c.hz[0], c.gapHz[0], tr("odd slots"), c.hz[1], c.gapHz[1],
                        tr("quieter"), c.quieterParity ? tr("odd") : tr("even"));
        }
    }
    if (now - lastHop > 5) { lastHop = now; suggested = hop.next(now); }
    if (!suggested.empty()) ImGui::Text("%s: %s", tr("Suggested band"), suggested.c_str());
    ImGui::Separator();
    if (ImGui::Checkbox(tr("CQ callers only##d40ftxh"), &cqOnly)) plat::prefs().setI("d40FtxHeardCq", cqOnly ? 1 : 0);
    ImGui::SameLine(); ImGui::TextDisabled("%zu %s", heard.size(), tr("stations heard"));
    int shown = 0;
    for (const auto& e : heard.list()) {
        if (cqOnly && !e.cq) continue;
        if (++shown > 12) break;
        ImGui::Text("%-10s %3d x  %s%s", e.call.c_str(), e.count, e.cq ? "CQ  " : "", e.lastText.c_str());
    }
}
