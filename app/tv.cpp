// the TV side: player, service list, programme guide, teletext
#include "app.h"

void teletextTab(App& a) {
    TeletextDecoder& tx = a.engine.teletext();
    const Player& pl = a.engine.player();
    if (tx.pid() < 0) {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(pl.selected() < 0 ? "Play a service (Player tab) - teletext is read from the playing service." : "The playing service has no teletext stream."); ImGui::PopTextWrapPos(); }
        return;
    }
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::InputInt("page", &a.ttxPage, 0, 0);
    a.ttxPage = std::max(100, std::min(899, a.ttxPage));
    ImGui::SameLine(); if (ImGui::SmallButton("<")) a.ttxPage = std::max(100, a.ttxPage - 1);
    ImGui::SameLine(); if (ImGui::SmallButton(">")) a.ttxPage = std::min(899, a.ttxPage + 1);
    ImGui::SameLine(); if (ImGui::SmallButton("100")) a.ttxPage = 100;
    ImGui::SameLine();
    {
        auto pages = tx.pages();
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu pages received, %llu packets", pages.size(), (unsigned long long)tx.packets()); ImGui::PopTextWrapPos(); }
        if (!pages.empty()) {
            ImGui::SameLine();
            std::string l = "available: ";
            int n = 0;
            for (int p : pages) { if (n++ >= 14) { l += "..."; break; } l += std::to_string(p) + " "; }
            ImGui::TextDisabled("%s", l.c_str());
        }
    }
    TtxPage pg;
    if (!tx.page(a.ttxPage, pg)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("page %d has not been received yet (pages repeat every few seconds)", a.ttxPage); ImGui::PopTextWrapPos(); } return; }
    TtxGrid grid;
    ttxRender(pg, grid);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cw = std::max(6.f, std::min(avail.x / 40.f, avail.y / 25.f * 0.55f)), ch = cw / 0.55f;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + cw * 40, o.y + ch * 25), IM_COL32(0, 0, 0, 255));
    const bool flashOn = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            const TtxCell& cell = grid[r][c];
            const ImVec2 p0(o.x + c * cw, o.y + r * ch), p1(p0.x + cw, p0.y + ch);
            if (cell.bg) dl->AddRectFilled(p0, p1, ttxColour(cell.bg));
            if (cell.flash && !flashOn) continue;
            if (cell.mosaic) {
                const float gx = cell.separated ? 1.f : 0.f;
                for (int k = 0; k < 6; k++) {
                    if (!(cell.sextants & (1 << k))) continue;
                    const int cx = k & 1, cy = k >> 1;
                    dl->AddRectFilled(ImVec2(p0.x + cx * cw / 2 + gx, p0.y + cy * ch / 3 + gx), ImVec2(p0.x + (cx + 1) * cw / 2 - gx, p0.y + (cy + 1) * ch / 3 - gx), ttxColour(cell.fg));
                }
            } else if (cell.ch > ' ') {
                char buf[5] = {0};
                uint32_t u = cell.ch;
                if (u < 0x80) buf[0] = (char)u;
                else if (u < 0x800) { buf[0] = (char)(0xC0 | (u >> 6)); buf[1] = (char)(0x80 | (u & 0x3F)); }
                else { buf[0] = (char)(0xE0 | (u >> 12)); buf[1] = (char)(0x80 | ((u >> 6) & 0x3F)); buf[2] = (char)(0x80 | (u & 0x3F)); }
                ImFont* f = a.mono;
                const float fs = cell.dh ? ch * 2.0f * 0.8f : ch * 0.8f;
                if (cell.dh == 1) { dl->PushClipRect(p0, p1, true); dl->AddText(f, fs, ImVec2(p0.x, p0.y), ttxColour(cell.fg), buf); dl->PopClipRect(); }
                else if (cell.dh == 2) { dl->PushClipRect(p0, p1, true); dl->AddText(f, fs, ImVec2(p0.x, p0.y - ch), ttxColour(cell.fg), buf); dl->PopClipRect(); }
                else dl->AddText(f, fs, ImVec2(p0.x, p0.y), ttxColour(cell.fg), buf);
            }
        }
    char title[64];
    snprintf(title, sizeof title, "P%d%s", pg.number, pg.subtitle ? "  (subtitle page)" : "");
    dl->AddText(ImVec2(o.x + 2, o.y + 1), IM_COL32(255, 255, 255, 255), title);
    ImGui::Dummy(ImVec2(cw * 40, ch * 25));
}

