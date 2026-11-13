// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tools > Antenna designer > Calculators: wavelength and wire lengths, cable loss, SWR / return loss, matching networks (L network, stub,
// quarter-wave, gamma, balun / choke), line transforms, series / parallel, EIRP / ERP, antenna factor, band presets. The formulas are in
// core (dect2/antd/antd_tools.h, docs/ANTENNA.md); this file only lays them out. And the polar plot used by the pattern cuts.
#include "antd_ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace antdui {

namespace ad = dect2::antd;

void polarPlot(const char* id, const std::vector<double>& g, double step, bool full, bool compass, float size, const char* title, double mark) {
    ImGui::PushID(id);
    ImGui::BeginGroup();
    if (title && *title) ImGui::TextUnformatted(title);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = size, h = full ? size : size * 0.56f;
    ImGui::InvisibleButton("##polar", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + w / 2, full ? p0.y + h / 2 : p0.y + h - 6 * gUi);
    const float R = (full ? w / 2 : std::min(w / 2, h - 10 * gUi)) - 14 * gUi;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 4 * gUi);
    double gmax = -999;
    for (double v : g) gmax = std::max(gmax, v);
    const double range = 40;   // dB shown below the maximum
    auto rad = [&](double v) { return (float)(R * std::max(0.0, (v - gmax + range) / range)); };
    // angle a (deg) to a screen direction
    auto dir = [&](double a) {
        const double r = a * dect2::antd::kPi / 180;
        if (compass) return ImVec2((float)std::sin(r), (float)-std::cos(r));
        return ImVec2((float)std::cos(r), (float)-std::sin(r));
    };
    const ImU32 grid = ImGui::GetColorU32(ImVec4(0.6f, 0.62f, 0.64f, 0.18f)), txt = ImGui::GetColorU32(ImVec4(0.62f, 0.64f, 0.66f, 1));
    for (double db : {0.0, -3.0, -10.0, -20.0, -30.0}) {
        const float r = (float)(R * (db + range) / range);
        if (full) dl->AddCircle(c, r, grid, 72);
        else { dl->PathArcTo(c, r, (float)dect2::antd::kPi, 2 * (float)dect2::antd::kPi, 48); dl->PathStroke(grid, 0, 1); }
        if (db < 0) { char b[16]; std::snprintf(b, sizeof b, "%g", db); dl->AddText(ImVec2(c.x + 2, c.y - r - ImGui::GetFontSize() * 0.5f), txt, b); }
    }
    for (int a = 0; a < (full ? 360 : 181); a += 30) {
        const ImVec2 d = dir(a);
        dl->AddLine(c, ImVec2(c.x + d.x * R, c.y + d.y * R), grid);
        char b[8]; std::snprintf(b, sizeof b, "%d", a);
        const ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(c.x + d.x * (R + 8 * gUi) - ts.x / 2, c.y + d.y * (R + 8 * gUi) - ts.y / 2), txt, b);
    }
    if (!g.empty() && gmax > -998) {
        const ImU32 col = ImGui::GetColorU32(pal::accent());
        std::vector<ImVec2> pts;
        for (size_t i = 0; i < g.size(); ++i) {
            const ImVec2 d = dir(i * step);
            const float r = rad(g[i]);
            pts.push_back(ImVec2(c.x + d.x * r, c.y + d.y * r));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.8f * gUi);
        if (mark >= 0) {
            const ImVec2 d = dir(mark);
            ImVec4 mc = pal::warnAmber(); mc.w = 0.7f;
            dl->AddLine(c, ImVec2(c.x + d.x * R, c.y + d.y * R), ImGui::GetColorU32(mc), 1);
        }
        char b[48]; std::snprintf(b, sizeof b, "max %.2f dBi", gmax);
        dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y + 2 * gUi), txt, b);
        if (hov) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            double a = compass ? std::atan2(m.x - c.x, -(m.y - c.y)) : std::atan2(-(m.y - c.y), m.x - c.x);
            a *= 180 / dect2::antd::kPi;
            if (a < 0) a += 360;
            const int i = (int)std::lround(a / step);
            if (i >= 0 && i < (int)g.size()) ImGui::SetTooltip("%.0f deg: %.2f dBi (%.2f dB below the maximum)", i * step, g[i], gmax - g[i]);
        }
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

