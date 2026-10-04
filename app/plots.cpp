// spectrum, waterfall, histogram and constellation plots
#include "app.h"

void spectrumPlot(App& a, ImVec2 size) {
    if (ImPlot::BeginPlot("##spec", size, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("frequency (MHz)", "power (dBFS/bin)");
        double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
        // follow a retune: when the centre or the span changes, bring the view back to the new band (otherwise it keeps the user's own zoom)
        static double lastC = 0, lastFs = 0;
        const bool moved = lastC != a.freqMhz || lastFs != fs;
        lastC = a.freqMhz; lastFs = fs;
        ImPlot::SetupAxisLimits(ImAxis_X1, a.freqMhz - fs / 2, a.freqMhz + fs / 2, moved ? ImPlotCond_Always : ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, a.yMin, a.yMax, ImPlotCond_Once);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.2f");
        if (!a.smooth.empty()) {
            auto x = xs(a);
            // channel overlay: 8 MHz occupied band around the centre
            double bw = kBw[a.bwIdx].mhz;
            double xo[2] = {a.freqMhz - bw / 2 * 0.95, a.freqMhz + bw / 2 * 0.95};
            double yo[2] = {a.yMax, a.yMax};
            ImPlotSpec band; band.FillColor = pal::accent(0.10f); band.LineColor = ImVec4(0, 0, 0, 0);
            ImPlot::PlotShaded("band", xo, yo, 2, a.yMin, band);
            if (a.peakHold) {
                ImPlotSpec ps; ps.LineColor = pal::grey(0.40f); ps.LineWeight = 1.0f;
                std::vector<double> yp(a.peak.begin(), a.peak.end());
                ImPlot::PlotLine("peak", x.data(), yp.data(), (int)std::min(x.size(), yp.size()), ps);
            }
            ImPlotSpec ss; ss.LineColor = pal::accent(); ss.LineWeight = 1.3f;
            std::vector<double> ysm(a.smooth.begin(), a.smooth.end());
            ImPlot::PlotLine("spectrum", x.data(), ysm.data(), (int)std::min(x.size(), ysm.size()), ss);
            double cx[2] = {a.freqMhz, a.freqMhz}, cy[2] = {a.yMin, a.yMax};
            ImPlotSpec cs; cs.LineColor = pal::grey(0.35f);
            ImPlot::PlotLine("centre", cx, cy, 2, cs);
        }
        ImPlot::EndPlot();
    }
}

void waterfallPlot(App& a, ImVec2 size) {
    const int H = Waterfall::H;
    double secs = H * a.frameDt;
    if (ImPlot::BeginPlot("##wf", size, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("frequency (MHz)", "seconds ago");
        double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
        double x0 = a.freqMhz - fs / 2, x1 = a.freqMhz + fs / 2;
        static double lastC = 0, lastFs = 0;
        const bool moved = lastC != a.freqMhz || lastFs != fs;
        lastC = a.freqMhz; lastFs = fs;
        ImPlot::SetupAxisLimits(ImAxis_X1, x0, x1, moved ? ImPlotCond_Always : ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -secs, 0, ImPlotCond_Once);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.2f");
        int w = a.wf.writeRow;
        double secA = (H - w) * a.frameDt;
        ImPlot::PlotImage("a", a.wf.img->texture(), ImPlotPoint(x0, -secA), ImPlotPoint(x1, 0), ImVec2(0, (float)w / H), ImVec2(1, 1));
        if (w > 0)
            ImPlot::PlotImage("b", a.wf.img->texture(), ImPlotPoint(x0, -secs), ImPlotPoint(x1, -secA), ImVec2(0, 0), ImVec2(1, (float)w / H));
        ImPlot::EndPlot();
    }
}

void histogramPlot(App& a, ImVec2 size) {
    if (ImPlot::BeginPlot("##hist", size, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("|sample| (fraction of full scale)", "count");
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, 1, ImPlotCond_Once);
        float h[64], x[64];
        for (int i = 0; i < 64; i++) { h[i] = (float)std::max<uint32_t>(1, a.spec.stats.hist[i]); x[i] = (i + 0.5f) / 64; }
        ImPlotSpec bs; bs.FillColor = pal::accent(0.8f);
        ImPlot::PlotBars("adc", x, h, 64, 1.0 / 64, bs);
        ImPlot::EndPlot();
    }
}

// Decoded-cell views of the data constellation: density heat map and per-point clusters
void constDensityPlot(App& a, ImVec2 sz) {
    const App::ConstStats& c = a.cst;
    if (ImPlot::BeginPlot("##c2d", sz, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_Equal | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, -1.4, 1.4, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.4, 1.4, ImPlotCond_Always);
        const int G = App::ConstStats::G;
        if ((int)c.grid.size() == G * G) {
            std::vector<float> v(c.grid.size());
            float mx = 0;
            for (size_t i = 0; i < v.size(); i++) { v[i] = std::sqrt(c.grid[i]); mx = std::max(mx, v[i]); }   // square root: faint areas stay visible
            static ImPlotColormap cm = -1;
            if (cm < 0) {   // black -> accent -> white, instead of the rainbow-like "hot" map
                const ImVec4 cols[4] = {ImVec4(0.02f, 0.03f, 0.05f, 1), ImVec4(0.10f, 0.28f, 0.45f, 1), pal::accent(), ImVec4(0.95f, 0.98f, 1.f, 1)};
                cm = ImPlot::AddColormap("onair", cols, 4);
            }
            ImPlot::PushColormap(cm);
            ImPlot::PlotHeatmap("density", v.data(), G, G, 0, std::max(1.f, mx * 0.85f), nullptr, ImPlotPoint(-1.4, -1.4), ImPlotPoint(1.4, 1.4));
            ImPlot::PopColormap();
        }
        const int M = 2 * (a.rx.plpFec.mod + 1);
        std::vector<cf32> grid;
        for (unsigned l = 0; l < (1u << M); l++) grid.push_back(qamPoint(a.rx.plpFec.mod, false, l));
        ImPlotSpec gs; gs.Marker = ImPlotMarker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(cf32);
        gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.55f);
        const float* g = reinterpret_cast<const float*>(grid.data());
        ImPlot::PlotScatter("ideal", g, g + 1, (int)grid.size(), gs);
        ImPlot::EndPlot();
    }
}

void constClusterPlot(App& a, ImVec2 sz) {
    const App::ConstStats& c = a.cst;
    const int mod = a.rx.plpFec.mod;
    static const float kDmin[4] = {1.4142f, 0.6325f, 0.3086f, 0.1534f};
    const float half = 0.5f * kDmin[mod];
    if (ImPlot::BeginPlot("##c2k", sz, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_Equal | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, -1.4, 1.4, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.4, 1.4, ImPlotCond_Always);
        int hover = -1;
        double meanRatio = 0; int nr = 0;
        for (auto& p : c.pts) if (p.n >= 4) { meanRatio += std::sqrt(p.e2 / (2 * p.n)); nr++; }
        meanRatio = nr ? meanRatio / nr : 1;
        const ImPlotPoint mp = ImPlot::GetPlotMousePos();
        double bestD = 1e9;
        for (size_t l = 0; l < c.pts.size(); l++) {
            const App::ConstStats::Pt& p = c.pts[l];
            const cf32 tx = qamPoint(mod, false, (unsigned)l);
            const double d = std::hypot(mp.x - tx.real(), mp.y - tx.imag());
            if (ImPlot::IsPlotHovered() && d < bestD && d < half * 1.2) { bestD = d; hover = (int)l; }
            if (p.n < 4) continue;
            const double sg = std::sqrt(p.e2 / (2 * p.n));        // standard deviation per axis
            // colour relative to the average ring: green = tighter than average, red = looser (a coded signal is decoded at
            // noise levels where all rings overlap, so an absolute scale would show everything red)
            const float t = (float)std::min(1.0, std::max(0.0, 0.5 + (sg / meanRatio - 1.0) * 4.0));
            const ImVec4 col(0.25f + 0.7f * t, 0.85f - 0.5f * t, 0.45f - 0.15f * t, 0.9f);
            const double cx = tx.real() + p.ei / p.n, cy = tx.imag() + p.eq / p.n;
            float xs[25], ys[25];
            for (int k = 0; k < 25; k++) { const double th = k * 2 * M_PI / 24; xs[k] = (float)(cx + sg * std::cos(th)); ys[k] = (float)(cy + sg * std::sin(th)); }
            ImPlotSpec ls; ls.LineColor = col; ls.LineWeight = (int)l == hover ? 2.5f : 1.3f;
            ImPlot::PlotLine("##ring", xs, ys, 25, ls);
            ImPlotSpec ds; ds.Marker = ImPlotMarker_Circle; ds.MarkerSize = 2.f; ds.MarkerFillColor = ds.MarkerLineColor = ds.LineColor = col;
            const float px = (float)cx, py = (float)cy;
            ImPlot::PlotScatter("##centre", &px, &py, 1, ds);
        }
        {
            const int M = 2 * (mod + 1);
            std::vector<cf32> grid;
            for (unsigned l = 0; l < (1u << M); l++) grid.push_back(qamPoint(mod, false, l));
            ImPlotSpec gs; gs.Marker = ImPlotMarker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(cf32);
            gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.8f);
            const float* g = reinterpret_cast<const float*>(grid.data());
            ImPlot::PlotScatter("ideal", g, g + 1, (int)grid.size(), gs);
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
        ImPlot::EndPlot();
    }
}

