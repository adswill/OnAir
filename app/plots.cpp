// spectrum, waterfall, histogram and constellation plots
#include "app.h"

// Click on the spectrum or the waterfall to tune to that frequency (a drag still pans). The grid follows the mode: FM 0.1 MHz, DMR 12.5 kHz, else 1 kHz.
static void clickToTune(App& a) {
    if (!plt::IsPlotHovered() || a.fmScan.running) return;
    double mhz = plt::GetPlotMousePos().x;
    double lo = 0.1, hi = 6000;
    double grid = 0.001;
    if (a.fmMode) { lo = 87.5; hi = 108.0; grid = 0.1; }
    else if (a.family == 9) grid = 0.0125;
    if (a.family >= 6) if (const ModeTuning* mt = modeTuning(a.family + 2)) { lo = mt->minMhz; hi = mt->maxMhz; }
    mhz = std::round(std::min(hi, std::max(lo, mhz)) / grid) * grid;
    if (a.family == 4 || a.family == 1 || a.family == 3) return;   // ATSC and ISDB-T channels sit on a fixed raster: use the frequency field
    const ImVec2 d = ImGui::GetMouseDragDelta(0, 0);
    if (ImGui::IsMouseReleased(0) && std::fabs(d.x) < 4 && std::fabs(d.y) < 4 && std::fabs(mhz - a.freqMhz) > 1e-6) {
        if (a.fmMode) fmTune(a, mhz); else tuneFreq(a, mhz);
        return;
    }
    if (std::fabs(mhz - a.freqMhz) > 1e-6) {
        const plt::Rect lim = plt::GetPlotLimits();   // a red line where a click would tune to
        const double lx[2] = {mhz, mhz}, ly[2] = {lim.Y.Min, lim.Y.Max};
        plt::Spec ls; ls.LineColor = ImVec4(0.95f, 0.25f, 0.25f, 0.9f); ls.LineWeight = 1.2f;
        plt::PlotLine("##tunehere", lx, ly, 2, ls);
        ImGui::SetTooltip("click to tune to %.4f MHz", mhz);
    }
}

ChanWidth channelWidth(const App& a) {
    ChanWidth w;
    if (a.family == 0) {   // what the engine uses while it runs (Automatic may have just switched), else the choice
        const double act = a.engine.running() ? a.engine.activeBandwidth() : 0;
        w.mhz = act > 0 ? act : kBw[a.bwIdx].mhz;
        w.settable = w.hasAuto = true; w.manual = !a.bwAuto;
    } else if (a.atscMode) w.mhz = 6;   // ATSC 1.0 and 3.0, ISDB-T
    else if (a.dabMode) w.mhz = 1.536;
    else if (a.fmMode) {
        w.settable = w.hasAuto = true; w.manual = a.fmChanKhz > 0;
        w.mhz = (w.manual ? a.fmChanKhz * 1e3 : FmReceiver::kDefaultChannelWidthHz) / 1e6;
    } else if (a.family == 7) { w.mhz = a.dtmbBwMhz; w.settable = w.manual = true; }
    else if (a.family == 6 && a.engine.running() && a.rx.standard == 7 && a.rx.dvbs.symbolRate > 0) w.mhz = a.rx.dvbs.symbolRate * (1 + a.rx.dvbs.rollOff) / 1e6;
    else if (const ModeTuning* mt = modeTuning(a.family + 2)) w.mhz = mt->bandwidthMhz;
    return w;
}

std::string widthText(double mhz) {
    char b[32];
    if (mhz >= 1) snprintf(b, sizeof b, "%.4g MHz", mhz); else snprintf(b, sizeof b, "%.4g kHz", mhz * 1e3);
    return b;
}

