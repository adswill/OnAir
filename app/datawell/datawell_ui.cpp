// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell Waverider buoy HF link screens: the FSK carrier (spectrum, width, tone shift, symbol clock); for the 2FSK HXV format (DWR-MkIII, DWR-G, WR-SG)
// the decoded displacements, sea state words (wave height and period, temperatures, position) and the directional spectrum, from the Datawell Waverider
// Reference Manual; for the 4FSK HVA format only measurements (its over-the-air coding is not public). Receive only.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "dect2/datawell/datawell_rx.h"
#include "dect2/datawell/datawell_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 link, 1 frames, 2 notes
    bool loaded = false;
    int fmt = 0;                         // 0 automatic, 1 2FSK, 2 4MFSK
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.fmt = std::max(0, std::min(2, (int)plat::prefs().getI("dwFmt", 0)));
    // applied by the tick: the engine may not be running yet
}

bool live(const App& a) { return a.rx.standard == 75; }   // the engine reports its standard code minus one

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: Datawell Waverider wave buoys send their data to a shore receiver on HF. The 2FSK format (DWR-MkIII, DWR-G, WR-SG) is decoded from the "
                        "published Datawell reference manual: displacements, wave height and period, temperatures, position, directional spectrum. The 4FSK format (DWR4 with the "
                        "acoustic current meter) is not: its over-the-air coding is not public, so only the carrier, its tones and a few bit-stream measurements are shown. "
                        "Know your local rules before receiving: the buoy channels sit near the 10 m amateur band (28.0 - 29.7 MHz).");
    ImGui::PopTextWrapPos();
}

void plotSpectrum(const dect2::DatawellTelemetry& t) {
    const float W = std::max(200.f, ImGui::GetContentRegionAvail().x), H = 90.f * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), IM_COL32(16, 22, 30, 255));
    const float cw = W / (float)dect2::kDwPsd;
    for (size_t c = 0; c < t.psd.size(); c++) {
        const float v = t.psd[c] / 255.f;
        if (v < 0.02f) continue;
        dl->AddRectFilled(ImVec2(p0.x + c * cw, p0.y + H * (1 - v)), ImVec2(p0.x + (c + 1) * cw + 0.5f, p0.y + H), IM_COL32(110, 190, 240, 255));
    }
    ImGui::Dummy(ImVec2(W, H));
    if (ImGui::IsItemHovered()) {
        const ImVec2 mp = ImGui::GetMousePos();
        ImGui::SetTooltip("%+.0f Hz from the signal centre", ((mp.x - p0.x) / W - 0.5) * t.psdSpanHz);
    }
    ImGui::TextDisabled("%+.0f Hz", -t.psdSpanHz / 2); ImGui::SameLine(W / 2 - 25 * gUi); ImGui::TextDisabled("signal centre"); ImGui::SameLine(W - 60 * gUi); ImGui::TextDisabled("%+.0f Hz", t.psdSpanHz / 2);
}

// the mean frequency of every symbol, in order: the tones show as 2 or 4 levels
void plotSymbols(const dect2::DatawellTelemetry& t) {
    const float W = std::max(200.f, ImGui::GetContentRegionAvail().x), H = 90.f * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), IM_COL32(16, 22, 30, 255));
    const double span = 2.0 * std::max(120.0, std::min(400.0, t.stepHz * 3.0));
    dl->AddLine(ImVec2(p0.x, p0.y + H / 2), ImVec2(p0.x + W, p0.y + H / 2), IM_COL32(70, 80, 90, 255));
    const size_t n = t.symMeans.size();
    for (size_t i = 0; i < n; i++) {
        const float x = p0.x + W * (float)i / (float)dect2::kDwSymMax;
        const float y = p0.y + H * (0.5f - (float)(t.symMeans[i] / span));
        if (y < p0.y || y > p0.y + H) continue;
        dl->AddCircleFilled(ImVec2(x, y), 1.8f * gUi, IM_COL32(255, 210, 90, 255));
    }
    ImGui::Dummy(ImVec2(W, H));
    ImGui::TextDisabled("mean tone of each symbol (Hz about the centre, +-%.0f Hz); %s tone levels expected", span / 2, t.format == "4MFSK" ? "four" : "two");
}

