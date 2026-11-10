// GOES / Meteosat DCS screens: the reports received (time, platform address, channel, rate, level, data) with the sensor values of the selected one,
// the table of platforms heard, and the band view (waterfall of the 401.7 - 402.1 MHz uplink with the carriers being followed).
// A data collection platform (river gauge, weather station, tide gauge, buoy) sends a short report on its own 1.5 kHz channel; the 8-hex-digit address
// is a BCH-protected code. Reports can be appended to a CSV file for unattended runs.
// The same screens serve the Meteosat HRDCP mode (family 700, hrdcp_live.h): its messages use the same report type with the HRDCP fields set.
#include "app.h"
#include "dect2/goesdcs/goesdcs_live.h"
#include "dect2/goesdcs/hrdcp_live.h"
#include "dect2/goesdcs/hrdcp_proto.h"
#include "dect2/goesdcs/goesdcs_proto.h"
#include "dect2/goesdcs/goesdcs_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                 // 0 reports, 1 platforms, 2 band
    uint32_t sel = 0;             // id of the report shown in detail
    int thr = 11;                 // carrier line over the noise, dB
    int pushedThr = -1;
    bool logOn = true;
    int pushedLog = -1;
    int plan = -1, pushedPlan = -2;   // channel plan (GOES DCS): -1 Auto, 0 GOES, 1 Meteosat, 2 Himawari
};
State gS[2];
bool gHr = false;                 // the screens being drawn are the Meteosat HRDCP ones (the callbacks of kHrdcpUi set it)
#define S (gS[gHr ? 1 : 0])

bool live(const App& a) { return a.rx.standard == (gHr ? 701 : 142); }
const dect2::DcsTelemetry& T(const App& a) { return gHr ? a.rx.hrdcp : a.rx.goesdcs; }
void clearRx(App& a) { if (gHr) a.engine.hrdcp().clearResults(); else a.engine.goesdcs().clearResults(); }
std::string summaryOf(const dect2::DcsTelemetry& t) { return gHr ? dect2::hrdcpSummary(t) : dect2::dcsSummary(t); }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::tm utc(int64_t t) {
    std::tm r{};
    const time_t tt = (time_t)t;
#ifdef _WIN32
    gmtime_s(&r, &tt);
#else
    gmtime_r(&tt, &r);
#endif
    return r;
}