void channelWidthPopup(App& a) {
    const ChanWidth cw = channelWidth(a);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 330 * gUi);
    ImGui::TextDisabled("Channel width: %s", widthText(cw.mhz).c_str());
    ImGui::Separator();
    if (a.family == 0) {
        if (ImGui::RadioButton("Automatic", a.bwAuto) && !a.bwAuto) setDvbBandwidth(a, -1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The width of the signal is measured in the spectrum and the receiver switches between 1.7, 5, 6, 7 and 8 MHz by itself.");
        if (ImGui::RadioButton("Manual", !a.bwAuto) && a.bwAuto) setDvbBandwidth(a, a.bwIdx);
        ImGui::BeginDisabled(a.bwAuto);
        ImGui::Indent(ImGui::GetFrameHeight());
        for (int i = 0; i < (int)(sizeof kBw / sizeof *kBw); i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(kBw[i].label, !a.bwAuto && i == a.bwIdx)) setDvbBandwidth(a, i);
        }
        ImGui::Unindent(ImGui::GetFrameHeight());
        ImGui::EndDisabled();
        ImGui::TextDisabled("DVB-T2 and DVB-T channels sit on this raster; a running radio may be opened again at another sample rate.");
    } else if (a.fmMode) {
        if (ImGui::RadioButton("Automatic (220 kHz)", a.fmChanKhz == 0) && a.fmChanKhz != 0) { a.fmChanKhz = 0; savePrefs(a); }
        if (ImGui::RadioButton("Manual", a.fmChanKhz > 0) && a.fmChanKhz == 0) { a.fmChanKhz = 180; savePrefs(a); }
        ImGui::BeginDisabled(a.fmChanKhz == 0);
        ImGui::SameLine();
        int k = a.fmChanKhz > 0 ? a.fmChanKhz : 220;
        ImGui::SetNextItemWidth(150 * gUi);
        if (ImGui::SliderInt("##fmchw", &k, (int)(FmReceiver::kMinChannelWidthHz / 1e3), (int)(FmReceiver::kMaxChannelWidthHz / 1e3), "%d kHz")) a.fmChanKhz = (k + 5) / 10 * 10;
        if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
        ImGui::EndDisabled();
        ImGui::TextDisabled("The receiver's channel filter. Narrower keeps a strong neighbour out (150 kHz copes with one 10 dB stronger 150 kHz away); "
                            "below about 180 kHz loud passages distort a little and RDS suffers first.");
    } else if (a.family == 7) {
        if (ImGui::RadioButton("8 MHz", a.dtmbBwMhz != 6) && a.dtmbBwMhz == 6) setDtmbBandwidth(a, 8);
        ImGui::SameLine();
        if (ImGui::RadioButton("6 MHz (Cuba)", a.dtmbBwMhz == 6) && a.dtmbBwMhz != 6) setDtmbBandwidth(a, 6);
        ImGui::TextDisabled("Set by the country: there is nothing to measure it from. A receiver set to the wrong width finds nothing.");
    } else if (a.family == 6) {
        ImGui::TextUnformatted("Follows the symbol rate: symbol rate x (1 + roll-off).");
        ImGui::TextDisabled("Choose automatic or a manual symbol rate in the tuner to change it.");
    } else {
        const char* name = "This mode";
        for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) name = kModes[i].name;
        ImGui::Text("%s: fixed for this mode.", name);
        ImGui::TextDisabled("The receiver's filters are made for the signal (or for the group of channels it watches), so a manual width would only make reception worse.");
    }
    ImGui::PopTextWrapPos();
}

// The channel band behind the trace (spectrum: a light fill and its edges) or on top of the picture (waterfall: the edges only)
static void channelBand(const App& a, bool fill) {
    const ChanWidth cw = channelWidth(a);
    if (cw.mhz <= 0) return;
    const double x0 = a.freqMhz - cw.mhz / 2, x1 = a.freqMhz + cw.mhz / 2;
    if (fill) { plt::Spec band; band.FillColor = pal::accent(pal::dev() ? 0.14f : 0.16f); plt::PlotVBand("band", x0, x1, band); }
    const double edges[2] = {x0, x1};
    plt::Spec es; es.LineColor = pal::accent(fill ? 0.40f : 0.55f); es.LineWeight = 1.f;
    plt::PlotInfLines("bandedges", edges, 2, es);
}