void linkView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to look for a buoy FSK link"); return; }
    const dect2::DatawellTelemetry& t = a.rx.datawell;
    if (ImGui::Button("Clear")) a.engine.datawell().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget the bits and frames found so far");
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%s", dect2::datawellSummary(t).c_str());
    plotSpectrum(t);
    ImGui::Spacing();
    if (t.state == 0) { ImGui::TextDisabled("no FSK-like signal heard yet"); return; }
    kvRow(a, "format guess", "%s%s (from the width at 90%% of the power: narrower than about 205 Hz is 2FSK, wider 4FSK; a guess, not an identification)", t.format.c_str(), t.formatForced ? ", set by hand" : "");
    kvRow(a, "centre", "%+.1f Hz from the tuned frequency%s%s", t.offsetHz, t.freqMhz > 0 ? ", " : "", t.freqMhz > 0 ? (std::to_string(t.freqMhz).substr(0, 9) + " MHz").c_str() : "");
    if (!t.channel.empty()) kvRow(a, "listed channel", "%s", t.channel.c_str());
    kvRow(a, "width / level", "%.0f Hz (90%% of the power), %.1f dB in 100 Hz, peak %.1f dB over the floor", t.bwHz, t.snrDb, t.peakDb);
    if (t.state < 2) { ImGui::TextDisabled("symbols not locked yet%s", t.format == "carrier / narrow tone" ? " (a steady carrier or a narrow tone is not FSK data)" : ""); return; }
    kvRow(a, "symbol clock", "%.3f baud (the buoys send %.2f; %+.0f ppm), %llu symbols", t.baud, dect2::datawell::kBaud, t.clockPpm, (unsigned long long)t.symbols);
    kvRow(a, t.format == "4MFSK" ? "tone spacing" : "tone shift", "%.1f Hz measured (sigidwiki recordings: about %.0f Hz), symbol error %.2f of the spacing", t.stepHz, t.format == "4MFSK" ? dect2::datawell::kSpacing4Mfsk : dect2::datawell::kShift2Fsk, t.symErr);
    if (t.format == "2FSK") {
        if (t.hxvLocked) kvRow(a, "HXV vectors", "%llu (%llu clean, %llu repaired by the parity code, %llu lost), %llu blocks%s", (unsigned long long)t.hxvVectors, (unsigned long long)t.hxvClean, (unsigned long long)t.hxvRepaired, (unsigned long long)t.hxvLost, (unsigned long long)t.hxvBlocks, t.hxvInverted ? ", tones inverted" : "");
        else kvRow(a, "HXV vectors", "looking for the 64-bit vector alignment (three clean vectors in a row, about 2.5 s of good signal)");
    }
    ImGui::Spacing();
    plotSymbols(t);
    ImGui::Spacing();
    ImGui::TextDisabled("Bit stream (higher tone = 1; 4FSK: the tone number as two bits, natural binary: assumptions)");
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.bits.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to look for frames"); return; }
    const dect2::DatawellTelemetry& t = a.rx.datawell;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("For the 4FSK (HVA) format, whose over-the-air coding is not public: frames are only cut when the bit stream repeats with a fixed period and a constant start pattern. "
                        "The pattern, the period and the CRC attempt are measured from the bits, not from a specification. (The 2FSK HXV format is decoded on the Waves and Sea state tabs.)");
    ImGui::PopTextWrapPos();
    if (t.periodBits == 0) { ImGui::TextDisabled("%s", t.state < 2 ? "symbols are not locked" : "no repeating period found in the bits yet (needs a few thousand bits)"); return; }
    kvRow(a, "period", "%d bits (%.2f s), %.0f standard deviations over chance", t.periodBits, t.periodSec, t.periodZ);
    if (t.syncBits) kvRow(a, "start pattern", "%s (%d bits, constant in every frame found)", t.syncHex.c_str(), t.syncBits);
    else kvRow(a, "start pattern", "none found: the frame boundary is unknown, so no frames are cut and no CRC is tried");
    if (!t.crcName.empty()) kvRow(a, "CRC attempt", "%s accepts %d of %d frames (%s bit order, %s polarity) - unverified", t.crcName.c_str(), t.crcPass, t.crcTotal, t.crcLsbFirst ? "LSB first" : "MSB first", t.crcInverted ? "inverted" : "as received");
    else if (t.syncBits) kvRow(a, "CRC attempt", "none of the 16 / 32 bit CRCs tried fits (CCITT, XMODEM, Kermit, X-25, ARC, Modbus and others)");
    kvRow(a, "frames", "%llu accepted, %llu not", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& f : t.frames) txt += clock(f.wall) + "\t" + f.hex + (f.crcOk ? "\tcrc-attempt-ok\n" : "\n");
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##dwfr", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("CRC attempt"); ImGui::TableSetupColumn("Frame (hex)", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& f : t.frames) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(f.wall).c_str());
        ImGui::TableNextColumn(); if (t.crcName.empty()) ImGui::TextDisabled("-"); else if (f.crcOk) ImGui::TextColored(pal::okGreen(), "ok"); else ImGui::TextDisabled("fails");
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(f.hex.c_str()); ImGui::PopFont();
    }
    ImGui::EndTable();
}