const EpgEvent* epgCurrent(const App& a, int sid, const EpgEvent** next) {
    auto it = a.epg.find(sid);
    if (it == a.epg.end()) return nullptr;
    const int64_t now = utcNowOf(a);
    const EpgEvent* cur = nullptr;
    if (next) *next = nullptr;
    for (size_t i = 0; i < it->second.size(); i++) {
        const EpgEvent& e = it->second[i];
        if (e.start <= now && now < e.end()) { cur = &e; if (next && i + 1 < it->second.size()) *next = &it->second[i + 1]; break; }
        if (e.start > now) { if (next && !*next) *next = &e; break; }
    }
    return cur;
}

void guideTab(App& a) {
    const int64_t now = utcNowOf(a);
    if (a.ts.services.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("waiting for the service list..."); ImGui::PopTextWrapPos(); } return; }
    size_t total = 0;
    for (auto& kv : a.epg) total += kv.second.size();
    if (total == 0) {
        ImGui::TextWrapped("No programme information received yet. Broadcasters send it in the EIT tables, which repeat every few seconds (now/next) to a few minutes (schedule). "
                           "Some multiplexes send none at all.");
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("clock: %s%s", fmtLocal(now, "%a %d %b %H:%M").c_str(), a.ts.utcNow ? " (from the broadcast time signal)" : " (this computer's clock)"); ImGui::PopTextWrapPos(); }
        return;
    }
    if (a.guideSid < 0 || !a.epg.count(a.guideSid)) { a.guideSid = a.engine.player().selected() >= 0 && a.epg.count(a.engine.player().selected()) ? a.engine.player().selected() : a.epg.begin()->first; }
    ImGui::BeginChild("gsv", ImVec2(260 * gUi, 0), ImGuiChildFlags_Borders);
    for (auto& sv : a.ts.services) {
        auto it = a.epg.find(sv.id);
        if (it == a.epg.end()) continue;
        const EpgEvent* nx = nullptr;
        const EpgEvent* cur = epgCurrent(a, sv.id, &nx);
        ImGui::PushID(sv.id);
        if (ImGui::Selectable(sv.name.empty() ? std::to_string(sv.id).c_str() : sv.name.c_str(), a.guideSid == sv.id)) { a.guideSid = sv.id; a.guideEvent = -1; }
        ImGui::TextDisabled("  %s", cur ? (fmtLocal(cur->start, "%H:%M") + " " + cur->title).c_str() : "-");
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("gev", ImVec2(0, 0));
    const auto& evs = a.epg[a.guideSid];
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu events   local time %s", evs.size(), fmtLocal(now, "%a %d %b %H:%M").c_str()); ImGui::PopTextWrapPos(); }
    const float detailH = 150;
    ImGui::BeginChild("evlist", ImVec2(0, -detailH), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("evt", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("length", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("programme");
        std::string lastDay;
        for (size_t i = 0; i < evs.size(); i++) {
            const EpgEvent& e = evs[i];
            if (e.end() < now - 1800) continue; // hide what ended more than half an hour ago
            const bool cur = e.start <= now && now < e.end();
            const std::string day = fmtLocal(e.start, "%a %d %b");
            if (day != lastDay) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextColored(pal::heading(), "%s", day.c_str()); lastDay = day; }
            ImGui::TableNextRow();
            if (cur) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(40, 110, 60, 110));
            ImGui::TableNextColumn();
            char lbl[64]; snprintf(lbl, sizeof lbl, "%s - %s##%d", fmtLocal(e.start, "%H:%M").c_str(), fmtLocal(e.end(), "%H:%M").c_str(), e.eventId);
            if (ImGui::Selectable(lbl, a.guideEvent == e.eventId, ImGuiSelectableFlags_SpanAllColumns)) a.guideEvent = e.eventId;
            ImGui::TableNextColumn(); ImGui::Text("%d min", (e.duration + 30) / 60);
            ImGui::TableNextColumn();
            if (cur) { const float f = (float)(now - e.start) / (float)e.duration; ImGui::Text("%s", e.title.c_str()); ImGui::SameLine(); ImGui::TextDisabled("  now, %.0f%%", f * 100); }
            else ImGui::TextUnformatted(e.title.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::BeginChild("evdet", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const EpgEvent* sel = nullptr;
    for (auto& e : evs) if (e.eventId == a.guideEvent) sel = &e;
    if (!sel) sel = epgCurrent(a, a.guideSid);
    if (sel) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1), "%s", sel->title.c_str());
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s - %s  (%d min)%s%s", fmtLocal(sel->start, "%a %H:%M").c_str(), fmtLocal(sel->end(), "%H:%M").c_str(), (sel->duration + 30) / 60, genreName(sel->genre)[0] ? "   " : "", genreName(sel->genre)); ImGui::PopTextWrapPos(); }
        if (!sel->text.empty()) ImGui::TextWrapped("%s", sel->text.c_str());
        if (!sel->extended.empty()) ImGui::TextWrapped("%s", sel->extended.c_str());
    } else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("select a programme"); ImGui::PopTextWrapPos(); }
    ImGui::EndChild();
    ImGui::EndChild();
}

