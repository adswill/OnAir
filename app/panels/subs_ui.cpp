// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Subtitles over the TV picture: DVB subtitles (EN 300 743, core/src/tv/dvbsub.cpp) as a bitmap texture, teletext subtitle pages (EN 300 706)
// as boxed text, the on/off and language choice, and the character-cell drawing of a teletext page that the Teletext tab shares.
#include "../app.h"
#include "../transcribe/transcribe_app.h"
#include "dect2/tv/epg.h"
#include <cmath>

namespace {

std::string utf8(uint32_t u) {
    std::string s;
    if (u < 0x80) s += (char)u;
    else if (u < 0x800) { s += (char)(0xC0 | (u >> 6)); s += (char)(0x80 | (u & 0x3F)); }
    else { s += (char)(0xE0 | (u >> 12)); s += (char)(0x80 | ((u >> 6) & 0x3F)); s += (char)(0x80 | (u & 0x3F)); }
    return s;
}

// the subtitle page to show for a teletext option: its own page; the fallback one is 888, else 777 when that is the one sent
int ttxSubtitlePage(App& a, const SubOpt& o) {
    if (!o.fallback) return o.comp;
    TtxPage p;
    if (a.engine.teletext().page(888, p)) return 888;
    if (a.engine.teletext().page(777, p)) return 777;
    return 888;
}

void overlayThunk(void* ctx, ImDrawList* dl, ImVec2 p0, ImVec2 size) {
    App& a = *(App*)ctx;
    // generated captions (auto subtitles, core/src/transcribe/captions.h): never in place of the station's own ones unless the user chose so
    tx::captionsOverlay(a, dl, p0, size, !a.subOpts.empty());
    if (!tx::captionsShowBroadcast()) return;
    if (a.subIdx < 0 || !a.subsOn || a.subIdx >= (int)a.subOpts.size()) return;
    const SubOpt& o = a.subOpts[a.subIdx];
    if (!o.ttx) {
        if (a.video.subShow && a.video.sub) dl->AddImage(a.video.sub->texture(), p0, ImVec2(p0.x + size.x, p0.y + size.y));
        return;
    }
    TtxPage p;
    if (!a.engine.teletext().page(ttxSubtitlePage(a, o), p) || (p.rowMask >> 1) == 0 || !a.mono) return;
    TtxGrid g;
    ttxRender(p, g);
    // teletext is laid out for a 4:3 screen: 40 x 25 cells inside that, centred in the picture
    float gh = size.y, gw = gh * 4.f / 3.f;
    if (gw > size.x) { gw = size.x; gh = gw * 3.f / 4.f; }
    const float cw = gw / 40.f, ch = gh / 25.f;
    drawTtxGrid(dl, a.mono, ImVec2(p0.x + (size.x - gw) * 0.5f, p0.y + (size.y - gh) * 0.5f), cw, ch, g, true, false);
}

} // namespace

void drawTtxGrid(ImDrawList* dl, ImFont* f, ImVec2 o, float cw, float ch, const TtxGrid& g, bool overlay, bool reveal) {
    const bool flashOn = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
    for (int r = overlay ? 1 : 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            const TtxCell& cell = g[r][c];
            if (overlay && !cell.boxed) continue;
            const ImVec2 p0(o.x + c * cw, o.y + r * ch), p1(p0.x + cw, p0.y + ch);
            if (overlay) dl->AddRectFilled(p0, p1, cell.bg ? ttxColour(cell.bg, 0.9f) : IM_COL32(0, 0, 0, 215));
            else if (cell.bg) dl->AddRectFilled(p0, p1, ttxColour(cell.bg));
            if (cell.flash && !flashOn) continue;
            if (cell.conceal && !reveal) continue;
            if (cell.mosaic) {
                const float gx = cell.separated ? std::max(1.f, cw / 10.f) : 0.f;
                for (int k = 0; k < 6; k++) {
                    if (!(cell.sextants & (1 << k))) continue;
                    const int cx = k & 1, cy = k >> 1;
                    dl->AddRectFilled(ImVec2(p0.x + cx * cw / 2 + gx, p0.y + cy * ch / 3 + gx), ImVec2(p0.x + (cx + 1) * cw / 2 - gx, p0.y + (cy + 1) * ch / 3 - gx), ttxColour(cell.fg));
                }
            } else if (cell.ch > ' ') {
                const std::string t = utf8(cell.ch);
                const float fs = cell.dh ? ch * 2.0f * 0.8f : ch * 0.8f;
                if (cell.dh == 1) { dl->PushClipRect(p0, p1, true); dl->AddText(f, fs, ImVec2(p0.x, p0.y), ttxColour(cell.fg), t.c_str()); dl->PopClipRect(); }
                else if (cell.dh == 2) { dl->PushClipRect(p0, p1, true); dl->AddText(f, fs, ImVec2(p0.x, p0.y - ch), ttxColour(cell.fg), t.c_str()); dl->PopClipRect(); }
                else dl->AddText(f, fs, ImVec2(p0.x, p0.y), ttxColour(cell.fg), t.c_str());
            }
        }
}