void plotLines(const std::vector<dect2::DwDisp>& d, float H) {
    const float W = std::max(200.f, ImGui::GetContentRegionAvail().x);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), IM_COL32(16, 22, 30, 255));
    int mx = 50;
    for (const auto& v : d) mx = std::max(mx, std::max(std::abs(v.h), std::max(std::abs(v.n), v.wrsg ? 0 : std::abs(v.w))));
    mx = (mx + 24) / 25 * 25;
    const float y0 = p0.y + H / 2;
    dl->AddLine(ImVec2(p0.x, y0), ImVec2(p0.x + W, y0), IM_COL32(70, 80, 90, 255));
    const ImU32 col[3] = {IM_COL32(255, 210, 90, 255), IM_COL32(110, 190, 240, 255), IM_COL32(150, 220, 150, 255)};
    for (int ch = 0; ch < 3; ch++) {
        ImVec2 prev(0, 0);
        for (size_t i = 0; i < d.size(); i++) {
            const int val = ch == 0 ? d[i].h : ch == 1 ? d[i].n : d[i].w;
            if (ch == 2 && d[i].wrsg) { prev = ImVec2(0, 0); continue; }
            const ImVec2 pt(p0.x + W * (float)(i + 1) / 128.f, y0 - (float)val / (float)mx * (H / 2 - 4));
            if (prev.x > 0) dl->AddLine(prev, pt, col[ch], 1.5f * gUi);
            prev = pt;
        }
    }
    ImGui::Dummy(ImVec2(W, H));
    ImGui::TextColored(ImVec4(1.f, 0.82f, 0.35f, 1), "heave"); ImGui::SameLine(0, 10 * gUi);
    ImGui::TextColored(ImVec4(0.43f, 0.75f, 0.94f, 1), "north"); ImGui::SameLine(0, 10 * gUi);
    ImGui::TextColored(ImVec4(0.6f, 0.86f, 0.6f, 1), "west"); ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("+-%d cm, one sample every 0.78 s (WR-SG: the two vertical samples are drawn as heave and north)", mx);
}

void wavesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to look for a buoy"); return; }
    const dect2::DatawellTelemetry& t = a.rx.datawell;
    if (t.format != "2FSK") { ImGui::TextDisabled("Displacements are decoded for the 2FSK (HXV) format only. The 4FSK (HVA) over-the-air coding is not public: see the Frames and Notes tabs."); return; }
    if (!t.hxvLocked || t.disp.empty()) { ImGui::TextDisabled("looking for the HXV vectors (about 2.5 s of good signal)"); return; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Heave, north and west displacements of the buoy: 12-bit sign-magnitude centimetres in 64-bit vectors at 1.28 Hz (Datawell DWR-MkIII reference manual, section 5.7.1.1). "
                        "Vectors the parity code repaired are drawn too; a vector with three or more bit errors is dropped or, about half of the time, repaired to something wrong, so isolated spikes can be repairs.");
    ImGui::PopTextWrapPos();
    const dect2::DwDisp& l = t.disp.back();
    if (l.wrsg) ImGui::Text("last vector: vertical %d cm and %d cm (WR-SG: two vertical samples, no horizontal data)", l.h, l.n);
    else ImGui::Text("last vector: heave %d cm, north %d cm, west %d cm", l.h, l.n, l.w);
    plotLines(t.disp, 160.f * gUi);
}

