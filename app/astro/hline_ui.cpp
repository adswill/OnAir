// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hydrogen line toolkit screens: velocity in the LSR frame with Gaussian line fits and exports (CSV, FITS, SigMF), the galactic l-v map with the
// terminal velocities and a rotation curve, pointing (Az/El, transits, a 24 h visibility plot), calibration (Y-factor, Tsys, RFI), the log and
// saved sessions.
#include "app.h"
#include "hline_ui.h"
#include "dect2/astro/astro_dsp.h"
#include "dect2/astro/hline.h"
#include "dect2/common/u8file.h"
#include "i18n/i18n.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace {

namespace hl = dect2::astro::hl;
namespace as = dect2::astro;

struct Hs {
    int body = 0;                       // hl::Body index
    float l = 0, b = 0, ra = 0, dec = 0;
    bool kinematic = true, rfi = true, lsrAxis = true, loaded = false;
    float tsys = 100, tHot = 295, tCold = 77, rfiSigma = 5, sky = 10, r0 = 8.5f, v0 = 220;
    double pHot = 0, pCold = 0;
    hl::YFactor yf;
    std::vector<hl::GaussComp> comps;
    double fitRms = 0;
    bool fitOk = false;
    hl::LvMap map;
    std::vector<std::string> log;
    std::string note;
    std::vector<double> lastTa, lastV, lastVlsr;
    std::vector<bool> lastFlag;
    double lastCorr = 0;
};
Hs H;

void say(const std::string& s) {
    char t[16];
    const std::time_t now = std::time(nullptr);
    std::strftime(t, sizeof t, "%H:%M:%S", std::gmtime(&now));
    H.log.push_back(std::string(t) + " UTC  " + s);
    if (H.log.size() > 300) H.log.erase(H.log.begin());
}

hl::Body bodyOf(int i) { return (hl::Body)i; }

hl::Eq targetJ2000() {
    if (H.body == (int)hl::Body::Custom) return {H.ra, H.dec};
    return hl::j2000FromGalactic({H.l, H.b});
}

double nowJd() {
    const std::time_t t = std::time(nullptr);
    return 2440587.5 + (double)t / 86400.0;
}

void site(double& lat, double& lon) { receiverLocation(lat, lon); }

void load() {
    if (H.loaded) return;
    H.loaded = true;
    auto& p = plat::prefs();
    H.tsys = (float)p.getD("hlTsys", 100); H.body = (int)p.getI("hlBody", 0); H.l = (float)p.getD("hlL", 0); H.b = (float)p.getD("hlB", 0);
    H.ra = (float)p.getD("hlRa", 0); H.dec = (float)p.getD("hlDec", 0);
}
void save() {
    auto& p = plat::prefs();
    p.setD("hlTsys", H.tsys); p.setI("hlBody", H.body); p.setD("hlL", H.l); p.setD("hlB", H.b); p.setD("hlRa", H.ra); p.setD("hlDec", H.dec);
}

const char* kBodies[] = {N_("Galactic position"), N_("Sun"), N_("Moon"), N_("Cassiopeia A"), N_("Cygnus A"), N_("Sagittarius A*"), N_("Custom (RA / Dec)")};

std::string utcText(double jd) {
    if (jd <= 0) return "-";
    const hl::Utc u = hl::fromJulianDate(jd);
    char b[48];
    snprintf(b, sizeof b, "%02d-%02d %02d:%02d", u.mo, u.d, u.h, u.mi);
    return b;
}

bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = dect2::u8fopen(path, "wb");
    const bool ok = f && fwrite(data.data(), 1, data.size(), f) == data.size();
    if (f) fclose(f);
    return ok;
}