void playerTab(App& a) {
    Player& pl = a.engine.player();
    PlayerStats ps = pl.stats();
    {   // channel selector (muxes found by the scanner) and the signal-quality bar
        const SavedChannel* cur = nullptr;
        for (auto& c : a.channels) if (std::fabs(c.freqMhz - a.freqMhz) < 0.01) cur = &c;
        char fb[48]; snprintf(fb, sizeof fb, "%.3f MHz", a.freqMhz);
        const std::string curLab = cur ? channelLabel(*cur) : std::string(fb);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("channel");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::min(360 * gUi, ImGui::GetContentRegionAvail().x - 60 * gUi));   // room for the + and x buttons
        if (ImGui::BeginCombo("##chan", ((cur && cur->favourite) ? "* " + curLab : curLab).c_str())) {
            if (a.channels.empty()) ImGui::TextDisabled("no channels yet - run a scan (Scan tab)");
            for (size_t i = 0; i < a.channels.size(); i++) {
                const SavedChannel& c = a.channels[i];
                std::string l = (c.favourite ? "* " : "") + channelLabel(c);
                char extra[64]; snprintf(extra, sizeof extra, "   SNR %.0f dB", c.snrDb);
                if (ImGui::Selectable((l + extra).c_str(), cur == &c)) tuneToChannel(a, c);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.mode.c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+##fav")) { ImGui::OpenPopup("savefav"); if (!a.favName[0] && cur) snprintf(a.favName, sizeof a.favName, "%s", cur->name.c_str()); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the current frequency as a favourite channel");
        if (ImGui::BeginPopup("savefav")) {
            ImGui::TextDisabled("name for %.3f MHz", a.freqMhz);
            ImGui::SetNextItemWidth(220 * gUi);
            ImGui::InputText("##favn", a.favName, sizeof a.favName);
            if (ImGui::Button("Save")) {
                SavedChannel sc; sc.freqMhz = a.freqMhz; sc.bwMhz = kBw[a.bwIdx].mhz; sc.name = a.favName; sc.favourite = true;
                if (cur) { sc.mode = cur->mode; sc.snrDb = cur->snrDb; sc.nServices = cur->nServices; }
                bool f = false;
                for (auto& c : a.channels) if (std::fabs(c.freqMhz - sc.freqMhz) < 0.01) { c = sc; f = true; }
                if (!f) a.channels.push_back(sc);
                std::sort(a.channels.begin(), a.channels.end(), [](const SavedChannel& x, const SavedChannel& y) { return x.freqMhz < y.freqMhz; });
                a.favName[0] = 0; savePrefs(a); ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (cur) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x##del")) { const double f = cur->freqMhz; a.channels.erase(std::remove_if(a.channels.begin(), a.channels.end(), [&](const SavedChannel& c) { return std::fabs(c.freqMhz - f) < 0.01; }), a.channels.end()); savePrefs(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget this channel");
        }
        const float qMin = ImGui::CalcTextSize("signal quality 100%  excellent").x + 16 * gUi;
        sameLineIf(qMin, 18 * gUi);   // on a line of its own when the tab is narrow
        qualityBar(a, std::max(qMin, ImGui::GetContentRegionAvail().x - 8));
    }
    {   // PLP selector for multi-PLP multiplexes, and a notice for anything the receiver cannot decode
        const RxTelemetry& rx = a.rx;
        if (rx.plpList.size() > 1) {
            static const char* mods[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"};
            static const char* rates[] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "1/3", "2/5"};
            auto lab = [&](const RxTelemetry::PlpInfo& p) {
                char b[120];
                snprintf(b, sizeof b, "PLP %d  type %d  %s %s%s%s", p.id, p.type, p.mod >= 0 && p.mod < 4 ? mods[p.mod] : "?", p.cod >= 0 && p.cod < 8 ? rates[p.cod] : "?", p.type == 0 ? "  (common)" : "", p.supported ? "" : "  (not supported)");
                return std::string(b);
            };
            std::string cur = a.plpSel < 0 ? "automatic" : "PLP " + std::to_string(a.plpSel);
            if (a.plpSel < 0 && rx.plpSelectedId >= 0) cur += " (PLP " + std::to_string(rx.plpSelectedId) + ")";
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("PLP");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(260 * gUi);
            if (ImGui::BeginCombo("##plp", cur.c_str())) {
                if (ImGui::Selectable("automatic (first data PLP)", a.plpSel < 0)) { a.plpSel = -1; a.engine.selectPlp(-1); }
                for (auto& p : rx.plpList) if (ImGui::Selectable(lab(p).c_str(), a.plpSel == p.id)) { a.plpSel = p.id; a.engine.selectPlp(p.id); }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This multiplex carries several physical layer pipes. The receiver decodes one at a time.");
        }
        for (auto& u : rx.unsupported) { ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1), "! %s", u.c_str()); }
    }
    // the controls below the picture take what they took in the last frame (they wrap onto more lines in a narrow tab)
    ImGuiStorage* stor = ImGui::GetStateStorage();
    const ImGuiID ctlKey = ImGui::GetID("##tvctlh");
    const float h = std::max(60.f * gUi, ImGui::GetContentRegionAvail().y - stor->GetFloat(ctlKey, 44.f * gUi));
    ImGui::BeginChild("vbox", ImVec2(-1, h), ImGuiChildFlags_Borders);
    if (pl.selected() < 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Pick a service in the list on the right to start playback."); ImGui::PopTextWrapPos(); }
    else if (!a.video.has()) ImGui::TextDisabled("%s", ps.hasAudio && !ps.hasVideo ? "audio only" : ps.status.c_str());
    else a.video.draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    const float ctlY = ImGui::GetCursorPosY();
    if (ImGui::Button(pl.selected() >= 0 ? "Stop" : "Play")) { if (pl.selected() >= 0) pl.select(-1); else if (a.playReq < 0 && !a.ts.services.empty()) { int sid = a.ts.services[0].id; pl.select(sid); pl.setVolume(a.volume); } }
    flowNext();
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderFloat("volume", &a.volume, 0, 1, "%.2f")) pl.setVolume(a.volume);
    flowNext();
    if (ImGui::Checkbox("mute", &a.muted)) pl.setMuted(a.muted);
    flowNext();
    ImGui::Checkbox("pop out", &a.popOut);
    flowNext();
    if (ImGui::Button("Fullscreen")) { a.videoOnly = true; if (!glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Video fills the screen (press F or Esc to leave)");
    flowNext();
    ImGui::Checkbox("deinterlace", &a.video.deint);
    flowNext();
    auto tracks = pl.audioTracks();
    if (!tracks.empty()) {
        static int cur = 0;
        cur = std::min(cur, (int)tracks.size() - 1);
        std::string lab = tracks[cur].codec + " " + tracks[cur].lang;
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::BeginCombo("audio", lab.c_str())) {
            for (int i = 0; i < (int)tracks.size(); i++) {
                std::string l = tracks[i].codec + " " + tracks[i].lang + " (" + std::to_string(tracks[i].channels) + " ch)";
                if (ImGui::Selectable(l.c_str(), i == cur)) { cur = i; pl.setAudioTrack(i); }
            }
            ImGui::EndCombo();
        }
        flowNext();
    }
    if (pl.subtitlesAvailable()) { if (ImGui::Checkbox("subtitles", &a.subsOn)) pl.setSubtitles(a.subsOn); }
    flowBreak();
    if (pl.selected() >= 0) {   // what is on now, and what comes next
        const EpgEvent* nx = nullptr;
        const EpgEvent* cur = epgCurrent(a, pl.selected(), &nx);
        if (cur) {
            const int64_t now = utcNowOf(a);
            const float f = std::min(1.f, std::max(0.f, (float)(now - cur->start) / (float)cur->duration));
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1), "NOW");
            ImGui::SameLine();
            ImGui::Text("%s - %s  %s", fmtLocal(cur->start, "%H:%M").c_str(), fmtLocal(cur->end(), "%H:%M").c_str(), cur->title.c_str());
            char left[32]; snprintf(left, sizeof left, "%d min left", (int)std::max<int64_t>(0, (cur->end() - now + 30) / 60));
            sameLineIf(160 * gUi);
            ImGui::ProgressBar(f, ImVec2(160 * gUi, 12 * gUi), left);
            if (nx) { ImGui::TextDisabled("NEXT"); ImGui::SameLine(); ImGui::Text("%s  %s", fmtLocal(nx->start, "%H:%M").c_str(), nx->title.c_str()); }
        } else if (nx) {
            ImGui::TextDisabled("NEXT"); ImGui::SameLine(); ImGui::Text("%s  %s", fmtLocal(nx->start, "%H:%M").c_str(), nx->title.c_str());
        } else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no programme information is broadcast for this service"); ImGui::PopTextWrapPos(); }
    }
    stor->SetFloat(ctlKey, ImGui::GetCursorPosY() - ctlY);
}