void seaView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to look for a buoy"); return; }
    const dect2::DatawellTelemetry& t = a.rx.datawell;
    if (t.format != "2FSK" || !t.hxvLocked) { ImGui::TextDisabled("Sea state numbers come with the 2FSK (HXV) format: one system word per 14 s block, 16 words (225 s) for a full set."); return; }
    const dect2::DwSystem& s = t.sys;
    int got = 0;
    for (int i = 0; i < 16; i++) got += s.have[i] ? 1 : 0;
    ImGui::TextDisabled("System file words %d of 16 received, spectrum points %d of 64 (the buoy repeats the set 8 times in 30 minutes).", got, t.specCount);
    if (s.have[1]) kvRow(a, "wave height", "Hrms %.3f m, Hs %.2f m (4 Hrms)", s.hrms, s.hs);
    if (s.have[2]) kvRow(a, "zero-crossing period", "Tz %.1f s", s.tz);
    if (s.have[3]) kvRow(a, "peak density", "%.2f m^2/Hz", s.psdMax);
    if (s.have[5]) kvRow(a, "water temperature", "%.2f C", s.tw);
    if (s.have[4]) kvRow(a, "internal reference", "%.2f C (the buoy's own check: 25 C when healthy)", s.tr);
    if (s.have[10] && s.have[11] && s.have[12] && s.have[13]) kvRow(a, "position", "%.5f %s, %.5f %s%s", std::fabs(s.lat), s.lat < 0 ? "S" : "N", std::fabs(s.lon), s.lon < 0 ? "W" : "E", s.have[0] && !s.gpsFix ? " (no current GPS fix)" : "");
    if (s.have[6]) kvRow(a, "battery / life", "status %d, %d weeks of operational life left", s.battery & 7, s.tolWeeks);
    if (s.have[0]) kvRow(a, "transmission", "number %d of 8, GPS module %s", s.tn, s.gpsModuleOk ? "ok" : "not ok");
    if (s.have[14]) kvRow(a, "orientation", "%.1f deg", s.orientDeg);
    if (s.have[15]) kvRow(a, "field inclination", "%.1f deg", s.inclDeg);
    if (s.have[7]) kvRow(a, "accelerometer offset", "vertical %.3f m/s^2", s.av0);
    ImGui::Spacing();
    if (t.specCount == 0) { ImGui::TextDisabled("no spectrum points yet (a block carries 4 of the 64)"); return; }
    if (ImGui::Button("Copy spectrum")) {
        std::string txt = "f_Hz\tPSD_m2_per_Hz\trel_PSD\tdir_deg\tspread_deg\tm2\tn2\tK\n";
        for (int i = 0; i < 64; i++) if (t.spec[i].valid) { char b[200]; snprintf(b, sizeof b, "%.3f\t%.5f\t%.4f\t%.1f\t%.1f\t%.3f\t%.3f\t%.2f\n", t.spec[i].hz, t.spec[i].psd, t.spec[i].rpsd, t.spec[i].dirDeg, t.spec[i].spreadDeg, t.spec[i].m2, t.spec[i].n2, t.spec[i].k); txt += b; }
        ImGui::SetClipboardText(txt.c_str());
    }
    const float W = std::max(200.f, ImGui::GetContentRegionAvail().x), H = 100.f * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), IM_COL32(16, 22, 30, 255));
    double mx = 1e-9;
    for (int i = 0; i < 64; i++) if (t.spec[i].valid) mx = std::max(mx, s.have[3] ? t.spec[i].psd : t.spec[i].rpsd);
    for (int i = 0; i < 64; i++) {
        if (!t.spec[i].valid) continue;
        const double v = (s.have[3] ? t.spec[i].psd : t.spec[i].rpsd) / mx;
        const float x = p0.x + W * (float)i / 64.f;
        dl->AddRectFilled(ImVec2(x, p0.y + H * (1 - (float)v)), ImVec2(x + W / 64.f - 1, p0.y + H), IM_COL32(110, 190, 240, 255));
    }
    ImGui::Dummy(ImVec2(W, H));
    ImGui::TextDisabled("power spectral density against frequency, 0.025 to 0.635 Hz (%s)", s.have[3] ? "m^2/Hz" : "relative: the peak density word has not arrived yet");
    if (!ImGui::BeginTable("##dwsp", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Hz"); ImGui::TableSetupColumn("PSD m2/Hz"); ImGui::TableSetupColumn("Direction"); ImGui::TableSetupColumn("Spread"); ImGui::TableSetupColumn("m2");
    ImGui::TableSetupColumn("n2"); ImGui::TableSetupColumn("K"); ImGui::TableSetupColumn("Relative", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int i = 0; i < 64; i++) {
        const dect2::DwSpec& p = t.spec[i];
        if (!p.valid) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%.3f", p.hz);
        ImGui::TableNextColumn(); if (s.have[3]) ImGui::Text("%.4f", p.psd); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%.1f deg", p.dirDeg);
        ImGui::TableNextColumn(); ImGui::Text("%.1f deg", p.spreadDeg);
        ImGui::TableNextColumn(); ImGui::Text("%.2f", p.m2);
        ImGui::TableNextColumn(); ImGui::Text("%.2f", p.n2);
        ImGui::TableNextColumn(); ImGui::Text("%.2f", p.k);
        ImGui::TableNextColumn(); ImGui::Text("%.4f", p.rpsd);
    }
    ImGui::EndTable();
}

void notesView() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextWrapped("Sources and what each part rests on:");
    ImGui::BulletText("Channels, 2FSK and 4FSK: sigidwiki.com/wiki/Datawell_Buoy_HF_Link and its audio samples. Listed channels 25.5 - 35.5 MHz (45 MHz on request), received as USB, about 50 km range.");
    ImGui::BulletText("81.92 symbols per second: 2FSK is 81.92 bit/s, 4FSK 163.84 bit/s (Datawell RX-C4 manual, specifications). Tone distances measured on the sigidwiki recordings: 2FSK about 129 Hz apart, 4FSK about 78 Hz apart.");
    ImGui::BulletText("2FSK (HXV: DWR-MkIII, DWR-G, WR-SG): 64-bit vectors every 0.78 s: 16-bit cyclic word, heave, north and west in cm (12-bit sign-magnitude), 12 parity bits. 18 vectors make a block "
                      "(sync word 0x7FFF, one system file word, 16 spectrum words), 16 blocks a full set. Datawell Waverider Reference Manual (DWR-MkIII), section 5.7; the RX-C4 manual's HXV examples checked the vectors.");
    ImGui::BulletText("The parity code is not in the manual (it only says a Galois code table encodes 63 bits). It was found from the sigidwiki recording and the RX-C4 examples: BCH(63,51), generator 12471 octal, "
                      "over the 63 bits of the vector without the least significant heave bit, repairing up to 2 bit errors. All 27 vectors checked agree. A finding, not a published fact.");
    ImGui::BulletText("A buoy sending the other format than its receiver expects shows as a wrong-format signal: use the format choice in the tuning bar to force one.");
    ImGui::Spacing();
    ImGui::TextWrapped("Not decoded: the 4FSK (HVA) format. Datawell's transmission protocol specification (DWTP) describes it only after the receiver's error correction (a BCH / Reed-Solomon concatenation "
                       "that is not published). The sigidwiki 4FSK recording does show a repeating 128-bit frame (one vector per 0.78 s): that, and the bits themselves, is all that is shown, on the Frames tab.");
    ImGui::Spacing();
    ImGui::TextWrapped("Checked on the sigidwiki 2FSK recording (20 s: sync word, system word 4 = 25.0 C reference temperature, spectrum points, smooth displacements, every parity check clean) and on the "
                       "29.525 MHz buoy recording (a WR-SG: vertical samples and the 0xAAA dummy decode, but the signal is noisy and vectors are lost). Everything else on synthetic signals built from the documented layout.");
    ImGui::Spacing();
    ImGui::TextWrapped("Legal note: receive only, nothing is transmitted. Datawell manuals say a transmit licence is needed in territorial waters and that 28.0 - 29.7 MHz is reserved for amateur radio; "
                       "whether and how you may receive and keep this telemetry depends on your country.");
    ImGui::PopTextWrapPos();
}