// the processed spectrum in kelvin on the topocentric and LSR axes, RFI masked
void prepare(const HlineIn& in, double jd) {
    const auto& r = *in.ratio;
    const int n = (int)r.size();
    H.lastTa.assign((size_t)n, 0.0); H.lastV.assign((size_t)n, 0.0); H.lastVlsr.assign((size_t)n, 0.0);
    for (int i = 0; i < n; i++) {
        H.lastTa[(size_t)i] = r[(size_t)i] * H.tsys;
        const double hz = in.centerHz + (i - n / 2) * in.rate / n;
        H.lastV[(size_t)i] = as::velocityKms(hz, in.restHz);
    }
    hl::RfiParams rp; rp.sigma = H.rfiSigma;
    H.lastFlag = H.rfi ? hl::flagRobust(H.lastTa, rp) : std::vector<bool>((size_t)n, false);
    if (H.rfi) H.lastTa = hl::maskInterpolate(H.lastTa, H.lastFlag);
    double lat, lon;
    site(lat, lon);
    const hl::ObserverVelocity o = hl::observerVelocity(targetJ2000(), jd, lat, lon, H.kinematic ? hl::Lsr::Kinematic : hl::Lsr::Dynamical);
    H.lastCorr = o.total;
    for (int i = 0; i < n; i++) H.lastVlsr[(size_t)i] = H.lastV[(size_t)i] + o.total;
}

void plot(const char* id, const std::vector<double>& x, const std::vector<double>& y, float h, const char* xl, const char* yl, bool descendingX = false) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(10, 11, 16, 255));
    ImGui::InvisibleButton(id, ImVec2(w, h));
    (void)descendingX;
    if (x.size() < 2 || x.size() != y.size()) return;
    const float l = p0.x + 48 * gUi, r = p0.x + w - 6 * gUi, top = p0.y + 6, bot = p0.y + h - ImGui::GetTextLineHeight() - 8;
    double xlo = *std::min_element(x.begin(), x.end()), xhi = *std::max_element(x.begin(), x.end());
    double ylo = *std::min_element(y.begin(), y.end()), yhi = *std::max_element(y.begin(), y.end());
    if (yhi - ylo < 1e-9) yhi = ylo + 1;
    const double pad = (yhi - ylo) * 0.08; ylo -= pad; yhi += pad;
    auto X = [&](double v) { return (float)(l + (v - xlo) / (xhi - xlo) * (r - l)); };
    auto Y = [&](double v) { return (float)(bot - (v - ylo) / (yhi - ylo) * (bot - top)); };
    const ImU32 txt = IM_COL32(130, 136, 150, 255), grid = IM_COL32(40, 44, 56, 255);
    char b[48];
    for (int i = 0; i <= 4; i++) {
        const double v = ylo + (yhi - ylo) * i / 4;
        dl->AddLine(ImVec2(l, Y(v)), ImVec2(r, Y(v)), grid);
        snprintf(b, sizeof b, "%.3g", v);
        dl->AddText(ImVec2(p0.x + 3, Y(v) - 7), txt, b);
        const double xv = xlo + (xhi - xlo) * i / 4;
        snprintf(b, sizeof b, "%.5g", xv);
        dl->AddText(ImVec2(X(xv) - 12, bot + 3), txt, b);
    }
    dl->AddText(ImVec2(r - ImGui::CalcTextSize(xl).x, top), txt, xl);
    dl->AddText(ImVec2(l + 4, top), txt, yl);
    std::vector<ImVec2> pts;
    for (size_t i = 0; i < x.size(); i++) pts.push_back(ImVec2(X(x[i]), Y(y[i])));
    dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(120, 200, 255, 255), 0, 1.3f * gUi);
}

// ------------------------------------------------------------------------------------------------------------------------- Velocity
void targetControls() {
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::BeginCombo("##hlbody", tr(kBodies[H.body]))) {
        for (int i = 0; i < 7; i++) if (ImGui::Selectable(tr(kBodies[i]), H.body == i)) { H.body = i; save(); }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * gUi);
    if (H.body == (int)hl::Body::Galactic) {
        ImGui::InputFloat("l##hll", &H.l, 0, 0, "%.1f"); ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat("b##hlb", &H.b, 0, 0, "%.1f");
    } else if (H.body == (int)hl::Body::Custom) {
        ImGui::InputFloat("RA##hlra", &H.ra, 0, 0, "%.3f"); ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat("Dec##hldec", &H.dec, 0, 0, "%.3f");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
}

// the target for the velocity maths: the body's J2000 position (Sun, Moon: its position now, taken as J2000 within the precession error)
hl::Eq velocityTarget(double jd) {
    const hl::Body b = bodyOf(H.body);
    if (b == hl::Body::Galactic || b == hl::Body::Custom) return targetJ2000();
    if (b == hl::Body::CasA) return hl::kCasA;
    if (b == hl::Body::CygA) return hl::kCygA;
    if (b == hl::Body::SgrA) return hl::kSgrA;
    return hl::bodyPosition(b, jd, {});
}