// "BW 7 MHz auto" in the top right corner of the spectrum: the width the band shows, and a click opens channelWidthPopup().
// Left out when it would cover yLabel, which the plot draws in its top left corner.
static void channelWidthButton(App& a, ImVec2 plotMin, ImVec2 plotSize, const char* yLabel) {
    const ChanWidth cw = channelWidth(a);
    if (cw.mhz <= 0) return;
    char b[64];
    snprintf(b, sizeof b, "BW %s%s###bwbtn", widthText(cw.mhz).c_str(), cw.hasAuto ? (cw.manual ? " manual" : " auto") : "");
    const ImVec2 sz = ImGui::CalcTextSize(b, nullptr, true);
    const float w = sz.x + 2 * ImGui::GetStyle().FramePadding.x, m = 4 * gUi;
    if (plotSize.x < w + 2 * m + ImGui::CalcTextSize(yLabel).x + 12 * gUi || plotSize.y < sz.y + 2 * m) return;   // no room
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(plotMin.x + plotSize.x - w - m, plotMin.y + m));
    ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg); bg.w = 0.85f;
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, cw.manual && cw.hasAuto ? pal::warnAmber() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    if (ImGui::SmallButton(b)) ImGui::OpenPopup("##bwpop");
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(cw.settable ? "Channel width: the band the receiver works with (the light band).\nClick to choose automatic or a manual width."
                                      : "Channel width: the band the receiver works with (the light band), set by the standard.");
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + 2 * gUi), ImGuiCond_Appearing, ImVec2(1, 0));   // under the button, right-aligned
    if (ImGui::BeginPopup("##bwpop")) { channelWidthPopup(a); ImGui::EndPopup(); }
    ImGui::SetCursorScreenPos(keep);
}

void spectrumPlot(App& a, ImVec2 size, bool noFreqAxis) {
    static const char* const kYLabel = "power (dBFS/bin)";
    ImVec2 plotMin, plotSize;
    bool drawn = false;
    if (plt::BeginPlot("##spec", size, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Overlay)) {
        drawn = true;
        if (noFreqAxis) plt::SetupAxes(nullptr, kYLabel, plt::AxisFlags_NoTickLabels, 0);   // the waterfall under it shows the frequencies
        else plt::SetupAxes("frequency (MHz)", kYLabel);
        double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
        // follow a retune: when the centre or the span changes, bring the view back to the new band (otherwise it keeps the user's own zoom)
        static double lastC = 0, lastFs = 0;
        const double rc = radioCenterMhz(a);   // the axis shows the radio's real centre, so a signal sits at its true frequency
        const bool moved = lastC != rc || lastFs != fs;
        lastC = rc; lastFs = fs;
        plt::SetupAxisLimits(plt::X1, rc - fs / 2, rc + fs / 2, moved ? plt::Cond_Always : plt::Cond_Once);
        plt::SetupAxisLimits(plt::Y1, a.yMin, a.yMax, plt::Cond_Once);
        plt::SetupAxisFormat(plt::X1, "%.2f");
        channelBand(a, true);
        plotMin = plt::GetPlotPos(); plotSize = plt::GetPlotSize();
        if (!a.smooth.empty()) {
            auto x = xs(a);
            if (a.peakHold) {
                plt::Spec ps; ps.LineColor = pal::grey(0.40f); ps.LineWeight = 1.0f;
                std::vector<double> yp(a.peak.begin(), a.peak.end());
                plt::PlotLine("peak", x.data(), yp.data(), (int)std::min(x.size(), yp.size()), ps);
            }
            plt::Spec ss; ss.LineColor = pal::accent(); ss.LineWeight = 1.3f;
            std::vector<double> ysm(a.smooth.begin(), a.smooth.end());
            plt::PlotLine("spectrum", x.data(), ysm.data(), (int)std::min(x.size(), ysm.size()), ss);
            double cx[2] = {a.freqMhz, a.freqMhz}, cy[2] = {a.yMin, a.yMax};
            plt::Spec cs; cs.LineColor = pal::grey(0.35f);
            plt::PlotLine("centre", cx, cy, 2, cs);
            if (a.fmMode && pal::dev()) {   // the stations the scan found, as bookmarks (as in SDR++)
                const plt::Rect lim = plt::GetPlotLimits();
                int k = 0;
                for (const auto& r : a.fmScan.results) {
                    if (!r.found || r.mhz < lim.X.Min || r.mhz > lim.X.Max) continue;
                    double mx[2] = {r.mhz, r.mhz}, my[2] = {a.yMin, a.yMax};
                    plt::Spec ms; ms.LineColor = ImVec4(0.85f, 0.85f, 0.85f, 0.22f);
                    char id[16]; snprintf(id, sizeof id, "mark%d", k++);
                    plt::PlotLine(id, mx, my, 2, ms);
                    char lb[48]; snprintf(lb, sizeof lb, "%s", r.name.empty() ? "" : r.name.c_str());
                    if (lb[0]) plt::PlotText(lb, r.mhz, a.yMax, ImVec2(ImGui::CalcTextSize(lb).x * 0.5f + 4, 10 + (k % 2) * 13));
                }
            }
        }
        clickToTune(a);
        plt::EndPlot();
    }
    if (drawn) channelWidthButton(a, plotMin, plotSize, kYLabel);
}