void tab(App& a) {
    subNav("dwv", S.view, {"Link", "Waves", "Sea state", "Frames", "Notes"});
    if (S.view == 0) linkView(a); else if (S.view == 1) wavesView(a); else if (S.view == 2) seaView(a); else if (S.view == 3) framesView(a); else notesView();
}

void list(App& a) {
    ImGui::TextDisabled("Listed buoy channels (click to tune)");
    int n = 0;
    const dect2::datawell::Channel* ch = dect2::datawell::channels(n);
    if (!ImGui::BeginTable("##dwch", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("MHz");
    for (int i = 0; i < n; i++) {
        ImGui::TableNextRow();
        ImGui::PushID(i);
        ImGui::TableNextColumn();
        if (ImGui::Selectable(ch[i].name, std::fabs(a.freqMhz - ch[i].mhz) < 0.0005, ImGuiSelectableFlags_SpanAllColumns)) {
            a.freqMhz = ch[i].mhz;
            a.tune.centerHz = ch[i].mhz * 1e6;
            a.engine.retune(a.tune);
        }
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.4f", ch[i].mhz);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::DatawellTelemetry& t = a.rx.datawell;
    kvRow(a, "state", "%s", dect2::datawellSummary(t).c_str());
    if (t.centerMhz > 0) kvRow(a, "tuned", "%.4f MHz", t.centerMhz);
    kvRow(a, "input", "%.0f ksps, %.1f dBFS", t.inputRate / 1e3, t.levelDb);
    kvRow(a, "frames", "%llu accepted by the CRC attempt, %llu not", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    receiveNote();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("FSK", !on ? 0 : a.rx.datawell.state >= 1 ? 1 : 0); flowNext(6 * gUi);
    lamp("Symbols", !on ? 0 : a.rx.datawell.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::datawellSummary(a.rx.datawell).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Datawell Buoy HF Link";
    if (live(a)) l2 = dect2::datawellSummary(a.rx.datawell);
}

void tick(App& a) {
    loadState();
    if (a.engine.running()) a.engine.datawell().setFormat(S.fmt);
}

void tuner(App& a, bool& retune) {
    loadState();
    int n = 0;
    const dect2::datawell::Channel* ch = dect2::datawell::channels(n);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Channel");
    ImGui::SameLine(0, 5 * gUi);
    std::string cur = dect2::datawell::channelNear(a.freqMhz, 1);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##dwch", cur.empty() ? "other" : cur.c_str())) {
        for (int i = 0; i < n; i++) if (ImGui::Selectable(ch[i].name, cur == ch[i].name)) { tuneFreq(a, ch[i].mhz); retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Buoy channels listed on sigidwiki (the buoy's own channel is set by its owner: others are possible)");
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Format");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
    static const char* names[] = {"automatic", "2FSK (HXV)", "4MFSK (HVA)"};
    if (ImGui::BeginCombo("##dwfmt", names[S.fmt])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], S.fmt == i)) { S.fmt = i; plat::prefs().setI("dwFmt", i); plat::prefs().flush(); a.engine.datawell().setFormat(i); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatic tells 2FSK from 4MFSK by the width of the signal. Choose one when a buoy sends the other format than the one it should.");
    flowEnd();
}

} // namespace

extern const ModeUi kDatawellUi;
const ModeUi kDatawellUi = {
    .sideTitle = "CHANNELS",
    .tabName = "Buoy",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
