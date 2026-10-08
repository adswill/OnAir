// antenna aiming: direction finder and compass
#include "app.h"

void feedDirection(App& a) {
    DirectionFinder& d = a.dir;
    if (d.state() != DirectionFinder::State::Measuring) return;
    if (!a.engine.running()) return;
    DirSample s;
    const QualityReport& q = a.quality.report();
    s.locked = a.rx.dataValid && q.valid && q.percent > 0;
    s.qualityPct = s.locked ? q.percent : 0;
    s.snrDb = a.rx.dataValid ? a.rx.dataSnrDb : 0;
    s.lossPct = q.valid ? 100.0 * (1.0 - q.fecOk) : 100.0;
    s.multipath = a.mpd.report().level;
    s.clipFraction = a.spec.stats.clipFraction;
    s.occupancyDb = occupancyDb(a.spec.dbfs, a.tune.sampleRate / 1e6, a.tune.bandwidthMhz);
    d.addSample(ImGui::GetTime(), s);
}

void compassRose(App& a, ImVec2 size) {
    const DirectionFinder& d = a.dir;
    ImGui::InvisibleButton("##rose", size);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float R = std::min(size.x, size.y) * 0.5f - 22;
    const ImVec2 c(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    auto pt = [&](double hdg, float r) { const double th = hdg * M_PI / 180.0; return ImVec2(c.x + (float)std::sin(th) * r, c.y - (float)std::cos(th) * r); };
    dl->AddCircleFilled(c, R, IM_COL32(14, 17, 22, 255), 64);
    for (int k = 1; k <= 4; k++) dl->AddCircle(c, R * k / 4, IM_COL32(48, 56, 66, 255), 64, k == 4 ? 1.5f : 1.f);
    for (int k = 0; k < 16; k++) dl->AddLine(pt(k * 22.5, k % 2 ? R * 0.9f : R * 0.82f), pt(k * 22.5, R), IM_COL32(70, 80, 92, 255));
    for (int k = 0; k < 8; k++) dl->AddLine(c, pt(k * 45.0, R), IM_COL32(34, 40, 48, 255));
    const char* nm[4] = {"N", "E", "S", "W"};
    for (int k = 0; k < 4; k++) {
        const ImVec2 q = pt(k * 90.0, R + 12), ts = ImGui::CalcTextSize(nm[k]);
        dl->AddText(ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), k == 0 ? IM_COL32(240, 110, 90, 255) : IM_COL32(190, 198, 208, 255), nm[k]);
    }
    if (d.state() == DirectionFinder::State::Idle) return;
    if (d.kind() != AntennaKind::Omni) {
        // one spoke per measured heading, length = score
        for (const DirResult& r : d.results()) {
            const float L = R * (0.06f + 0.94f * (float)r.score / 100.f);
            dl->AddLine(c, pt(r.heading, L), scoreColour(r.score), 9.f);
            dl->AddCircleFilled(pt(r.heading, L), 6.f, scoreColour(r.score));
        }
        if (d.kind() == AntennaKind::Dipole)
            for (const DirResult& r : d.results()) dl->AddLine(c, pt(r.heading + 180, R * 0.06f + R * 0.94f * (float)r.score / 100.f), IM_COL32(100, 110, 122, 120), 2.f);
        if (d.state() != DirectionFinder::State::Done) {
            const float pulse = 0.55f + 0.45f * (float)std::sin(ImGui::GetTime() * 5);
            const ImU32 col = IM_COL32(255, 205, 60, (int)(255 * pulse));
            dl->AddLine(pt(d.targetHeading(), R * 0.15f), pt(d.targetHeading(), R), col, 3.f);
            const ImVec2 tip = pt(d.targetHeading(), R + 2), l = pt(d.targetHeading() + 5, R - 16), r2 = pt(d.targetHeading() - 5, R - 16);
            dl->AddTriangleFilled(tip, l, r2, col);
        } else if (d.recommendation().valid && !d.recommendation().flat) {
            const double h = d.recommendation().heading;
            const ImU32 col = IM_COL32(70, 230, 120, 255);
            dl->AddLine(c, pt(h, R - 14), col, 5.f);
            dl->AddTriangleFilled(pt(h, R + 2), pt(h + 7, R - 22), pt(h - 7, R - 22), col);
            if (d.recommendation().symmetric) dl->AddLine(c, pt(h + 180, R - 14), IM_COL32(70, 230, 120, 120), 3.f);
        }
    }
    dl->AddCircleFilled(c, 5.f, IM_COL32(220, 226, 234, 255));
}