void subsUpdate(App& a) {
    Player& pl = a.engine.player();
    pl.setSubtitles(false);   // subtitles are decoded here; the player's own burn-in into the picture stays off
    a.video.overlayFn = overlayThunk;
    a.video.overlayCtx = &a;

    // ---- the subtitle services of the playing programme, from its PMT
    a.subOpts.clear();
    int firstTtxPid = -1;
    const int sid = pl.selected();
    if (sid >= 0) for (const auto& sv : a.ts.services) {
        if (sv.id != sid) continue;
        for (const auto& st : sv.streams) {
            if (st.kind == "teletext" && firstTtxPid < 0) firstTtxPid = st.pid;
            for (const auto& e : st.subs) {
                SubOpt o;
                o.ttx = e.teletext; o.pid = st.pid; o.lang = e.lang;
                if (e.teletext) {
                    if (e.type != 2 && e.type != 5) continue;   // initial / information / schedule pages are not subtitles
                    o.comp = e.page;
                    o.label = "Teletext " + std::to_string(e.page) + (e.lang.empty() ? "" : "  " + e.lang) + (e.type == 5 ? "  (hearing impaired)" : "");
                } else {
                    o.comp = e.page; o.anc = e.ancPage;
                    o.label = "DVB" + (e.lang.empty() ? std::string() : "  " + e.lang) + (e.type >= 0x20 && e.type <= 0x24 ? "  (hearing impaired)" : "");
                }
                o.key = std::string(e.teletext ? "ttx:" : "dvb:") + e.lang + ":" + std::to_string(e.page) + ":" + std::to_string(e.type);
                a.subOpts.push_back(o);
            }
        }
        if (a.subOpts.empty() && firstTtxPid >= 0) {   // teletext without a subtitle entry: the usual subtitle page 888 (777 on some channels)
            SubOpt o; o.ttx = true; o.pid = firstTtxPid; o.fallback = true; o.comp = 888;
            o.label = "Teletext subtitles (page 888)"; o.key = "ttx::888:0";
            a.subOpts.push_back(o);
        }
        for (const auto& st : sv.streams) if (st.kind == "teletext" && st.subs.empty() && firstTtxPid < 0) firstTtxPid = st.pid;
    }
    // ---- the chosen one: the saved key, else the same kind and language, else the first
    int idx = -1;
    for (int i = 0; i < (int)a.subOpts.size() && idx < 0; i++) if (a.subOpts[i].key == a.subsPick) idx = i;
    if (idx < 0 && !a.subsPick.empty()) {
        const size_t c2 = a.subsPick.find(':', a.subsPick.find(':') + 1);
        const std::string kl = c2 == std::string::npos ? a.subsPick : a.subsPick.substr(0, c2);   // "dvb:eng"
        for (int i = 0; i < (int)a.subOpts.size() && idx < 0; i++) if (a.subOpts[i].key.compare(0, kl.size(), kl) == 0) idx = i;
        const std::string lg = kl.substr(kl.find(':') + 1);
        for (int i = 0; i < (int)a.subOpts.size() && idx < 0 && !lg.empty(); i++) if (a.subOpts[i].lang == lg) idx = i;
    }
    if (idx < 0 && !a.tvLang.empty())   // nothing remembered fits: the preferred language of the TV panel (#268)
        for (int i = 0; i < (int)a.subOpts.size() && idx < 0; i++) if (!a.subOpts[i].lang.empty() && epgLanguageKey(a.subOpts[i].lang) == epgLanguageKey(a.tvLang)) idx = i;
    if (idx < 0 && !a.subOpts.empty()) idx = 0;
    a.subIdx = idx;

    // ---- decoders: teletext follows the chosen teletext stream (or the first one), DVB subtitles run only while they are wanted
    int ttxPid = firstTtxPid;
    if (idx >= 0 && a.subOpts[idx].ttx) ttxPid = a.subOpts[idx].pid;
    a.engine.teletext().setPid(ttxPid);
    DvbSubDecoder& dv = a.engine.dvbSubtitles();
    if (idx >= 0 && !a.subOpts[idx].ttx && a.subsOn) {
        const SubOpt& o = a.subOpts[idx];
        dv.setStream(o.pid, o.comp, o.anc);
        auto d = dv.displayAt(a.video.pts);
        if (!d) a.video.subShow = false;
        else {
            if (d->seq != a.video.subSeq || !a.video.sub) {
                if (!a.video.sub || a.video.subW != d->dispW || a.video.subH != d->dispH) {
                    delete a.video.sub;
                    a.video.sub = gGfx->createVideo(d->dispW, d->dispH);
                    a.video.subW = d->dispW; a.video.subH = d->dispH;
                }
                std::vector<uint8_t> rgba;
                dvbSubComposite(*d, rgba);
                if (a.video.sub) a.video.sub->uploadRGBA(rgba.data());
                a.video.subSeq = d->seq;
            }
            a.video.subShow = a.video.sub != nullptr;
        }
    } else {
        dv.setStream(-1, 0, 0);
        a.video.subShow = false;
    }
}

void subsKey(App& a, int what) {
    if (what == 1) { a.subsOn = !a.subsOn; savePrefs(a); }
    else if (what == 2 && !a.subOpts.empty()) {
        a.subIdx = (a.subIdx + 1) % (int)a.subOpts.size();
        a.subsPick = a.subOpts[a.subIdx].key;
        a.subsOn = true;
        savePrefs(a);
    }
}

void subsControls(App& a) {
    if (a.subOpts.empty()) return;
    if (ImGui::Checkbox("subtitles", &a.subsOn)) savePrefs(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Subtitles over the picture (S in the pop-out window)");
    if (a.subOpts.size() > 1 || (a.subIdx >= 0 && !a.subOpts[a.subIdx].lang.empty())) {
        flowNext();
        ImGui::SetNextItemWidth(220 * gUi);
        const char* cur = a.subIdx >= 0 ? a.subOpts[a.subIdx].label.c_str() : "";
        if (ImGui::BeginCombo("##sublang", cur)) {
            for (int i = 0; i < (int)a.subOpts.size(); i++)
                if (ImGui::Selectable(a.subOpts[i].label.c_str(), i == a.subIdx)) { a.subIdx = i; a.subsPick = a.subOpts[i].key; a.subsOn = true; savePrefs(a); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Subtitle language (L in the pop-out window)");
    }
}