void rightPanel(App& a) {
    if (const ModeUi* mu = modeUi(a.family)) if (mu->list) { mu->list(a); return; }
    const bool run = a.engine.running();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // ---- clock: broadcast time (TDT) when the stream has one, otherwise this computer's clock
    {
        const int64_t now = utcNowOf(a);
        time_t t = (time_t)now; struct tm m; dect2::gmTime(t, &m);
        char hm[16], ss[8], date[40];
        strftime(hm, sizeof hm, "%H:%M", &m); strftime(ss, sizeof ss, ":%S", &m); strftime(date, sizeof date, "%a %d %b %Y", &m);
        if (pal::dev()) {   // one plain line: 14:41:34 UTC
            ImGui::Text("%s%s", hm, ss);
            ImGui::SameLine(0, 6 * gUi);
            ImGui::TextDisabled("UTC");
        } else {
        ImGui::PushFont(a.ui, 40.f);
        ImGui::TextUnformatted(hm);
        ImGui::PopFont();
        ImGui::SameLine(0, 4 * gUi);
        ImGui::BeginGroup();
        ImGui::TextDisabled("UTC");
        ImGui::TextDisabled("%s", ss);
        ImGui::EndGroup();
        }
        ImGui::TextDisabled("%s", date);
        {   // the parts that do not fit go on the next line (a narrow list)
            const std::string loc = "local " + fmtLocal(now, "%H:%M"), src = a.ts.utcNow ? "(broadcast time)" : "(computer clock)";
            sameLineIf(ImGui::CalcTextSize(loc.c_str()).x, 10 * gUi);
            ImGui::TextDisabled("%s", loc.c_str());
            sameLineIf(ImGui::CalcTextSize(src.c_str()).x, 12 * gUi);
            ImGui::TextDisabled("%s", src.c_str());
        }
    }
    ImGui::Spacing();
    if (a.dabMode) { dabStations(a); return; }
    if (a.fmMode) { fmRadioPanel(a); return; }
    sectionHeader(Ic::Tv, "Services");
    // ---- service cards
    const float cardH = 68 * gUi;
    ImGui::BeginChild("svcs", ImVec2(0, std::max(140.f * gUi, ImGui::GetContentRegionAvail().y * 0.40f)));
    dl = ImGui::GetWindowDrawList(); // draw into the child so the cards are clipped and scroll with it
    if (a.ts.services.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(run ? "waiting for PAT / SDT..." : "start the receiver to see services"); ImGui::PopTextWrapPos(); }
    for (auto& sv : a.ts.services) {
        std::string vc, ac;
        double kb = 0;
        bool hasVideo = false;
        for (auto& st : sv.streams) { kb += st.kbps; if (st.kind == "video") { hasVideo = true; if (vc.empty()) vc = st.codec; } if (st.kind == "audio" && ac.empty()) ac = st.codec; }
        const bool sel = sv.id == a.engine.player().selected();
        ImGui::PushID(sv.id);
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##card", ImVec2(w, cardH));
        const bool hov = ImGui::IsItemHovered();
        if (clicked) { if (sel) a.engine.player().select(-1); else { a.engine.player().select(sv.id); a.engine.player().setVolume(a.volume); } }
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + cardH - 4), sel ? IM_COL32(30, 36, 40, 255) : hov ? IM_COL32(26, 27, 29, 255) : IM_COL32(16, 17, 19, 255), pal::rnd(4.f));
        dl->AddRect(p, ImVec2(p.x + w, p.y + cardH - 4), sel ? pal::remap(IM_COL32(115, 184, 209, 255)) : IM_COL32(48, 50, 52, 255), 4.f, 0, sel ? 1.6f : 1.f);
        // name and bit rate
        const std::string name = sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name;
        const bool isRadio = sv.type == 0x02 || sv.type == 0x0A || (!hasVideo && !ac.empty());
        if (!pal::dev()) icons::draw(isRadio ? Ic::Radio : Ic::Tv, ImVec2(p.x + 20 * gUi, p.y + 15 * gUi), 17.f * gUi, sel ? pal::remap(IM_COL32(120, 200, 255, 255)) : IM_COL32(120, 136, 156, 255), dl);
        char kbs[32]; fmtKbps(kbs, sizeof kbs, kb);
        const ImVec2 ks = ImGui::CalcTextSize(kbs);
        dl->AddText(a.ui, 16.f * gUi, ImVec2(p.x + 34 * gUi, p.y + 6 * gUi), IM_COL32(245, 247, 250, 255), ellipsize(name, w - 34 * gUi - ks.x - 18 * gUi, 16.f * gUi).c_str());
        dl->AddText(ImVec2(p.x + w - ks.x - 10 * gUi, p.y + 8 * gUi), IM_COL32(150, 158, 168, 255), kbs);
        // tag pills
        float x = p.x + 10 * gUi;
        const float ty = p.y + 27 * gUi;
        const bool radio = sv.type == 0x02 || sv.type == 0x0A || (!hasVideo && !ac.empty());
        x += tagAt(dl, ImVec2(x, ty), radio ? "radio" : hasVideo ? "TV" : sv.typeName(), IM_COL32(34, 74, 108, 255)) + 4;
        if (!vc.empty()) x += tagAt(dl, ImVec2(x, ty), vc.c_str(), IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255)) + 4;
        if (!ac.empty()) x += tagAt(dl, ImVec2(x, ty), ac.c_str(), IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255)) + 4;
        if (sv.caFlag) x += tagAt(dl, ImVec2(x, ty), "scrambled", IM_COL32(112, 52, 50, 255)) + 4;
        // what is on
        const EpgEvent* cur = epgCurrent(a, sv.id);
        std::string line;
        if (cur) line = fmtLocal(cur->start, "%H:%M") + "-" + fmtLocal(cur->end(), "%H:%M") + "  " + cur->title;
        else if (!sv.now.empty()) line = sv.now;
        else if (!sv.provider.empty()) line = sv.provider;
        dl->PushClipRect(ImVec2(p.x + 8 * gUi, p.y), ImVec2(p.x + w - 8 * gUi, p.y + cardH), true);
        dl->AddText(ImVec2(p.x + 10 * gUi, p.y + 47 * gUi), IM_COL32(128, 136, 146, 255), ellipsize(line, w - 18 * gUi).c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- what the player is doing, as label / value rows
    {
        const Player& pl = a.engine.player();
        const PlayerStats ps = pl.stats();
        sectionHeader(Ic::Play, "Player");
        if (pl.selected() < 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("click a service to play it"); ImGui::PopTextWrapPos(); }
        else if (ImGui::BeginTable("pkv", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 88);
            auto row = [&](Ic ic, const char* k, const char* fmt, auto... args) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(ic, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn();
                char b[120]; snprintf(b, sizeof b, fmt, args...); ImGui::TextUnformatted(b);
            };
            if (ps.hasVideo) row(Ic::Tv, "Video", "%s  %dx%d  %.1f fps  %s", ps.videoCodec.c_str(), ps.width, ps.height, ps.fps, ps.hardware ? "(hardware)" : "(software)");
            if (ps.hasAudio) row(Ic::Speaker, "Audio", "%s  %d ch", ps.audioCodec.c_str(), ps.audioChannels);
            row(Ic::Clock, "Buffer", "%.0f ms     A/V %+.0f ms", ps.audioBufferMs, ps.avOffsetMs);
            row(Ic::Camera, "Pictures", "%llu shown / %llu late", (unsigned long long)ps.shown, (unsigned long long)ps.late);
            row(Ic::Warning, "Errors", "%llu     underruns %d", (unsigned long long)ps.errors, ps.underruns);
            if (ps.repairEvents) row(Ic::Camera, "Repaired", "%llu damaged spans, %llu pictures replaced", (unsigned long long)ps.repairEvents, (unsigned long long)ps.repairedFrames);
            if (ps.concealEvents) row(Ic::Pulse, "Smoothed", "%llu gaps, %llu pictures generated (%s)", (unsigned long long)ps.concealEvents, (unsigned long long)ps.concealedFrames, ps.concealBackend.c_str());
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Pulse, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Gaps"); ImGui::TableNextColumn();
            { bool on = a.engine.player().conceal(); if (ImGui::Checkbox("smooth picture gaps", &on)) a.engine.player().setConceal(on);
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("When a fade swallows part of the stream or damages the pictures after it, generate the missing pictures\n(Apple ML interpolation) instead of freezing or showing smears. It hides the loss; it cannot bring the data back.\nThe sound is not changed: a dropout stays a clean silence."); }
#if defined(__APPLE__) || defined(_WIN32)
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Chip, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Decode"); ImGui::TableNextColumn();
            { bool on = a.engine.player().hardwareDecode(); if (ImGui::Checkbox("hardware video decoding", &on)) { a.engine.player().setHardwareDecode(on); plat::prefs().setB("hwVideo", on); plat::prefs().flush(); }
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use the graphics chip to decode the video. It saves processor time for the receiver.\nSwitch it off if the picture shows green or garbled patches (a graphics driver problem)."); }
#endif
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(a.muted ? Ic::Mute : Ic::Speaker, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Volume"); ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            float vol = a.volume * 100.f;
            if (ImGui::SliderFloat("##vol", &vol, 0, 100, "%.0f %%")) { a.volume = vol / 100.f; a.engine.player().setVolume(a.volume); }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    // ---- video preview and outputs
    {
        const float vw = ImGui::GetContentRegionAvail().x;
        const float vh = std::min(vw * 9.f / 16.f, std::max(60.f, ImGui::GetContentRegionAvail().y - 52.f));
        ImGui::BeginChild("video", ImVec2(0, vh), ImGuiChildFlags_Borders);
        const PlayerStats ps = a.engine.player().stats();
        if (a.engine.player().selected() < 0) ImGui::TextDisabled("no picture");
        else if (!ps.hasVideo && ps.hasAudio) ImGui::TextDisabled("audio only");
        else if (!a.video.has()) ImGui::TextDisabled("%s", ps.status.c_str());
        else a.video.draw(ImGui::GetContentRegionAvail());
        ImGui::EndChild();
    }
    {
        OutputStats os = a.engine.outputStats();
        iconLabel(Ic::Link, "Outputs", iconAccent());
        ImGui::SameLine(0, 10 * gUi);
        iconInline(Ic::File, os.fileOpen ? IM_COL32(120, 190, 235, 255) : IM_COL32(86, 94, 104, 255), 0.9f); ImGui::SameLine(0, 3 * gUi);
        ImGui::TextColored(os.fileOpen ? (pal::dev() ? pal::accent() : ImVec4(0.55f, 0.76f, 0.92f, 1)) : ImVec4(0.45f, 0.48f, 0.52f, 1), "file");
        ImGui::SameLine(0, 10 * gUi);
        iconInline(Ic::Globe, os.udpOpen ? IM_COL32(120, 190, 235, 255) : IM_COL32(86, 94, 104, 255), 0.9f); ImGui::SameLine(0, 3 * gUi);
        ImGui::TextColored(os.udpOpen ? (pal::dev() ? pal::accent() : ImVec4(0.55f, 0.76f, 0.92f, 1)) : ImVec4(0.45f, 0.48f, 0.52f, 1), "udp");
        if (os.fileOpen) { ImGui::SameLine(); ImGui::TextDisabled("%.1f MB", os.fileBytes / 1e6); }
        if (os.udpOpen) { ImGui::SameLine(); ImGui::TextDisabled("%s:%d%s", a.udpHost, a.out.port, a.out.rtp ? " RTP" : ""); }
    }
}

void tvTab(App& a) {
    subNav("tv", a.subTv, {"Player", "Guide", "Teletext"});
    if (a.subTv == 0) playerTab(a);
    else if (a.subTv == 1) guideTab(a);
    else teletextTab(a);
}

// channel estimate, impulse response and per-carrier SNR on one screen
void channelDashboard(App& a) {
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("chA", ImVec2(w * 0.5f - 4, h));
    channelTab(a);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("chB", ImVec2(0, h));
    ImGui::BeginChild("chB1", ImVec2(0, h * 0.5f - 2));
    impulseTab(a);
    ImGui::EndChild();
    ImGui::BeginChild("chB2", ImVec2(0, 0));
    snrTab(a);
    ImGui::EndChild();
    ImGui::EndChild();
}