void antennaTab(App& a) {
    DirectionFinder& d = a.dir;
    const DirectionFinder::State st = d.state();
    const bool running = a.engine.running();
    const bool inDevice = running && a.devices[a.devIdx].isRadio();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    const float leftW = std::max(300.f, w * 0.52f);

    ImGui::BeginChild("antL", ImVec2(leftW, h));
    if (st == DirectionFinder::State::Idle || st == DirectionFinder::State::Done) {
        { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.7f, 0.76f, 0.84f, 1), "Which antenna do you use?"); ImGui::PopTextWrapPos(); }
        ImGui::Spacing();
        const char* names[3] = {"Directional (Yagi, log-periodic, panel)", "Dipole / rabbit ears / loop (picks up two opposite directions)", "Omnidirectional (same in all directions)"};
        for (int i = 0; i < 3; i++) {   // a narrow pane: the part in brackets goes (shown on hover)
            const std::string lab = fitCaption(names[i], ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::RadioButton((lab + "##ant" + std::to_string(i)).c_str(), a.antKind == i)) a.antKind = i;
            if (lab != names[i] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", names[i]);
        }
        ImGui::Spacing();
        ImGui::TextWrapped("%s", a.antKind == 0 ? "You will be asked to point the antenna north, east, south and west first, then the directions in between, then in smaller steps around the best one. Use your own north: a compass or phone app helps, but any fixed reference works."
                                  : a.antKind == 1 ? "The antenna hears two opposite ends equally, so the search finds the line to aim along. You will be asked to turn it to several headings."
                                  : "An omnidirectional antenna has no direction to find. Instead you will be asked to try different places (windowsill, higher up, another room), and the best one is picked.");
        ImGui::Spacing();
        if (!running) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "Start the receiver on the channel you want to improve first."); ImGui::PopTextWrapPos(); }
        else if (!inDevice) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "This is a recording or the synthetic signal: turning an antenna will not change anything. Try it with a HackRF."); ImGui::PopTextWrapPos(); }
        ImGui::BeginDisabled(!running);
        if (ImGui::Button("  Start  ", ImVec2(130 * gUi, 0))) { d.start((AntennaKind)a.antKind); a.dirAgcWas = false; }
        ImGui::EndDisabled();
        if (st == DirectionFinder::State::Done) {
            const DirRecommendation& r = d.recommendation();
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            if (r.valid && !r.flat && a.antKind != 2) {
                ImGui::PushFont(a.ui, 26);
                { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1), "Point at %s  (%.0f\xC2\xB0)", r.label.c_str(), r.heading); ImGui::PopTextWrapPos(); }
                ImGui::PopFont();
                char cb[32]; snprintf(cb, sizeof cb, "confidence %.0f%%", r.confidence * 100);
                gaugePill(220, (float)r.confidence, r.confidence > 0.66 ? IM_COL32(40, 160, 90, 255) : r.confidence > 0.33 ? IM_COL32(200, 160, 40, 255) : IM_COL32(190, 70, 50, 255), cb);
            }
            ImGui::TextWrapped("%s", r.text.c_str());
        }
        if (!d.results().empty() && st == DirectionFinder::State::Done && ImGui::Button("Clear")) d.stop();
    } else {
        ImGui::PushFont(a.ui, 24);
        const std::string ins = d.instruction();
        ImGui::TextColored(st == DirectionFinder::State::WaitConfirm ? ImVec4(1, 0.82f, 0.25f, 1) : ImVec4(0.6f, 0.85f, 1, 1), "%s", ins.c_str());
        ImGui::PopFont();
        ImGui::Spacing();
        if (st == DirectionFinder::State::WaitConfirm) {
            ImGui::TextWrapped("Turn the antenna to this heading, hold it still, then press Measure. The receiver then listens for about %.0f seconds.", 14.0);
            ImGui::Spacing();
            if (ImGui::Button("  Measure  ", ImVec2(140 * gUi, 0))) { d.confirm(ImGui::GetTime()); if (a.agcOn) a.dirAgcWas = true; }
            ImGui::SameLine();
            if (ImGui::Button("Skip")) d.skip();
            if (d.kind() == AntennaKind::Omni && d.results().size() >= 2) { ImGui::SameLine(); if (ImGui::Button("That is enough: pick the best")) d.finishNow(); }
        } else {
            gaugePill(ImGui::GetContentRegionAvail().x - 8, (float)d.progress(ImGui::GetTime()), IM_COL32(52, 92, 108, 255), "measuring - do not touch the antenna");
            const QualityReport& q = a.quality.report();
            ImGui::Spacing();
            { ImGui::PushTextWrapPos(0); ImGui::Text("now: quality %.0f%%   SNR %.1f dB   %s", q.valid ? q.percent : 0.0, a.rx.dataValid ? a.rx.dataSnrDb : 0.f, a.rx.dataValid ? "locked" : "no lock"); ImGui::PopTextWrapPos(); }
            if (a.spec.stats.clipFraction > 0.005) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1), "ADC overload: lower the gain (AGC is paused during a measurement)"); ImGui::PopTextWrapPos(); }
        }
        ImGui::Spacing();
        if (ImGui::SmallButton("Stop and use what we have")) d.finishNow();
        ImGui::SameLine(); if (ImGui::SmallButton("Cancel")) d.stop();
        if (!running) { d.stop(); }
    }
    // results table
    ImGui::Spacing();
    if (!d.results().empty() && ImGui::BeginTable("dirres", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Heading"); ImGui::TableSetupColumn("Score"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("Lost"); ImGui::TableSetupColumn("Lock"); ImGui::TableSetupColumn("Multipath");
        ImGui::TableHeadersRow();
        const int best = d.recommendation().bestIndex;
        for (size_t i = 0; i < d.results().size(); i++) {
            const DirResult& r = d.results()[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (d.kind() == AntennaKind::Omni) ImGui::Text("%s", r.label.c_str()); else ImGui::Text("%s  %.0f\xC2\xB0", r.label.c_str(), r.heading);
            if ((int)i == best) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1), "best"); }
            ImGui::TableNextColumn(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(scoreColour(r.score)), "%.0f", r.score);
            ImGui::TableNextColumn(); if (r.lockFraction > 0.3) ImGui::Text("%.1f dB", r.snrDb); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.1f%%", r.lossPct);
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", r.lockFraction * 100);
            ImGui::TableNextColumn();
            const char* ml[] = {"-", "none", "mild", "likely", "severe"};
            const int mi = (int)r.multipath;
            ImGui::TextUnformatted(mi >= 0 && mi < 5 ? ml[mi] : "-");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("antR", ImVec2(0, h));
    if (a.antKind == 2 || d.kind() == AntennaKind::Omni) {
        if (d.kind() == AntennaKind::Omni && !d.results().empty()) {
            for (const DirResult& r : d.results()) {
                char t[48]; snprintf(t, sizeof t, "%s  score %.0f", r.label.c_str(), r.score);
                gaugePill(ImGui::GetContentRegionAvail().x - 10, (float)r.score / 100.f, scoreColour(r.score), t);
            }
        } else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Places you try will be compared here."); ImGui::PopTextWrapPos(); }
    } else {
        compassRose(a, ImGui::GetContentRegionAvail());
    }
    ImGui::EndChild();
}