void velocityTab(App& a, const HlineIn& in) {
    (void)a;
    targetControls();
    const double jd = nowJd();
    double lat, lon;
    site(lat, lon);
    const hl::Eq tg = velocityTarget(jd);
    const hl::Body body = bodyOf(H.body);
    const hl::Hor hz = hl::bodyHorizontal(body, jd, lat, lon, body == hl::Body::Galactic || body == hl::Body::Custom ? targetJ2000() : hl::Eq{});
    const hl::ObserverVelocity o = hl::observerVelocity(tg, jd, lat, lon, H.kinematic ? hl::Lsr::Kinematic : hl::Lsr::Dynamical);
    ImGui::Checkbox(tr("IAU standard solar motion (off: Schoenrich 2010)"), &H.kinematic); ImGui::SameLine();
    ImGui::Checkbox(tr("LSR axis"), &H.lsrAxis);
    ImGui::TextDisabled("%s", tr("Site is the receiver location (View > Map). Time is the computer clock, UTC."));
    ImGui::Text(tr("Az %.1f  El %.1f   site %.3f, %.3f"), hz.azDeg, hz.elDeg, lat, lon);
    ImGui::Text(tr("Towards the target: Earth rotation %+.2f, orbit %+.2f, Sun %+.2f; total %+.2f km/s (add to the topocentric velocity)"), o.rotation, o.orbit, o.sun, o.total);
    if (!in.have || !in.ratio || in.ratio->size() < 64) { ImGui::TextDisabled("%s", tr("No processed spectrum yet.")); return; }
    prepare(in, jd);
    const int n = (int)H.lastTa.size(), e = (int)(in.edge * n);
    std::vector<double> x, y;
    for (int i = e; i < n - e; i++) { x.push_back(H.lsrAxis ? H.lastVlsr[(size_t)i] : H.lastV[(size_t)i]); y.push_back(H.lastTa[(size_t)i]); }
    plot("##hlvel", x, y, std::max(150.f * gUi, ImGui::GetContentRegionAvail().y * 0.5f), H.lsrAxis ? "km/s (LSR)" : "km/s (topocentric)", "K");
    if (ImGui::Button(tr("Fit lines"))) {
        std::vector<double> xs, ys;
        for (int i = e; i < n - e; i++) { const double v = H.lastVlsr[(size_t)i]; if (std::fabs(v) < 300) { xs.push_back(v); ys.push_back(H.lastTa[(size_t)i]); } }
        std::vector<hl::GaussComp> g0 = hl::guessComponents(xs, ys, 4);
        const hl::FitResult f = hl::fitGaussians(xs, ys, g0);
        H.comps = f.comps; H.fitOk = f.ok; H.fitRms = f.rms;
        say(f.ok ? "line fit: " + std::to_string(f.comps.size()) + " component(s), rms " + std::to_string(f.rms) + " K" : "line fit failed (no clear line)");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Save CSV"))) {
        const std::string p = saveFileDialog("hline_lsr.csv");
        if (!p.empty()) {
            char c[96]; snprintf(c, sizeof c, "target l %.2f b %.2f, Tsys %.1f K, LSR %s", hl::galacticFromJ2000(tg).lDeg, hl::galacticFromJ2000(tg).bDeg, H.tsys, H.kinematic ? "IAU" : "dynamical");
            std::vector<double> f((size_t)n);
            for (int i = 0; i < n; i++) f[(size_t)i] = in.centerHz + (i - n / 2) * in.rate / n;
            H.note = writeFile(p, hl::spectrumCsv(f, H.lastV, H.lastCorr, H.lastTa, H.lastFlag, c)) ? "saved " + p : "could not write " + p;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Save FITS"))) {
        const std::string p = saveFileDialog("hline.fits");
        if (!p.empty()) {
            std::vector<double> f((size_t)n);
            for (int i = 0; i < n; i++) f[(size_t)i] = in.centerHz + (i - n / 2) * in.rate / n;
            char tb[32]; snprintf(tb, sizeof tb, "%.2f", H.tsys);
            const std::string ext = hl::fitsTable({"FREQ", "VEL_TOPO", "VEL_LSR", "TA"}, {"Hz", "km/s", "km/s", "K"}, {f, H.lastV, H.lastVlsr, H.lastTa},
                                                  {{"EXTNAME", "SPECTRUM"}, {"RESTFRQ", std::to_string(in.restHz)}, {"TSYS", tb}, {"INTTIME", std::to_string(in.integSec)}});
            const std::string file = hl::fitsFile({{"ORIGIN", "OnAir"}, {"OBSGEO-B", std::to_string(lat)}, {"OBSGEO-L", std::to_string(lon)}, {"LSRCORR", std::to_string(H.lastCorr)}}, {ext});
            H.note = writeFile(p, file) ? "saved " + p : "could not write " + p;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Save SigMF text"))) {
        const std::string p = saveFileDialog("hline.sigmf-meta");
        if (!p.empty()) {
            const hl::Utc u = hl::fromJulianDate(jd);
            char iso[40]; snprintf(iso, sizeof iso, "%04d-%02d-%02dT%02d:%02d:%02dZ", u.y, u.mo, u.d, u.h, u.mi, (int)u.s);
            H.note = writeFile(p, hl::sigmfMeta(in.centerHz, in.rate, "H I spectrum annotated with line fits (LSR km/s)", H.comps, iso, lat, lon, 0)) ? "saved " + p : "could not write " + p;
        }
    }
    if (!H.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", H.note.c_str()); }
    if (H.fitOk) {
        ImGui::TextDisabled("%s", tr("peak K, centre km/s (LSR), FWHM km/s, area K km/s; one sigma"));
        for (size_t i = 0; i < H.comps.size(); i++) {
            const auto& g = H.comps[i];
            ImGui::Text("%zu:  %.2f +- %.2f   %+.2f +- %.2f   %.2f +- %.2f   %.1f +- %.1f", i + 1, g.amp, g.dAmp, g.centre, g.dCentre, g.fwhm(), g.dFwhm(), g.area(), g.dArea);
        }
        ImGui::TextDisabled(tr("fit rms %.3f K"), H.fitRms);
    }
}

// ------------------------------------------------------------------------------------------------------------------------------- Map
void mapTab(App& a, const HlineIn& in) {
    (void)a;
    ImGui::TextWrapped("%s", tr("Point the antenna at each galactic longitude l along b = 0 (set it in Velocity), integrate, then add the spectrum here."));
    ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat(tr("R0 kpc"), &H.r0, 0, 0, "%.1f"); ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat(tr("V0 km/s"), &H.v0, 0, 0, "%.0f");
    if (ImGui::Button(tr("Add spectrum at the target l"))) {
        if (in.have && in.ratio && in.ratio->size() >= 64) {
            prepare(in, nowJd());
            const int n = (int)H.lastTa.size(), e = (int)(in.edge * n);
            std::vector<double> v(H.lastVlsr.begin() + e, H.lastVlsr.end() - e), t(H.lastTa.begin() + e, H.lastTa.end() - e);
            H.map.add(H.l, v, t);
            say("l-v map: added l = " + std::to_string(H.l));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear map"))) { H.map = hl::LvMap(); }
    ImGui::SameLine();
    if (ImGui::Button(tr("Save session"))) {
        const std::string p = saveFileDialog("hline_session.txt");
        if (!p.empty()) {
            hl::Session s;
            s.info["tsys"] = std::to_string(H.tsys); s.info["vlo"] = std::to_string(H.map.vLo); s.info["vhi"] = std::to_string(H.map.vHi); s.info["vstep"] = std::to_string(H.map.vStep);
            s.spectra = H.map.rows; s.times = H.map.l;
            H.note = writeFile(p, hl::sessionText(s)) ? "saved " + p : "could not write " + p;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Open session"))) {
        const std::string p = openFileDialog();
        FILE* f = p.empty() ? nullptr : dect2::u8fopen(p, "rb");
        if (f) {
            std::string txt; char buf[4096]; size_t k;
            while ((k = fread(buf, 1, sizeof buf, f)) > 0) txt.append(buf, k);
            fclose(f);
            hl::Session s;
            if (hl::parseSession(txt, s)) {
                H.map = hl::LvMap();
                if (s.info.count("vlo")) H.map.vLo = std::atof(s.info["vlo"].c_str());
                if (s.info.count("vhi")) H.map.vHi = std::atof(s.info["vhi"].c_str());
                if (s.info.count("vstep")) H.map.vStep = std::atof(s.info["vstep"].c_str());
                H.map.rows = s.spectra; H.map.l = s.times;
                H.note = "opened " + p;
            } else H.note = "not a session file";
        }
    }
    if (!H.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", H.note.c_str()); }
    // the image: l along x (sorted), velocity along y
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = std::max(160.f * gUi, ImGui::GetContentRegionAvail().y * 0.55f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(6, 7, 10, 255));
    ImGui::InvisibleButton("##hlmap", ImVec2(w, h));
    if (!H.map.rows.empty()) {
        std::vector<size_t> ord(H.map.rows.size());
        for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
        std::sort(ord.begin(), ord.end(), [&](size_t x, size_t y) { return H.map.l[x] < H.map.l[y]; });
        double mx = 1e-9;
        for (const auto& r : H.map.rows) for (double v : r) mx = std::max(mx, v);
        const float l0 = p0.x + 40 * gUi, r0 = p0.x + w - 4, cw = std::max(2.f, (r0 - l0) / (float)ord.size());
        const int nv = H.map.nv();
        const float ch = (h - 18) / nv;
        for (size_t c = 0; c < ord.size(); c++) {
            const auto& row = H.map.rows[ord[c]];
            for (int g = 0; g < nv; g++) {
                const float t = (float)std::clamp(row[(size_t)g] / mx, 0.0, 1.0);
                if (t < 0.03f) continue;
                const float y = p0.y + h - 14 - (g + 1) * ch;
                dl->AddRectFilled(ImVec2(l0 + c * cw, y), ImVec2(l0 + (c + 1) * cw, y + ch + 1), Waterfall::jet(t));
            }
            char b[16]; snprintf(b, sizeof b, "%.0f", H.map.l[ord[c]]);
            if (cw > 22 || c % 3 == 0) dl->AddText(ImVec2(l0 + c * cw, p0.y + h - 14), IM_COL32(130, 136, 150, 255), b);
        }
        dl->AddText(ImVec2(p0.x + 3, p0.y + 3), IM_COL32(130, 136, 150, 255), "v LSR");
    } else dl->AddText(ImVec2(p0.x + 44 * gUi, p0.y + 8), IM_COL32(110, 116, 128, 255), tr("no positions yet"));
    const auto term = hl::terminalVelocities(H.map);
    const auto rc = hl::rotationCurve(term, H.r0, H.v0);
    if (rc.size() >= 2) {
        std::vector<double> x, y;
        for (const auto& p : rc) { x.push_back(p.rKpc); y.push_back(p.vKms); }
        plot("##hlrot", x, y, std::max(110.f * gUi, ImGui::GetContentRegionAvail().y - 4), "R kpc (tangent points)", "V km/s");
    } else ImGui::TextDisabled("%s", tr("The rotation curve needs spectra at several longitudes between 0 and 90 (or 270 and 360)."));
}

// ---------------------------------------------------------------------------------------------------------------------------- Pointing
void pointingTab(App&, const HlineIn&) {
    targetControls();
    double lat, lon;
    site(lat, lon);
    const double jd = nowJd();
    const hl::Utc u = hl::fromJulianDate(jd);
    hl::Utc mid = u; mid.h = 0; mid.mi = 0; mid.s = 0;
    const double jd0 = hl::julianDate(mid);
    ImGui::TextDisabled("%s", tr("Body                 Az     El     transit (UTC)  rise    set"));
    for (int i = 0; i < 7; i++) {
        const hl::Body b = bodyOf(i);
        const hl::Eq t = (b == hl::Body::Galactic || b == hl::Body::Custom) ? (i == H.body ? targetJ2000() : hl::Eq{}) : hl::Eq{};
        if ((b == hl::Body::Galactic || b == hl::Body::Custom) && i != H.body) continue;
        const hl::Hor h = hl::bodyHorizontal(b, jd, lat, lon, t);
        const hl::Transit tr_ = hl::events(b, jd0, lat, lon, t, (b == hl::Body::Sun || b == hl::Body::Moon) ? -0.5667 : 0);
        ImGui::Text("%-20s %5.1f %5.1f  %s  %s  %s", tr(kBodies[i]), h.azDeg, h.elDeg, tr_.transits ? utcText(tr_.transitJd).c_str() : "-", tr_.rises && tr_.riseJd ? utcText(tr_.riseJd).c_str() : "-", tr_.rises && tr_.setJd ? utcText(tr_.setJd).c_str() : "-");
    }
    const hl::Body sel = bodyOf(H.body);
    const hl::Eq t = targetJ2000();
    const auto vis = hl::visibility(sel, jd0, lat, lon, t, 144);
    std::vector<double> x(vis.size());
    for (size_t i = 0; i < x.size(); i++) x[i] = 24.0 * (double)i / (double)x.size();
    ImGui::TextDisabled("%s", tr("Elevation of the selected body over the UTC day (hours)"));
    plot("##hlvis", x, vis, std::max(130.f * gUi, ImGui::GetContentRegionAvail().y - 4), "UTC h", "El deg");
}

// -------------------------------------------------------------------------------------------------------------------------- Calibration
void calTab(App&, const HlineIn& in) {
    ImGui::SetNextItemWidth(80 * gUi); if (ImGui::InputFloat(tr("Tsys K"), &H.tsys, 0, 0, "%.1f")) save(); ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat(tr("sky + spill K"), &H.sky, 0, 0, "%.1f");
    ImGui::TextWrapped("%s", tr("Y-factor: capture a spectrum looking at a hot load (room-temperature absorber), then at a cold load (or the cold sky), with their temperatures."));
    ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat(tr("hot K"), &H.tHot, 0, 0, "%.0f"); ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat(tr("cold K"), &H.tCold, 0, 0, "%.0f");
    auto meanPow = [&]() { double s = 0; if (in.on && !in.on->empty()) { const int n = (int)in.on->size(); for (int i = n / 4; i < n - n / 4; i++) s += (*in.on)[(size_t)i]; s /= (n - 2 * (n / 4)); } return s; };
    if (ImGui::Button(tr("Capture hot"))) { H.pHot = meanPow(); say("hot load captured"); }
    ImGui::SameLine();
    if (ImGui::Button(tr("Capture cold"))) { H.pCold = meanPow(); say("cold load captured"); }
    ImGui::SameLine();
    if (ImGui::Button(tr("Compute"))) {
        H.yf = hl::yFactor(H.pHot, H.pCold, H.tHot, H.tCold);
        if (H.yf.ok) { H.tsys = (float)hl::tsysFromTrx(H.yf.trx, H.sky); save(); say("Y-factor " + std::to_string(H.yf.y) + ", Trx " + std::to_string(H.yf.trx) + " K, Tsys " + std::to_string(H.tsys) + " K"); }
        else say("Y-factor failed: the hot power must exceed the cold power");
    }
    if (H.yf.ok) ImGui::Text(tr("Y = %.3f   receiver %.1f K   Tsys %.1f K"), H.yf.y, H.yf.trx, H.tsys);
    ImGui::Separator();
    ImGui::Checkbox(tr("Flag and interpolate RFI"), &H.rfi); ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * gUi); ImGui::SliderFloat(tr("sigma"), &H.rfiSigma, 3, 10, "%.1f");
    int flagged = 0;
    for (bool f : H.lastFlag) flagged += f;
    ImGui::Text(tr("%d bins flagged in the last spectrum"), flagged);
    if (!H.lastFlag.empty()) {
        std::vector<double> x, y;
        const int n = (int)H.lastFlag.size();
        for (int i = 0; i < n; i += std::max(1, n / 512)) { x.push_back(i); y.push_back(H.lastFlag[(size_t)i] ? 1 : 0); }
        plot("##hlrfi", x, y, 60 * gUi, "bin", "flag");
    }
}

void logTab(App&, const HlineIn&) {
    if (H.log.empty()) ImGui::TextDisabled("%s", tr("Nothing logged yet."));
    for (size_t i = H.log.size(); i-- > 0;) ImGui::TextUnformatted(H.log[i].c_str());
}

} // namespace

void hlineLog(const std::string& s) { say(s); }

void hlineTab(App& a, HlineTab t, const HlineIn& in) {
    load();
    switch (t) {
    case HlineTab::Velocity: velocityTab(a, in); break;
    case HlineTab::Map: mapTab(a, in); break;
    case HlineTab::Pointing: pointingTab(a, in); break;
    case HlineTab::Calibration: calTab(a, in); break;
    case HlineTab::Log: logTab(a, in); break;
    }
}
