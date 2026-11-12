// DVB-C: the QAM order the symbol cloud looks like, next to the one the decoder uses. HD Radio: level of each digital sideband and the imbalance. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/d40/tv_d40.h"
#include "dect2/dvbc/dvbc_tel.h"
#include <cmath>
#include <complex>
#include <cstdio>

namespace d4 = dect2::d40;

void d40DvbcQamLine(const dect2::DvbcTelemetry& t) {
    if (t.cells.size() < 400) return;
    std::vector<std::complex<double>> p;
    p.reserve(t.cells.size());
    for (const auto& c : t.cells) p.push_back({(double)c.real(), (double)c.imag()});
    const auto g = d4::guessQam(p);
    if (g.order == 0) { ImGui::TextDisabled("%s", tr("Constellation check: no regular grid recognised.")); return; }
    static const int kOrder[5] = {16, 32, 64, 128, 256};
    const int used = t.qam >= 0 && t.qam < 5 ? kOrder[t.qam] : 0;
    ImGui::Text("%s: %d-QAM (MER %.1f dB)", tr("The symbol cloud looks like"), g.order, g.merDb);
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A check of the received symbols against each QAM grid, judged against what a random cloud would score on the same grid (a finer grid always fits better). The decoder finds its order from the stream by itself; this is a second opinion."));
    if (used && used != g.order) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s %d-QAM", tr("decoder uses"), used); }
}

void d40HdrLevelsCard(App& a) {
    if (!typo::collapsingHeader(trw("Digital sideband levels"), 0)) return;
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Power of the lower and upper digital sideband against the host signal, from the capture spectrum. A first-adjacent station hurts one side: a clear imbalance tells which. Needs a capture at least 440 kHz wide around the station."));
    const auto& db = a.spec.dbfs;
    if (!a.engine.running() || db.size() < 64) { ImGui::TextDisabled("%s", tr("Start the receiver.")); return; }
    const double fs = a.engine.sampleRate(), binHz = fs / (double)db.size();
    const double offHz = a.freqMhz * 1e6 - a.engine.captureCentreHz();
    const int c = (int)std::lround((double)db.size() / 2 + offHz / binHz);
    const int h = (int)std::lround(240e3 / binHz);
    if (binHz > 2000 || c - h < 0 || c + h >= (int)db.size()) { ImGui::TextDisabled("%s", tr("The capture is too narrow around the station (needs +-240 kHz, bins under 2 kHz).")); return; }
    const std::vector<float> win(db.begin() + (c - h), db.begin() + (c + h + 1));
    const auto r = d4::hdrLevels(win, binHz);
    if (!r.ok) { ImGui::TextDisabled("%s", tr("No HD Radio sidebands found in the spectrum.")); return; }
    ImGui::Text("%s %.1f dBc, %s %.1f dBc, %s %.1f dB: %s", tr("lower"), r.lowerDbc, tr("upper"), r.upperDbc, tr("imbalance"), r.imbalanceDb, r.kind.c_str());
    if (!r.note.empty()) ImGui::TextDisabled("%s", r.note.c_str());
}