void waterfallPlot(App& a, ImVec2 size) {
    const int H = Waterfall::H;
    // The time scale is worked out once and kept until the plot changes size (or the first real measure of the row time is in),
    // so the labels never move while the picture scrolls under them.
    struct Scale { ImVec2 avail{-1, -1}; bool measured = false; double dt = 0; std::vector<double> pos; std::vector<std::string> lab; };
    static Scale sc;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool measured = a.wf.nStamps >= 128;
    if (avail.x != sc.avail.x || avail.y != sc.avail.y || measured != sc.measured) {
        sc.avail = avail; sc.measured = measured; sc.dt = a.wf.rowDt > 0 ? a.wf.rowDt : 1.0 / 30;   // a zero would make the label step zero: an endless loop
        // a 1-2-5 step that gives a label about every three text lines
        const double secs = H * sc.dt, want = std::max(2.0, (double)avail.y / (ImGui::GetTextLineHeight() * 3));
        const double raw = secs / want, mag = std::pow(10.0, std::floor(std::log10(raw))), n = raw / mag;
        const double step = (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
        const int dec = std::max(0, (int)std::ceil(-std::log10(step) - 1e-9));
        sc.pos.clear(); sc.lab.clear();
        for (double s = 0; s <= secs + 1e-9; s += step) {
            char b[32]; snprintf(b, sizeof b, "%.*f", dec, s > 0 ? -s : 0.0);
            sc.pos.push_back(-s / sc.dt); sc.lab.push_back(b);
        }
    }
    if (plt::BeginPlot("##wf", size, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("frequency (MHz)", "seconds ago");
        double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
        const double rc = radioCenterMhz(a);
        double x0 = rc - fs / 2, x1 = rc + fs / 2;
        static double lastC = 0, lastFs = 0;
        const bool moved = lastC != rc || lastFs != fs;
        lastC = rc; lastFs = fs;
        plt::SetupAxisLimits(plt::X1, x0, x1, moved ? plt::Cond_Always : plt::Cond_Once);
        // y is in rows, so the picture keeps its size while the measured row time wanders; the labels are the fixed scale above
        plt::SetupAxisLimits(plt::Y1, -H, 0, plt::Cond_Always);
        plt::SetupAxisUnit(plt::Y1, sc.dt);
        std::vector<const char*> labs;
        for (auto& s : sc.lab) labs.push_back(s.c_str());
        plt::SetupAxisTicks(plt::Y1, sc.pos.data(), (int)sc.pos.size(), labs.data());
        plt::SetupAxisFormat(plt::X1, "%.2f");
        // rows arrive at the engine's ~30 Hz, the screen draws faster: slide down by the part of a row that is due since the last one,
        // so the picture moves at an even speed instead of jumping a row at a time
        const double frac = a.wf.pushT > 0 ? std::min(1.0, std::max(0.0, (glfwGetTime() - a.wf.pushT) / a.wf.rowDt)) : 0;
        int w = a.wf.writeRow;
        ImTextureID tex = a.wf.img->texture();
        plt::PlotImage("a", tex, plt::Point(x0, -(H - w) - frac), plt::Point(x1, -frac), ImVec2(0, (float)w / H), ImVec2(1, 1));
        if (w > 0)
            plt::PlotImage("b", tex, plt::Point(x0, -H - frac), plt::Point(x1, -(H - w) - frac), ImVec2(0, 0), ImVec2(1, (float)w / H));
        const float vNew = (w + 0.5f) / H;   // the gap above: the newest row, stretched
        if (frac > 0) plt::PlotImage("top", tex, plt::Point(x0, -frac), plt::Point(x1, 0), ImVec2(0, vNew), ImVec2(1, vNew));
        channelBand(a, false);
        clickToTune(a);
        plt::EndPlot();
    }
}

void histogramPlot(App& a, ImVec2 size) {
    if (plt::BeginPlot("##hist", size, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("|sample| (fraction of full scale)", "count");
        plt::SetupAxisScale(plt::Y1, plt::Scale_Log10);
        plt::SetupAxisLimits(plt::X1, 0, 1, plt::Cond_Once);
        float h[64], x[64];
        for (int i = 0; i < 64; i++) { h[i] = (float)std::max<uint32_t>(1, a.spec.stats.hist[i]); x[i] = (i + 0.5f) / 64; }
        plt::Spec bs; bs.FillColor = pal::accent(0.8f);
        plt::PlotBars("adc", x, h, 64, 1.0 / 64, bs);
        plt::EndPlot();
    }
}

// Decoded-cell views of the data constellation: density heat map and per-point clusters
void constDensityPlot(App& a, ImVec2 sz) {
    const App::ConstStats& c = a.cst;
    if (plt::BeginPlot("##c2d", sz, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Equal | plt::Flags_NoMouseText)) {
        plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
        plt::SetupAxisLimits(plt::X1, -1.4, 1.4, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -1.4, 1.4, plt::Cond_Always);
        const int G = App::ConstStats::G;
        if ((int)c.grid.size() == G * G) {
            std::vector<float> v(c.grid.size());
            float mx = 0;
            for (size_t i = 0; i < v.size(); i++) { v[i] = std::sqrt(c.grid[i]); mx = std::max(mx, v[i]); }   // square root: faint areas stay visible
            static int cm = -1;
            if (cm < 0) {   // black -> accent -> white, instead of the rainbow-like "hot" map
                const ImVec4 cols[4] = {ImVec4(0.02f, 0.03f, 0.05f, 1), ImVec4(0.10f, 0.28f, 0.45f, 1), pal::accent(), ImVec4(0.95f, 0.98f, 1.f, 1)};
                cm = plt::AddColormap("onair", cols, 4);
            }
            plt::PushColormap(cm);
            plt::PlotHeatmap("density", v.data(), G, G, 0, std::max(1.f, mx * 0.85f), nullptr, plt::Point(-1.4, -1.4), plt::Point(1.4, 1.4));
            plt::PopColormap();
        }
        const int M = 2 * (a.rx.plpFec.mod + 1);
        std::vector<cf32> grid;
        for (unsigned l = 0; l < (1u << M); l++) grid.push_back(qamPoint(a.rx.plpFec.mod, false, l));
        plt::Spec gs; gs.Marker = plt::Marker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(cf32);
        gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.55f);
        const float* g = reinterpret_cast<const float*>(grid.data());
        plt::PlotScatter("ideal", g, g + 1, (int)grid.size(), gs);
        plt::EndPlot();
    }
}

void constClusterPlot(App& a, ImVec2 sz) {
    const App::ConstStats& c = a.cst;
    const int mod = a.rx.plpFec.mod;
    static const float kDmin[4] = {1.4142f, 0.6325f, 0.3086f, 0.1534f};
    const float half = 0.5f * kDmin[mod];
    if (plt::BeginPlot("##c2k", sz, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Equal | plt::Flags_NoMouseText)) {
        plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
        plt::SetupAxisLimits(plt::X1, -1.4, 1.4, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -1.4, 1.4, plt::Cond_Always);
        int hover = -1;
        double meanRatio = 0; int nr = 0;
        for (auto& p : c.pts) if (p.n >= 4) { meanRatio += std::sqrt(p.e2 / (2 * p.n)); nr++; }
        meanRatio = nr ? meanRatio / nr : 1;
        const plt::Point mp = plt::GetPlotMousePos();
        double bestD = 1e9;
        for (size_t l = 0; l < c.pts.size(); l++) {
            const App::ConstStats::Pt& p = c.pts[l];
            const cf32 tx = qamPoint(mod, false, (unsigned)l);
            const double d = std::hypot(mp.x - tx.real(), mp.y - tx.imag());
            if (plt::IsPlotHovered() && d < bestD && d < half * 1.2) { bestD = d; hover = (int)l; }
            if (p.n < 4) continue;
            const double sg = std::sqrt(p.e2 / (2 * p.n));        // standard deviation per axis
            // colour relative to the average ring: green = tighter than average, red = looser (a coded signal is decoded at
            // noise levels where all rings overlap, so an absolute scale would show everything red)
            const float t = (float)std::min(1.0, std::max(0.0, 0.5 + (sg / meanRatio - 1.0) * 4.0));
            const ImVec4 col(0.25f + 0.7f * t, 0.85f - 0.5f * t, 0.45f - 0.15f * t, 0.9f);
            const double cx = tx.real() + p.ei / p.n, cy = tx.imag() + p.eq / p.n;
            float xs[25], ys[25];
            for (int k = 0; k < 25; k++) { const double th = k * 2 * M_PI / 24; xs[k] = (float)(cx + sg * std::cos(th)); ys[k] = (float)(cy + sg * std::sin(th)); }
            plt::Spec ls; ls.LineColor = col; ls.LineWeight = (int)l == hover ? 2.5f : 1.3f;
            plt::PlotLine("##ring", xs, ys, 25, ls);
            plt::Spec ds; ds.Marker = plt::Marker_Circle; ds.MarkerSize = 2.f; ds.MarkerFillColor = ds.MarkerLineColor = ds.LineColor = col;
            const float px = (float)cx, py = (float)cy;
            plt::PlotScatter("##centre", &px, &py, 1, ds);
        }
        {
            const int M = 2 * (mod + 1);
            std::vector<cf32> grid;
            for (unsigned l = 0; l < (1u << M); l++) grid.push_back(qamPoint(mod, false, l));
            plt::Spec gs; gs.Marker = plt::Marker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(cf32);
            gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.8f);
            const float* g = reinterpret_cast<const float*>(grid.data());
            plt::PlotScatter("ideal", g, g + 1, (int)grid.size(), gs);
        }
        if (hover >= 0 && c.pts[hover].n >= 4) {
            const App::ConstStats::Pt& p = c.pts[hover];
            const int M = 2 * (mod + 1);
            char bits[16];
            for (int b = 0; b < M; b++) bits[b] = ((hover >> (M - 1 - b)) & 1) ? '1' : '0';
            bits[M] = 0;
            ImGui::BeginTooltip();
            ImGui::Text("point %s", bits);
            ImGui::Text("sigma %.3f per axis (%.0f%% of the half-distance to a neighbour)", std::sqrt(p.e2 / (2 * p.n)), 100 * std::sqrt(p.e2 / (2 * p.n)) / half);
            ImGui::Text("offset %+.3f %+.3f", p.ei / p.n, p.eq / p.n);
            ImGui::Text("EVM %.1f dB", 10 * std::log10(std::max(1e-9, p.e2 / p.n)));
            ImGui::EndTooltip();
        }
        plt::EndPlot();
    }
}