namespace {

struct Calc {
    bool init = false;
    double f = 14.2, vf = 0.66;
    int cable = 1; double cableLen = 20, cableSwr = 1.5, cableP = 100;
    double swr = 1.5, rl = 14, zr = 75, zx = 0, z0 = 50;
    double lr = 200, lx = 0, lz0 = 50;
    int stubOpen = 0;
    double qrl = 100;
    double elemD = 0.012;
    double balHi = 2450, balLo = 50, chokeOhm = 1000;
    double tR = 72, tX = 0, tZ0 = 50, tLen = 5, tVf = 0.66, tLoss = 0;
    double sR = 10, sX = 20;
    double txW = 100, lossDb = 1.5, gDbi = 2.15;
    double afG = 2.15, ap = 1;
};
Calc C;

bool num(const char* label, double* v, const char* fmt = "%.4g", double step = 0) {
    ImGui::SetNextItemWidth(140 * gUi);
    return ImGui::InputDouble(label, v, step, step * 10, fmt);
}

} // namespace

void calculatorsTab(double fMHz, double z0, const Units& u, double* setF0, double* setF1, bool* picked) {
    if (!C.init) { C.init = true; C.f = fMHz; C.z0 = z0; C.lz0 = z0; C.tZ0 = z0; }
    if (ImGui::SmallButton("Use the designer's frequency")) { C.f = fMHz; C.z0 = z0; }
    ImGui::SameLine();
    num("MHz##cf", &C.f, "%.4f");
    C.f = std::max(C.f, 1e-3);
    const double fHz = C.f * 1e6;
    char b[160];
    if (ImGui::CollapsingHeader("Wavelength and wire lengths", ImGuiTreeNodeFlags_DefaultOpen)) {
        num("velocity factor##wl", &C.vf, "%.3f");
        C.vf = std::min(1.0, std::max(0.05, C.vf));
        const double lam = ad::wavelength(fHz), lamv = ad::wavelength(fHz, C.vf);
        ImGui::Text("Wavelength %.4f %s  (in the line %.4f %s);  half %.4f, quarter %.4f %s", u.len(lam), u.lenUnit(), u.len(lamv), u.lenUnit(),
                    u.len(lam / 2), u.len(lam / 4), u.lenUnit());
        const ad::WireLengths w = ad::wireLengths(fHz);
        ImGui::Text("Half-wave dipole %.3f %s total (142.65 / f), each leg %.3f", u.len(w.halfDipole), u.lenUnit(), u.len(w.halfDipole / 2));
        ImGui::Text("Inverted-V %.3f %s total;  quarter-wave vertical %.3f %s (71.3 / f)", u.len(w.inverted_v), u.lenUnit(), u.len(w.quarterVertical), u.lenUnit());
        ImGui::Text("Full-wave loop %.3f %s (306.3 / f);  EFHW %.3f %s;  5/8 vertical %.3f %s", u.len(w.fullLoop), u.lenUnit(), u.len(w.efhw), u.lenUnit(),
                    u.len(w.fiveEighths), u.lenUnit());
        ImGui::Text("Quarter-wave line (velocity factor %.2f): %.3f %s;  half-wave %.3f %s", C.vf, u.len(lamv / 4), u.lenUnit(), u.len(lamv / 2), u.lenUnit());
    }
    if (ImGui::CollapsingHeader("Cable loss")) {
        const auto& cs = ad::cables();
        C.cable = std::min(std::max(C.cable, 0), (int)cs.size() - 1);
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo("cable", cs[C.cable].name)) {
            for (int i = 0; i < (int)cs.size(); ++i) if (ImGui::Selectable(cs[i].name, i == C.cable)) C.cable = i;
            ImGui::EndCombo();
        }
        double len = u.len(C.cableLen);
        std::snprintf(b, sizeof b, "length (%s)##cl", u.lenUnit());
        if (num(b, &len, "%.2f")) C.cableLen = std::max(0.0, u.lenIn(len));
        num("SWR at the antenna##cs", &C.cableSwr, "%.2f"); C.cableSwr = std::max(1.0, C.cableSwr);
        num("power in (W)##cp", &C.cableP, "%.1f");
        const ad::Cable& c = cs[C.cable];
        const double a = ad::cableLossDb(c, fHz, C.cableLen);
        const ad::LineLoss ll = ad::mismatchedLineLoss(a, C.cableSwr);
        ImGui::Text("%s: %.0f ohm, velocity factor %.2f%s%s", c.name, c.z0, c.vf, *c.note ? ", " : "", c.note);
        ImGui::Text("Matched loss %.2f dB (%.2f dB / 100 m); with the mismatch %.2f dB", a, ad::cableLossDb(c, fHz, 100), ll.totalDb);
        ImGui::Text("SWR at the radio %.2f;  power at the antenna %.1f W of %.1f W", ll.swrIn, C.cableP * std::pow(10.0, -ll.totalDb / 10), C.cableP);
        ImGui::TextDisabled("Typical datasheet values, approximate; old or wet cable is worse.");
    }
    if (ImGui::CollapsingHeader("SWR, return loss, mismatch")) {
        bool ch = false;
        ad::Mismatch m{};
        if (num("SWR##sw", &C.swr, "%.3f")) { C.swr = std::max(1.0, C.swr); ch = true; m = ad::fromSwr(C.swr); C.rl = m.returnLossDb; }
        if (num("return loss (dB)##rl", &C.rl, "%.2f")) { m = ad::fromReturnLoss(C.rl); C.swr = m.swr; ch = true; }
        ImGui::TextUnformatted("or from an impedance:");
        bool zc = num("R##zr", &C.zr, "%.2f"); ImGui::SameLine(); zc |= num("X##zx", &C.zx, "%.2f"); ImGui::SameLine(); zc |= num("Z0##zz", &C.z0, "%.1f");
        if (zc) { m = ad::fromZ(ad::cd(C.zr, C.zx), C.z0); C.swr = m.swr; C.rl = m.returnLossDb; }
        (void)ch;
        m = ad::fromSwr(C.swr);
        ImGui::Text("|Gamma| %.4f;  return loss %.2f dB;  mismatch loss %.3f dB;  reflected power %.2f %%", m.gamma, m.returnLossDb, m.mismatchLossDb, m.reflectedPct);
    }
    if (ImGui::CollapsingHeader("Matching: L network")) {
        num("load R##lr", &C.lr, "%.2f"); ImGui::SameLine(); num("load X##lx", &C.lx, "%.2f"); ImGui::SameLine(); num("to (ohm)##lz", &C.lz0, "%.1f");
        const auto sol = ad::lNetwork(ad::cd(C.lr, C.lx), C.lz0, fHz);
        if (sol.empty()) ImGui::TextDisabled("needs a load resistance above 0");
        int k = 1;
        for (const auto& s : sol) {
            ImGui::Text("%d: %s  series %s (X %+.1f ohm), shunt %s (X %+.1f ohm)", k++, s.shuntAtLoad ? "shunt across the load, then" : "series at the load, then shunt at the radio;",
                        s.series.c_str(), s.xSeries, s.shunt.c_str(), s.xShunt);
        }
    }
    if (ImGui::CollapsingHeader("Matching: stub, quarter-wave, gamma, balun")) {
        ImGui::TextUnformatted("Single shunt stub (load above, line Z0 below):");
        num("line Z0##st", &C.tZ0, "%.1f"); ImGui::SameLine(); num("velocity factor##stv", &C.vf, "%.3f");
        ImGui::SameLine(); ImGui::RadioButton("shorted", &C.stubOpen, 0); ImGui::SameLine(); ImGui::RadioButton("open", &C.stubOpen, 1);
        for (const auto& s : ad::stubMatch(ad::cd(C.lr, C.lx), C.tZ0, fHz, C.vf, C.stubOpen == 1))
            ImGui::Text("  stub %.4f lambda (%.3f %s) at %.4f lambda (%.3f %s) from the load", s.lLambda, u.len(s.lM), u.lenUnit(), s.dLambda, u.len(s.dM), u.lenUnit());
        ImGui::Separator();
        num("load R (quarter-wave)##q", &C.qrl, "%.1f");
        const ad::QuarterWave q = ad::quarterWave(C.qrl, C.lz0, fHz, C.vf);
        ImGui::Text("Quarter-wave transformer: %.1f ohm line, %.3f %s long (velocity factor %.2f)", q.z, u.len(q.lengthM), u.lenUnit(), C.vf);
        ImGui::Separator();
        double d = u.rad(C.elemD);
        std::snprintf(b, sizeof b, "element diameter (%s)##gd", u.radUnit());
        if (num(b, &d, "%.2f")) C.elemD = std::max(1e-4, u.radIn(d));
        const ad::Gamma g = ad::gammaMatch(fHz, C.elemD);
        ImGui::Text("Gamma match start: rod %.3f %s, spacing %.3f %s, rod diameter %.2f %s, capacitor about %.0f pF (trim on the antenna)",
                    u.len(g.rodLengthM), u.lenUnit(), u.len(g.rodSpacingM), u.lenUnit(), u.rad(C.elemD * g.rodDiameterRatio), u.radUnit(), g.capPf);
        ImGui::Separator();
        num("high Z##bh", &C.balHi, "%.0f"); ImGui::SameLine(); num("low Z##bl", &C.balLo, "%.0f"); ImGui::SameLine(); num("choke ohm##bc", &C.chokeOhm, "%.0f");
        const ad::Choke ck = ad::balunChoke(fHz, C.balHi, C.balLo, C.chokeOhm);
        ImGui::Text("Transformer %.0f:%.0f = %.2f:1 impedance, turns ratio %.2f:1;  a %.0f-ohm choke needs %.2f uH at this frequency",
                    C.balHi, C.balLo, C.balLo > 0 ? C.balHi / C.balLo : 0.0, ck.turnsRatio, C.chokeOhm, ck.inductanceH * 1e6);
    }
    if (ImGui::CollapsingHeader("Impedance through a line, series / parallel")) {
        num("load R##tr", &C.tR, "%.2f"); ImGui::SameLine(); num("load X##tx", &C.tX, "%.2f");
        num("Z0##tz", &C.tZ0, "%.1f"); ImGui::SameLine();
        double len = u.len(C.tLen);
        std::snprintf(b, sizeof b, "length (%s)##tl", u.lenUnit());
        if (num(b, &len, "%.3f")) C.tLen = std::max(0.0, u.lenIn(len));
        num("velocity factor##tv", &C.tVf, "%.3f"); ImGui::SameLine(); num("loss (dB/100 m)##tlo", &C.tLoss, "%.2f");
        const ad::cd zin = ad::lineTransform(ad::cd(C.tR, C.tX), C.tZ0, C.tLen, fHz, std::max(0.05, C.tVf), C.tLoss / 100);
        ImGui::Text("Input impedance %.2f %+.2fj ohm (SWR %.2f on the line's Z0); electrical length %.1f deg", zin.real(), zin.imag(),
                    ad::vswr(ad::cd(C.tR, C.tX), C.tZ0), 360 * C.tLen / ad::wavelength(fHz, std::max(0.05, C.tVf)));
        ImGui::Separator();
        num("R series##sr", &C.sR, "%.2f"); ImGui::SameLine(); num("X series##sx", &C.sX, "%.2f");
        double rp, xp; ad::seriesToParallel(ad::cd(C.sR, C.sX), rp, xp);
        ImGui::Text("Parallel equivalent: %.2f ohm in parallel with %+.2fj ohm (%s)", rp, xp, ad::reactancePart(xp, fHz).c_str());
    }
    if (ImGui::CollapsingHeader("EIRP / ERP, antenna factor")) {
        num("transmitter (W)##tw", &C.txW, "%.2f"); ImGui::SameLine(); num("feeder loss (dB)##fl", &C.lossDb, "%.2f"); ImGui::SameLine(); num("gain (dBi)##gd", &C.gDbi, "%.2f");
        const ad::Eirp e = ad::eirp(C.txW, C.lossDb, C.gDbi);
        ImGui::Text("EIRP %.2f W (%.2f dBm, %.2f dBW);  ERP %.2f W (%.2f dBm)", e.eirpW, e.eirpDbm, e.eirpDbm - 30, e.erpW, e.erpDbm);
        ImGui::Separator();
        num("antenna gain (dBi)##af", &C.afG, "%.2f"); ImGui::SameLine(); num("Z0##afz", &C.z0, "%.1f");
        ImGui::Text("Antenna factor %.2f dB(1/m): field (dBuV/m) = receiver (dBuV) + AF", ad::antennaFactorDb(fHz, C.afG, C.z0));
        num("aperture (m2)##ap", &C.ap, "%.3f");
        ImGui::Text("A dish or horn of that aperture: %.2f dBi (100 %% efficient; about 55 %% is typical: %.2f dBi)", ad::gainFromAperture(C.ap, fHz),
                    ad::gainFromAperture(C.ap * 0.55, fHz));
    }
    if (ImGui::CollapsingHeader("Band presets")) {
        if (ImGui::BeginTable("##bands", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (const auto& bp : ad::bandPresets()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(bp.name, false, ImGuiSelectableFlags_SpanAllColumns) && setF0 && setF1 && picked) {
                    *setF0 = bp.f0 / 1e6; *setF1 = bp.f1 / 1e6; *picked = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sweep this band in the designer");
                ImGui::TableNextColumn(); ImGui::Text("%.4f - %.4f MHz", bp.f0 / 1e6, bp.f1 / 1e6);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", bp.note);
            }
            ImGui::EndTable();
        }
    }
}

} // namespace antdui