std::string clock(int64_t sec) {
    const std::tm t = utc(sec);
    char b[24];
    snprintf(b, sizeof b, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return b;
}

std::string stamp(int64_t sec) {
    const std::tm t = utc(sec);
    char b[32];
    snprintf(b, sizeof b, "%02d-%02d %02d:%02d:%02d", t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return b;
}

std::string oneLine(const std::string& s, size_t n) {
    std::string o;
    for (char c : s) o.push_back(c == '\n' || c == '\r' ? ' ' : c);
    if (o.size() > n) o = o.substr(0, n) + "...";
    return o;
}

std::string logFile() { return plat::dataDir() + (gHr ? "/hrdcp.csv" : "/goes_dcs.csv"); }

void pushSettings(App& a) {
    if (S.thr != S.pushedThr) { if (gHr) a.engine.hrdcp().setThresholdDb(S.thr); else a.engine.goesdcs().setThresholdDb(S.thr); S.pushedThr = S.thr; }
    const int want = S.logOn ? 1 : 0;
    if (want != S.pushedLog) { const std::string f = S.logOn ? logFile() : std::string(); if (gHr) a.engine.hrdcp().setLogFile(f); else a.engine.goesdcs().setLogFile(f); S.pushedLog = want; }
    if (!gHr && S.plan != S.pushedPlan) { a.engine.goesdcs().setPlan(S.plan); S.pushedPlan = S.plan; }
}

void tick(App& a) {
    gHr = false; pushSettings(a);
    gHr = true; if (gS[1].thr == 11) gS[1].thr = 12; pushSettings(a);
    gHr = false;
}

ImU32 heat(float v) {   // 0..1: dark, blue, cyan, yellow, white
    const float r = std::min(1.f, std::max(0.f, (v - 0.5f) * 2.f)), g = std::min(1.f, std::max(0.f, v * 1.6f - 0.3f)), b = std::min(1.f, 0.25f + v * 1.4f) * (v > 0.75f ? 1.f - (v - 0.75f) * 2.f : 1.f);
    return IM_COL32((int)(255 * r), (int)(255 * g * 0.95f), (int)(255 * std::max(0.f, b) * (v < 0.02f ? 0.35f : 1.f)), 255);
}

std::string csvText(const dect2::DcsTelemetry& t) {
    std::string txt = "id,unix_time,address,channel,freq_hz,bps,coding,snr_db,format,parity_errors,text\n";
    for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {
        const auto& m = *it;
        std::string q;
        for (char c : m.text) { if (c == '"') q += "\"\""; else q += (c == '\n' ? ' ' : c); }
        char b[160];
        snprintf(b, sizeof b, "%u,%lld,%s,%d,%.0f,%d,%s,%.1f,%s,%d,\"", m.id, (long long)m.wallTime, dect2::dcs::addressHex(m.address).c_str(), m.channel, m.freqHz, m.bps,
                 m.manchester ? "manchester" : "nrz", (double)m.snrDb, m.format == 1 ? "pseudo-binary" : "ascii", m.parityErrors);
        txt += b + q + "\"\n";
    }
    return txt;
}

void controls(App& a) {
    const dect2::DcsTelemetry& t = T(a);
    if (ImGui::Button(tr("Clear"))) clearRx(a);
    ImGui::SameLine();
    if (ImGui::Button(tr("Copy"))) ImGui::SetClipboardText(csvText(t).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Copies the reports as CSV: id, time, platform address, channel, frequency, bit rate, coding, level, data format, parity errors, data."));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Log to file"), &S.logOn);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", gHr ? tr("Appends every message to hrdcp.csv in the data folder, also when nobody watches the screen.") : tr("Appends every report to goes_dcs.csv in the data folder, also when nobody watches the screen."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::SliderInt("##dcsthr", &S.thr, 6, 20, tr("threshold %d dB"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How far above the noise a carrier line must rise to be followed. Lower finds weaker platforms and more false carriers."));
}

void reportsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the reports")); return; }
    const dect2::DcsTelemetry& t = T(a);
    controls(a);
    ImGui::TextDisabled("%llu reports from %zu platforms, %llu cut off or with a bad address", (unsigned long long)t.blocksOk, t.platforms.size(), (unsigned long long)t.blocksBad);
    if (t.messages.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", gHr ? tr("No message yet. Tune 402.3 MHz (the HRDCP part of the Meteosat regional band, 402.2 to 402.435 MHz) with 0.25 Msps or more: platforms send in their assigned time slots, once every few minutes to once an hour.")
                                                   : tr("No report yet. Tune the uplink band (401.7 to 402.1 MHz, centre 401.85 MHz) with 0.5 Msps or more: platforms send once every few minutes to once an hour, mostly at set times."));
        if (!gHr) ImGui::TextDisabled("%s", tr("Meteosat and Himawari platforms: 402.0 to 402.435 MHz, centre 402.22 MHz."));
        ImGui::PopTextWrapPos();
    }
    const float h = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y * 0.5f);
    if (ImGui::BeginTable("##dcsrep", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time (UTC)")); ImGui::TableSetupColumn(tr("Platform")); ImGui::TableSetupColumn(tr("Channel"));
        ImGui::TableSetupColumn(gHr ? tr("Sequence") : tr("Rate")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("Data"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& m : t.messages) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable((stamp(m.wallTime) + "##" + std::to_string(m.id)).c_str(), S.sel == m.id, ImGuiSelectableFlags_SpanAllColumns)) S.sel = m.id;
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::dcs::addressHex(m.address).c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); if (m.channel) ImGui::Text("%d", m.channel); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (m.sequence >= 0) ImGui::Text("#%d", m.sequence); else ImGui::Text("%d bps", m.bps);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", (double)m.snrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(oneLine(m.text, 90).c_str());
        }
        ImGui::EndTable();
    }
    for (const auto& m : t.messages) {
        if (m.id != S.sel) continue;
        ImGui::Spacing();
        ImGui::Text("%s UTC  %s", stamp(m.wallTime).c_str(), dect2::dcs::addressHex(m.address).c_str());
        if (m.channel) kvRow(a, tr("channel"), "%s %d, %.4f MHz", dect2::dcs::planName((dect2::dcs::Plan)m.plan), m.channel, m.freqHz / 1e6);
        else if (m.freqAbsolute) kvRow(a, tr("channel"), "%.4f MHz", m.freqHz / 1e6);
        else kvRow(a, tr("channel"), "%+.0f Hz from the centre", m.freqHz);
        if (m.sequence >= 0) {
            kvRow(a, tr("signal"), "%s, %.1f dB over the noise", tr("1200 bit/s, offset QPSK, convolutional and Reed-Solomon code"), (double)m.snrDb);
            kvRow(a, tr("message"), "%s %d, %s, %s", tr("sequence"), m.sequence, m.hrType == 1 ? tr("alert") : tr("self-timed"), m.compression == 1 ? tr("gzip") : m.compression ? tr("compressed") : tr("not compressed"));
            kvRow(a, tr("health"), "%s", dect2::hrdcp::healthText(m.health).c_str());
            kvRow(a, tr("check"), "%s, %d %s, %d %s", m.crcOk ? tr("CRC good") : tr("CRC bad"), m.frames, tr("code frames"), m.rsCorrected, tr("byte errors corrected"));
            kvRow(a, tr("format"), "%s, %zu bytes", m.format == 2 ? tr("binary") : m.format == 1 ? tr("pseudo-binary B") : tr("ASCII"), m.raw.size());
        } else {
            kvRow(a, tr("signal"), "%d bps, %s, %.1f dB over the noise", m.bps, m.manchester ? tr("Manchester") : tr("NRZ"), (double)m.snrDb);
            if (m.flags >= 0) kvRow(a, tr("flags"), "0x%02X", m.flags);
            kvRow(a, tr("format"), "%s, %zu characters, %d parity errors%s", m.format == 1 ? tr("pseudo-binary B") : tr("ASCII"), m.raw.size(), m.parityErrors, m.eot ? "" : tr(", no end code (carrier lost)"));
        }
        kvRow(a, tr("address"), "%s", m.addressFixed ? tr("BCH check passed after correcting bit errors") : tr("BCH check passed"));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("sensor data"));
        ImGui::PushFont(a.mono, 0);
        for (const auto& f : m.fields) ImGui::BulletText("%s", f.c_str());
        ImGui::PopFont();
        if (m.format == 0 || m.format == 2) {
            ImGui::TextDisabled("%s", m.format == 2 ? tr("bytes (hex)") : tr("text as received"));
            ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(m.text.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
        }
        break;
    }
}

void platformsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the platforms")); return; }
    const dect2::DcsTelemetry& t = T(a);
    ImGui::TextDisabled("%s", tr("platforms heard, the most recent first"));
    if (ImGui::BeginTable("##dcsplat", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y - 4)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Platform")); ImGui::TableSetupColumn(tr("Reports")); ImGui::TableSetupColumn(tr("Last (UTC)")); ImGui::TableSetupColumn(tr("Channel"));
        ImGui::TableSetupColumn(tr("Rate")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("Latest data"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& p : t.platforms) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::dcs::addressHex(p.address).c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%u", p.count);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(stamp(p.lastTime).c_str());
            ImGui::TableNextColumn(); if (p.channel) ImGui::Text("%d", p.channel); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%d bps", p.bps);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", (double)p.snrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(oneLine(p.last, 90).c_str());
        }
        ImGui::EndTable();
    }
    if (t.platforms.empty()) ImGui::TextDisabled("%s", tr("no platform heard yet"));
}

void bandView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the band")); return; }
    const dect2::DcsTelemetry& t = T(a);
    ImGui::TextDisabled("%s", tr("the captured band, the last few seconds (brighter = stronger over the noise); carriers being followed are marked"));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = std::max(120.f * gUi, ImGui::GetContentRegionAvail().x);
    const float h = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y - 150 * gUi);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##dcswf", ImVec2(w, h));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(8, 9, 14, 255));
    if (t.waterfall.size() == (size_t)dect2::kDcsWfRows * dect2::kDcsWfCols) {
        const float cw = w / dect2::kDcsWfCols, ch = h / dect2::kDcsWfRows;
        for (int r = 0; r < dect2::kDcsWfRows; r++)
            for (int c = 0; c < dect2::kDcsWfCols; c++) {
                const uint8_t v = t.waterfall[(size_t)r * dect2::kDcsWfCols + c];
                if (v < 20) continue;
                dl->AddRectFilled(ImVec2(p.x + cw * c, p.y + ch * r), ImVec2(p.x + cw * (c + 1) + 0.5f, p.y + ch * (r + 1) + 0.5f), heat(v / 255.f));
            }
    }
    const double span = std::max(1.0, t.bandHighHz - t.bandLowHz);
    for (const auto& c : t.carriers) {
        const float x = p.x + (float)((c.offsetHz - t.bandLowHz) / span) * w;
        dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), c.state == 2 ? IM_COL32(120, 230, 150, 200) : IM_COL32(240, 190, 80, 160));
        char b[24];
        if (c.channel) snprintf(b, sizeof b, "ch %d", c.channel); else snprintf(b, sizeof b, "%+.1f kHz", c.offsetHz / 1e3);
        dl->AddText(ImVec2(x + 3, p.y + 2), IM_COL32(255, 230, 150, 255), b);
    }
    char lo[32], hi[32];
    if (t.centreHz > 0) { snprintf(lo, sizeof lo, "%.3f MHz", (t.centreHz + t.bandLowHz) / 1e6); snprintf(hi, sizeof hi, "%.3f MHz", (t.centreHz + t.bandHighHz) / 1e6); }
    else { snprintf(lo, sizeof lo, "%.0f kHz", t.bandLowHz / 1e3); snprintf(hi, sizeof hi, "+%.0f kHz", t.bandHighHz / 1e3); }
    dl->AddText(ImVec2(p.x + 3, p.y + h - 14 * gUi), IM_COL32(150, 156, 170, 255), lo);
    dl->AddText(ImVec2(p.x + w - 90 * gUi, p.y + h - 14 * gUi), IM_COL32(150, 156, 170, 255), hi);
    if (!t.spectrum.empty()) {
        ImGui::TextDisabled("%s", tr("spectrum now (dB over the local noise)"));
        ImGui::PlotLines("##dcsspec", t.spectrum.data(), (int)t.spectrum.size(), 0, nullptr, 0.f, 40.f, ImVec2(ImGui::GetContentRegionAvail().x, 56 * gUi));
    }
}

void tuner(App& a, bool& retune) {
    if (gHr) {
        ImGui::TextDisabled("%s", tr("Band"));
        if (ImGui::Button(tr("Meteosat HRDCP 402.3 MHz"))) { a.freqMhz = 402.3; retune = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The HRDCP assignments lie in 402.2005 - 402.4345 MHz: 0.25 Msps around 402.3 MHz holds them all."));
        return;
    }
    ImGui::TextDisabled("%s", tr("Band"));
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));
    const bool down = std::fabs(a.freqMhz - 1679.85) < 0.5, eu = !down && a.freqMhz >= 402.0 && a.freqMhz < 403.0;
    if (ImGui::BeginCombo("##goesband", down ? tr("Relay downlink 1679.85 MHz") : eu ? tr("Meteosat / Himawari 402.22 MHz") : tr("Uplink 401.85 MHz"))) {
        if (ImGui::Selectable(tr("Uplink 401.85 MHz"), !down && !eu)) { a.freqMhz = 401.85; retune = true; }
        if (ImGui::Selectable(tr("Meteosat / Himawari 402.22 MHz"), eu)) { a.freqMhz = 402.22; retune = true; }
        if (ImGui::Selectable(tr("Relay downlink 1679.85 MHz"), down)) { a.freqMhz = 1679.85; retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The satellite retransmits the uplink carriers around 1679.85 MHz: one dish hears a hemisphere of platforms. Tested on simulated signals only."));
    ImGui::TextDisabled("%s", tr("Channel plan"));
    const char* plans[4] = {tr("Auto (from the band)"), "GOES", "Meteosat", "Himawari"};
    int sel = S.plan + 1;
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##dcsplan", &sel, plans, 4)) { S.plan = sel - 1; pushSettings(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How the channel numbers are counted: NESDIS (GOES), EUMETSAT TD 16 table 11 (Meteosat), or none for Himawari (JMA publishes frequencies only). Auto uses GOES below 402.0 MHz and Meteosat from there up."));
}

void tab(App& a) {
    subNav("dcs", S.view, {tr("Reports"), tr("Platforms"), tr("Band")});
    if (S.view == 0) reportsView(a);
    else if (S.view == 1) platformsView(a);
    else bandView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::DcsTelemetry& t = T(a);
    ImGui::Text("%llu %s", (unsigned long long)t.blocksOk, tr("reports"));
    ImGui::PushFont(a.mono, 0);
    int n = 0;
    for (const auto& m : t.messages) { if (n++ >= 10) break; ImGui::Text("%s %s", clock(m.wallTime).c_str(), dect2::dcs::addressHex(m.address).c_str()); }
    ImGui::PopFont();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::DcsTelemetry& t = T(a);
    kvRow(a, tr("state"), "%s", summaryOf(t).c_str());
    kvRow(a, tr("carriers"), "%zu followed, %llu seen", t.carriers.size(), (unsigned long long)t.carriersSeen);
    for (const auto& c : t.carriers) {
        const char* st = c.state == 2 ? tr("receiving") : (c.state == 1 ? tr("locked, waiting for the sync") : tr("carrier"));
        if (c.channel) kvRow(a, "", "ch %d  %+.1f kHz  %.0f dB  %s%s%s", c.channel, c.offsetHz / 1e3, (double)c.snrDb, st, c.bps ? "  " : "", c.bps ? std::to_string(c.bps).c_str() : "");
        else kvRow(a, "", "%+.1f kHz  %.0f dB  %s", c.offsetHz / 1e3, (double)c.snrDb, st);
    }
    kvRow(a, tr("noise"), "%.1f dBFS in one %.0f Hz bin", (double)t.noiseDb, t.binHz);
    kvRow(a, tr("input"), "%.3f Msps, worked at %.0f kHz", t.inputRate / 1e6, t.workRate / 1e3);
    kvRow(a, tr("log"), "%s", t.logging ? t.logPath.c_str() : tr("off"));
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", gHr ? tr("Meteosat high rate data collection platforms: a 2 s carrier, then the message at 1200 bit/s offset QPSK with a convolutional and a Reed-Solomon code and a CRC, on a 3 kHz assignment of 402.2 to 402.435 MHz. The 8-digit hex address names the platform.")
                                  : tr("The GOES and Meteosat data collection system: river gauges, weather stations, tide gauges and buoys send a short report on a 1.5 kHz channel of 401.7 to 402.1 MHz, at 100, 300 or 1200 bps. The 8-digit hex address names the platform. The receiver tries every bit rate and coding on every carrier it finds."));
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Carrier"), !on ? 0 : T(a).carriers.empty() ? 2 : 1); flowNext(10 * gUi);
    lamp(tr("Report"), !on ? 0 : T(a).dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : summaryOf(T(a)).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = gHr ? "Meteosat HRDCP" : "GOES DCS";
    if (live(a)) l2 = summaryOf(T(a));
}

void synthHr(App& a, bool& changed) {
    static const char* names[] = {"Two platforms: a short report and a 2-frame message", "One platform, a short report every 12 s"};
    int& o = a.tune.synth.modeOpt[0];
    if (o < 0 || o > 1) o = 0;
    ImGui::SetNextItemWidth(std::min(330 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##hrsynth", names[o])) {
        for (int i = 0; i < 2; i++) if (ImGui::Selectable(names[i], i == o)) { o = i; changed = true; }
        ImGui::EndCombo();
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The test signal is HRDCP platforms at 402.2945 and 402.3245 MHz; the noise is set as Eb/N0."));
    ImGui::PopTextWrapPos();
}

void synth(App& a, bool& changed) {
    static const char* names[] = {"Three platforms: 100, 300 and 1200 bps", "One platform, 300 bps", "100 bps, and 300 and 1200 bps as NRZ"};
    int& o = a.tune.synth.modeOpt[0];
    if (o < 0 || o > 2) o = 0;
    ImGui::SetNextItemWidth(std::min(330 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##dcssynth", names[o])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], i == o)) { o = i; changed = true; }
        ImGui::EndCombo();
    }
    bool b = a.tune.synth.modeOpt[1] == 1;
    if (ImGui::Checkbox(tr("with a steady carrier on another channel"), &b)) { a.tune.synth.modeOpt[1] = b ? 1 : 0; changed = true; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The test signal is three platforms on channels 60, 100 and 140 that each send a short report every 14 to 18 seconds."));
    ImGui::PopTextWrapPos();
}

} // namespace

extern const ModeUi kGoesDcsUi;
const ModeUi kGoesDcsUi = {
    .sideTitle = "REPORTS",
    .tabName = "Platforms",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
};

// Meteosat HRDCP (family 700): the same screens with gHr set
namespace {
void tabH(App& a) { gHr = true; tab(a); gHr = false; }
void receiverH(App& a) { gHr = true; receiver(a); gHr = false; }
void listH(App& a) { gHr = true; list(a); gHr = false; }
void statusH(App& a) { gHr = true; status(a); gHr = false; }
void summaryH(const App& a, std::string& l1, std::string& l2) { gHr = true; summary(a, l1, l2); gHr = false; }
void tunerH(App& a, bool& r) { gHr = true; tuner(a, r); gHr = false; }
void synthH(App& a, bool& c) { gHr = true; synthHr(a, c); gHr = false; }
} // namespace

extern const ModeUi kHrdcpUi;
const ModeUi kHrdcpUi = {
    .sideTitle = "MESSAGES",
    .tabName = "Platforms",
    .tabIcon = Ic::Signal,
    .tab = tabH,
    .receiver = receiverH,
    .list = listH,
    .status = statusH,
    .summary = summaryH,
    .tuner = tunerH,
    .synth = synthH,
    .tick = tick,
};

#undef S
