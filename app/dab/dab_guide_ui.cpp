// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB: the programme guide (SPI, ETSI TS 102 818 / TS 102 371). dabSpiTick() runs every frame in DAB mode: it starts the collection of the SPI data
// service of the ensemble (a packet mode sub-channel decoded next to the playing station), loads and saves the per-ensemble cache, and gives the station
// logos of the carousel to the logo cache. The Guide tab shows the progress of the acquisition, a services x time grid, a search, a details pane with the
// description and picture, and the XMLTV export. Now and next of a station come from dabSpiNowText().
#include "app.h"
#include "dect2/common/timecompat.h"
#include "dect2/dab/dab_spi.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace {

struct Tex { gfx::Image* img = nullptr; int w = 0, h = 0; };

struct Guide {
    uint16_t eid = 0;
    bool haveEid = false;
    bool collect = true;                 // collect the guide in the background
    int srcSub = -2, srcAddr = -1;       // what is being collected
    uint32_t srcSid = 0;
    bool fromCache = false;
    uint64_t savedSeq = 0;
    double lastSave = 0;
    uint64_t changeSeen = 0;
    double lastChangeT = -1;             // ImGui time of the last change of the data
    uint64_t imgSeq = 0;
    SpiSnapshot snap;
    uint64_t snapSeq = ~0ull;
    std::map<std::string, Tex> tex;      // pictures by ContentName
    std::map<std::string, bool> texMiss;
    uint32_t selSid = 0;
    std::string selKey;
    int64_t selStart = 0;
    char search[96] = "";
    float pxHour = 220.f;
    bool jumpNow = true;
    bool lastBy = false;
    std::string msg;                     // the result of the last export
    std::string msgPath;
    std::string cacheDir;
};

Guide& G() { static Guide g; return g; }

int64_t nowUtc(const DabEnsemble& ens) { return ens.utc ? ens.utc : (int64_t)time(nullptr); }

std::string fmtClock(int64_t utc, int offMin) {
    const time_t t = (time_t)(utc + offMin * 60);
    struct tm m;
    dect2::gmTime(t, &m);
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d", m.tm_hour, m.tm_min);
    return b;
}
std::string fmtDay(int64_t utc, int offMin) {
    const time_t t = (time_t)(utc + offMin * 60);
    struct tm m;
    dect2::gmTime(t, &m);
    char b[32];
    strftime(b, sizeof b, "%a %d %b", &m);
    return b;
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// The SPI data service of the ensemble: a packet mode component (its sub-channel and address) or the PAD of an audio component (address -2)
bool spiSource(const DabEnsemble& ens, uint32_t& sid, int& sub, int& addr, bool& pad) {
    for (const auto& kv : ens.services) {
        const DabService& sv = kv.second;
        if (!sv.spi) continue;
        for (size_t i = 0; i < sv.comps.size(); i++) {
            const DabComponent& c = sv.comps[i];
            if (c.tmid == 3) {
                auto it = ens.packet.find(c.scid);
                if (it == ens.packet.end() || it->second.subId < 0 || !ens.subs.count(it->second.subId)) continue;
                sid = sv.sid; sub = it->second.subId; addr = it->second.address; pad = false;
                return true;
            }
        }
        for (const DabComponent& c : sv.comps) if (c.tmid == 0) { sid = sv.sid; sub = c.subId; addr = -2; pad = true; return true; }
    }
    return false;
}

void freeTex(Guide& g) {
    for (auto& kv : g.tex) delete kv.second.img;
    g.tex.clear(); g.texMiss.clear();
}

// A texture for the picture a SPI object carries under that ContentName; null when it has not arrived or cannot be decoded
Tex* picture(App& a, Guide& g, const std::string& name) {
    if (!gGfx || name.empty() || name.compare(0, 4, "http") == 0) return nullptr;
    auto it = g.tex.find(name);
    if (it != g.tex.end()) return &it->second;
    if (g.texMiss.count(name) && g.texMiss[name]) return nullptr;
    int sub; std::vector<uint8_t> data;
    if (!a.engine.dabSpi().image(name, sub, data)) return nullptr;
    Tex t;
    std::vector<uint8_t> rgba;
    if (!dect2::decodeSlideImage(data.data(), data.size(), sub, t.w, t.h, rgba) || t.w <= 0 || t.h <= 0 || rgba.size() < (size_t)t.w * (size_t)t.h * 4) { g.texMiss[name] = true; return nullptr; }
    t.img = gGfx->createImage(t.w, t.h, 0xFF000000u);
    t.img->update(0, 0, t.w, t.h, reinterpret_cast<const uint32_t*>(rgba.data()));
    return &(g.tex[name] = t);
}

// The logo of a service the carousel points to, handed to the logo cache of the station list
void feedLogos(App& a, Guide& g, const DabEnsemble& ens) {
    if (!gGfx) return;
    const uint64_t is = a.engine.dabSpi().progress().changeCount;
    if (is == g.imgSeq) return;
    g.imgSeq = is;
    static const char* order[] = {"logo_colour_square", "logo_unrestricted", "logo_colour_rectangle"};
    for (const auto& kv : g.snap.services) {
        const SpiService& s = kv.second;
        for (const char* want : order) {
            bool done = false;
            for (const SpiMedia& m : s.media) {
                if (m.type != want || m.url.empty() || m.url.compare(0, 4, "http") == 0) continue;
                int sub; std::vector<uint8_t> data;
                if (!a.engine.dabSpi().image(m.url, sub, data)) continue;
                int w, h; std::vector<uint8_t> rgba;
                if (!dect2::decodeSlideImage(data.data(), data.size(), sub, w, h, rgba) || w <= 0 || h <= 0 || rgba.size() < (size_t)w * (size_t)h * 4) continue;
                dabLogoSet(ens.eid, s.sid, w, h, reinterpret_cast<const uint32_t*>(rgba.data()));
                done = true;
                break;
            }
            if (done) break;
        }
    }
}

void refreshSnap(App& a, Guide& g) {
    const uint64_t s = a.engine.dabSpi().store().seq();
    if (s == g.snapSeq) return;
    g.snap = a.engine.dabSpi().store().snapshot();
    g.snapSeq = s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- per frame
bool dabHasSpi(const DabEnsemble& ens) {
    for (const auto& kv : ens.services) if (kv.second.spi) return true;
    return false;
}

void dabSpiTick(App& a) {
    Guide& g = G();
    if (!a.dabMode || !a.engine.running()) return;
    const DabEnsemble ens = a.engine.dabEnsemble();
    if (!ens.valid) return;
    if (!g.haveEid || g.eid != ens.eid) {          // another ensemble: the cache of the previous one is written, this one's is read
        if (g.haveEid && a.engine.dabSpi().store().seq() != g.savedSeq && g.cacheDir.size()) dect2::spiSave(a.engine.dabSpi().store(), g.cacheDir, g.eid);
        g.cacheDir = plat::dataDir() + "/dab-spi";
        g.eid = ens.eid; g.haveEid = true;
        g.srcSub = -2; g.srcAddr = -1; g.snapSeq = ~0ull; g.fromCache = false; g.lastChangeT = -1; g.selSid = 0; g.selKey.clear();
        freeTex(g);
        a.engine.dabSpi().clearStore();
        a.engine.dabSpi().reset();
        dect2::spiCleanCache(g.cacheDir, nowUtc(ens));
        g.fromCache = dect2::spiLoad(a.engine.dabSpi().store(), g.cacheDir, ens.eid, nowUtc(ens));
        g.savedSeq = a.engine.dabSpi().store().seq();
    }
    uint32_t sid = 0; int sub = -1, addr = -1; bool pad = false;
    const bool have = spiSource(ens, sid, sub, addr, pad);
    int wantSub = -1, wantAddr = -1;
    if (g.collect && have) {
        if (!pad) { wantSub = sub; wantAddr = addr; }
        else if (a.engine.dabSelected() == sub) { wantSub = sub; wantAddr = -2; }   // in the PAD of the playing station
    }
    if (wantSub != g.srcSub || wantAddr != g.srcAddr) {
        g.srcSub = wantSub; g.srcAddr = wantAddr; g.srcSid = sid;
        a.engine.dabSelectSpi(wantSub, wantAddr);
    }
    dect2::SpiStore& st = a.engine.dabSpi().store();
    const double now = ImGui::GetTime();
    const uint64_t ch = a.engine.dabSpi().progress().changeCount;
    if (ch != g.changeSeen) { g.changeSeen = ch; g.lastChangeT = now; g.fromCache = false; }
    if (st.seq() != g.savedSeq && now - g.lastSave > 30.0 && g.cacheDir.size()) {
        st.prune(nowUtc(ens));
        dect2::spiSave(st, g.cacheDir, ens.eid);
        g.savedSeq = st.seq(); g.lastSave = now;
    }
    if (st.seq() != g.snapSeq) { g.snap = st.snapshot(); g.snapSeq = g.snap.seq; }
    feedLogos(a, g, ens);
}

// "Now: title (until 21:00)   Next: title (21:00)" for a service, "" when the guide knows nothing about it
std::string dabSpiNowText(App& a, uint32_t sid) {
    Guide& g = G();
    if (!g.haveEid) return "";
    const DabEnsemble ens = a.engine.dabEnsemble();
    const dect2::SpiNowNext nn = a.engine.dabSpi().store().nowNext(sid, nowUtc(ens));
    if (!nn.haveNow && !nn.haveNext) return "";
    auto title = [&](const dect2::SpiEvent& e) {
        auto it = g.snap.programmes.find(e.progKey);
        return it == g.snap.programmes.end() ? std::string("?") : it->second.title();
    };
    std::string s;
    if (nn.haveNow) s += title(nn.now) + (nn.now.duration > 0 ? "  (" + std::string(tr("until")) + " " + fmtClock(nn.now.start + nn.now.duration, nn.now.offMin) + ")" : "");
    if (nn.haveNext) s += std::string(s.empty() ? "" : "   ") + tr("Next") + ": " + title(nn.next) + " (" + fmtClock(nn.next.start, nn.next.offMin) + ")";
    return s;
}

// ---------------------------------------------------------------------------------------------------------------- the tab
namespace {

std::string exportXmltv(App& a, Guide& g) {
    const std::string dir = plat::dataDir() + "/dab-guide";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char nm[64];
    snprintf(nm, sizeof nm, "/dab-guide-%04x.xml", (unsigned)g.eid);
    const std::string path = dir + nm;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return "";
    const std::string x = a.engine.dabSpi().store().toXmltv();
    f.write(x.data(), (std::streamsize)x.size());
    return f ? path : "";
}

const dect2::SpiProgramme* progOf(const Guide& g, const dect2::SpiEvent& e) {
    auto it = g.snap.programmes.find(e.progKey);
    return it == g.snap.programmes.end() ? nullptr : &it->second;
}

void select(Guide& g, const dect2::SpiEvent& e) { g.selSid = e.sid; g.selKey = e.progKey; g.selStart = e.start; }

void details(App& a, Guide& g, const DabEnsemble& ens) {
    const dect2::SpiEvent* ev = nullptr;
    for (const dect2::SpiEvent& e : g.snap.events) if (e.sid == g.selSid && e.start == g.selStart && e.progKey == g.selKey) { ev = &e; break; }
    if (!ev) { ImGui::TextDisabled("%s", tr("Click a programme in the grid or the search results to see its details.")); return; }
    const dect2::SpiProgramme* p = progOf(g, *ev);
    if (!p) return;
    Tex* img = nullptr;
    for (const dect2::SpiMedia& m : p->media) { img = picture(a, g, m.url); if (img) break; }
    const float imgW = img ? std::min(180.f * gUi, 0.3f * ImGui::GetContentRegionAvail().x) : 0.f;
    ImGui::BeginGroup();
    ImGui::PushFont(a.ui, 18.f);
    ImGui::TextUnformatted(p->title().empty() ? "?" : p->title().c_str());
    ImGui::PopFont();
    std::string sname;
    auto sv = g.snap.services.find(ev->sid);
    if (sv != g.snap.services.end()) sname = sv->second.display();
    if (sname.empty()) { auto e2 = ens.services.find(ev->sid); if (e2 != ens.services.end()) sname = e2->second.label; }
    char when[160];
    snprintf(when, sizeof when, "%s  %s", fmtDay(ev->start, ev->offMin).c_str(), fmtClock(ev->start, ev->offMin).c_str());
    std::string line = sname + "   " + when;
    if (ev->duration > 0) line += " - " + fmtClock(ev->start + ev->duration, ev->offMin) + "  (" + std::to_string(ev->duration / 60) + " min)";
    ImGui::TextDisabled("%s", line.c_str());
    if (!p->longName.empty() && p->longName != p->title()) ImGui::TextUnformatted(p->longName.c_str());
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    const std::string d = p->description();
    if (!d.empty()) ImGui::TextUnformatted(d.c_str());
    else ImGui::TextDisabled("%s", tr("No description in the guide."));
    if (!p->shortDesc.empty() && !p->longDesc.empty() && p->shortDesc != p->longDesc) { ImGui::Spacing(); ImGui::TextDisabled("%s", p->shortDesc.c_str()); }
    ImGui::PopTextWrapPos();
    if (!p->genres.empty()) { std::string gs; for (const std::string& x : p->genres) gs += (gs.empty() ? "" : ", ") + x; ImGui::TextDisabled("%s: %s", tr("Genre"), gs.c_str()); }
    if (!p->lang.empty()) ImGui::TextDisabled("%s: %s", tr("Language"), p->lang.c_str());
    for (const std::string& l : p->links) if (l.compare(0, 4, "http") == 0) { if (ImGui::SmallButton(l.c_str())) dect2::openUrl(l); break; }
    // listen
    auto e2 = ens.services.find(ev->sid);
    if (e2 != ens.services.end()) {
        const DabComponent* ac = e2->second.playable();
        if (ac && ImGui::SmallButton(tr("Listen to this station"))) dabSelectStation(a, ac->subId, true);
    }
    ImGui::EndGroup();
    if (img) {
        ImGui::SameLine(0, 12 * gUi);
        const float sc = std::min(imgW / (float)img->w, 220.f * gUi / (float)img->h);
        ImGui::Image(img->img->texture(), ImVec2((float)img->w * sc, (float)img->h * sc));
    }
}

}  // namespace

void dabGuideTab(App& a) {
    Guide& g = G();
    const DabEnsemble ens = a.engine.dabEnsemble();
    dabSpiTick(a);
    if (ens.services.empty()) { ImGui::TextDisabled("%s", tr("Start the receiver on a DAB channel to see the guide.")); return; }
    const int64_t now = nowUtc(ens);
    dect2::SpiStore& st = a.engine.dabSpi().store();
    const dect2::DabSpiDecoder::Progress pr = a.engine.dabSpi().progress();
    const dect2::SpiStats ss = st.stats();
    uint32_t sid; int sub, addr; bool pad;
    const bool signalled = spiSource(ens, sid, sub, addr, pad);

    // ---- status and progress
    ImGui::PushTextWrapPos(0);
    if (!signalled && g.snap.services.empty()) {
        ImGui::TextDisabled("%s", tr("This ensemble does not announce a programme guide (SPI, user application 0x007)."));
        ImGui::PopTextWrapPos();
        return;
    }
    if (ImGui::Checkbox(tr("Collect the guide in the background"), &g.collect)) {}
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) { a.engine.dabSpi().clearStore(); a.engine.dabSpi().reset(); g.snapSeq = ~0ull; freeTex(g); g.selKey.clear(); }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Export XMLTV"))) {
        const std::string p = exportXmltv(a, g);
        g.msgPath = p;
        g.msg = p.empty() ? tr("Could not write the file.") : std::string(tr("Saved")) + ": " + p;
    }
    if (!g.msg.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g.msg.c_str());
        if (!g.msgPath.empty()) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Open folder"))) dect2::openUrl(plat::dataDir() + "/dab-guide"); }
    }
    if (g.collect && g.srcSub < 0 && signalled && pad) ImGui::TextDisabled("%s", tr("The guide of this ensemble is sent in the PAD of a station: play that station to collect it."));
    {
        char l[256];
        if (pr.expected > 0) snprintf(l, sizeof l, "%s: %d / %d %s (%.0f %%)", tr("Guide data"), pr.have, pr.expected, tr("objects"), pr.percent);
        else if (g.srcSub >= 0) snprintf(l, sizeof l, "%s: %s", tr("Guide data"), tr("waiting for the carousel directory..."));
        else snprintf(l, sizeof l, "%s: %s", tr("Guide data"), g.fromCache ? tr("from the cache") : tr("not collecting"));
        ImGui::ProgressBar(pr.percent >= 0 ? (float)(pr.percent / 100.0) : 0.f, ImVec2(std::max(160.f * gUi, ImGui::GetContentRegionAvail().x * 0.5f), 0), l);
        ImGui::SameLine();
        char u[160];
        if (g.lastChangeT >= 0) { const int s = (int)(ImGui::GetTime() - g.lastChangeT); snprintf(u, sizeof u, "%s %d:%02d", tr("last update"), s / 60, s % 60); }
        else snprintf(u, sizeof u, "%s", g.fromCache ? tr("cached data, expires with time") : tr("no update yet"));
        ImGui::TextDisabled("%s", u);
        ImGui::TextDisabled("%zu %s, %zu %s, %zu %s   (%llu %s)", g.snap.services.size(), tr("services"), g.snap.programmes.size(), tr("programmes"), g.snap.events.size(), tr("scheduled"),
                            (unsigned long long)ss.objects, tr("objects decoded"));
    }
    if (g.snap.events.empty()) {
        if (pr.expected > 0 && pr.have >= pr.expected) ImGui::TextDisabled("%s", tr("The broadcaster sends service information (names, logos) but no programme schedule."));
        else ImGui::TextDisabled("%s", tr("No programme schedule received yet. A guide carousel can take several minutes to come round."));
    }
    ImGui::PopTextWrapPos();
    if (g.snap.events.empty()) return;

    // ---- search
    ImGui::SetNextItemWidth(std::max(140.f * gUi, ImGui::GetContentRegionAvail().x * 0.4f));
    ImGui::InputTextWithHint("##gsearch", tr("search title or description"), g.search, sizeof g.search);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Now"))) g.jumpNow = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::SliderFloat("##gzoom", &g.pxHour, 90.f, 520.f, "%.0f px/h");

    const float detailH = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y * 0.34f);
    const float gridH = std::max(100.f * gUi, ImGui::GetContentRegionAvail().y - detailH - 8 * gUi);
    const std::string q = lower(g.search);
    if (!q.empty()) {
        ImGui::BeginChild("gsres", ImVec2(0, gridH), ImGuiChildFlags_Borders);
        int shown = 0;
        for (const dect2::SpiEvent& e : g.snap.events) {
            if (e.start + std::max(e.duration, 1) < now - 3600) continue;
            const dect2::SpiProgramme* p = progOf(g, e);
            if (!p) continue;
            if (lower(p->title()).find(q) == std::string::npos && lower(p->description()).find(q) == std::string::npos && lower(p->longName).find(q) == std::string::npos) continue;
            auto sv = g.snap.services.find(e.sid);
            char row[320];
            snprintf(row, sizeof row, "%s %s  %s  -  %s", fmtDay(e.start, e.offMin).c_str(), fmtClock(e.start, e.offMin).c_str(), sv != g.snap.services.end() ? sv->second.display().c_str() : "?", p->title().c_str());
            ImGui::PushID(shown);
            if (ImGui::Selectable(row, g.selKey == e.progKey && g.selStart == e.start && g.selSid == e.sid)) select(g, e);
            ImGui::PopID();
            if (++shown >= 300) break;
        }
        if (!shown) ImGui::TextDisabled("%s", tr("Nothing matches."));
        ImGui::EndChild();
    } else {
        // ---- the grid: services as rows, time to the right
        std::vector<uint32_t> sids;
        {
            std::map<uint32_t, bool> seen;
            for (const dect2::SpiEvent& e : g.snap.events) if (!seen[e.sid]) { seen[e.sid] = true; sids.push_back(e.sid); }
        }
        auto nameOf = [&](uint32_t s) {
            auto sv = g.snap.services.find(s);
            std::string n = sv != g.snap.services.end() ? sv->second.display() : "";
            if (n.empty()) { auto e2 = ens.services.find(s); if (e2 != ens.services.end()) n = e2->second.label; }
            if (n.empty()) { char b[16]; snprintf(b, sizeof b, "%04X", (unsigned)s); n = b; }
            return n;
        };
        std::sort(sids.begin(), sids.end(), [&](uint32_t x, uint32_t y) { return nameOf(x) < nameOf(y); });
        int64_t tmin = INT64_MAX, tmax = 0;
        for (const dect2::SpiEvent& e : g.snap.events) { tmin = std::min(tmin, e.start); tmax = std::max(tmax, e.start + std::max(e.duration, 600)); }
        const int off = g.snap.events.front().offMin;
        const int64_t t0 = std::min(tmin, now - 1800) / 1800 * 1800;
        const int64_t t1 = std::max(tmax, now + 7200);
        const float rowH = 34.f * gUi, nameW = 150.f * gUi, headH = 22.f * gUi, ph = g.pxHour * gUi;
        const float totalW = nameW + (float)(t1 - t0) / 3600.f * ph + 20.f;
        ImGui::BeginChild("gsgrid", ImVec2(0, gridH), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float scrollX = ImGui::GetScrollX();
        ImGui::Dummy(ImVec2(totalW, headH + rowH * (float)sids.size()));
        const ImVec2 win = ImGui::GetWindowPos();
        const float viewW = ImGui::GetWindowWidth();
        if (g.jumpNow) { ImGui::SetScrollX(std::max(0.f, (float)(now - t0 - 900) / 3600.f * ph)); g.jumpNow = false; }
        auto xOf = [&](int64_t t) { return o.x + nameW + (float)(t - t0) / 3600.f * ph; };
        const ImU32 cText = ImGui::GetColorU32(ImGuiCol_Text), cDim = ImGui::GetColorU32(ImGuiCol_TextDisabled), cBg = ImGui::GetColorU32(ImGuiCol_ChildBg);
        // hour ticks
        for (int64_t t = t0; t <= t1; t += 1800) {
            const float x = xOf(t);
            if (x < win.x + nameW - 40 || x > win.x + viewW + 40) continue;
            dl->AddLine(ImVec2(x, o.y + headH), ImVec2(x, o.y + headH + rowH * (float)sids.size()), IM_COL32(255, 255, 255, (t % 3600 == 0) ? 28 : 12));
            if (t % 3600 == 0) dl->AddText(ImVec2(x + 3, o.y + 2), cDim, fmtClock(t, off).c_str());
        }
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool click = ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered();
        const dect2::SpiEvent* hover = nullptr;
        for (size_t r = 0; r < sids.size(); r++) {
            const float y = o.y + headH + (float)r * rowH;
            if (y + rowH < win.y || y > win.y + ImGui::GetWindowHeight()) continue;
            if (r & 1) dl->AddRectFilled(ImVec2(o.x + nameW, y), ImVec2(o.x + totalW, y + rowH), IM_COL32(255, 255, 255, 6));
        }
        for (const dect2::SpiEvent& e : g.snap.events) {
            const size_t r = (size_t)(std::find(sids.begin(), sids.end(), e.sid) - sids.begin());
            if (r >= sids.size()) continue;
            const float y = o.y + headH + (float)r * rowH;
            if (y + rowH < win.y || y > win.y + ImGui::GetWindowHeight()) continue;
            const int dur = e.duration > 0 ? e.duration : 1800;
            const float x0 = xOf(e.start), x1 = xOf(e.start + dur);
            if (x1 < win.x + nameW || x0 > win.x + viewW) continue;
            const ImVec2 a0(std::max(x0, win.x + nameW) + 1, y + 2), a1(x1 - 1, y + rowH - 2);
            if (a1.x <= a0.x) continue;
            const bool on = e.start <= now && now < e.start + dur;
            const bool sel = g.selSid == e.sid && g.selStart == e.start && g.selKey == e.progKey;
            const bool hov = mouse.x >= a0.x && mouse.x < a1.x && mouse.y >= a0.y && mouse.y < a1.y && ImGui::IsWindowHovered();
            if (hov) hover = &e;
            dl->AddRectFilled(a0, a1, sel ? IM_COL32(60, 110, 150, 255) : on ? IM_COL32(40, 80, 60, 255) : hov ? IM_COL32(52, 56, 62, 255) : IM_COL32(36, 40, 46, 255), 3.f);
            dl->AddRect(a0, a1, sel ? IM_COL32(130, 190, 230, 255) : IM_COL32(64, 68, 74, 255), 3.f);
            const dect2::SpiProgramme* p = progOf(g, e);
            const std::string title = p ? p->title() : "?";
            dl->PushClipRect(a0, a1, true);
            dl->AddText(ImVec2(a0.x + 4, a0.y + 1), cText, title.c_str());
            dl->AddText(ImVec2(a0.x + 4, a0.y + 1 + ImGui::GetTextLineHeight()), cDim, fmtClock(e.start, e.offMin).c_str());
            dl->PopClipRect();
        }
        if (click && hover) select(g, *hover);
        if (hover && ImGui::BeginTooltip()) {
            const dect2::SpiProgramme* p = progOf(g, *hover);
            ImGui::TextUnformatted(p ? p->title().c_str() : "?");
            ImGui::TextDisabled("%s - %s", fmtClock(hover->start, hover->offMin).c_str(), hover->duration > 0 ? fmtClock(hover->start + hover->duration, hover->offMin).c_str() : "?");
            ImGui::EndTooltip();
        }
        // now line
        { const float x = xOf(now); dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + headH + rowH * (float)sids.size()), IM_COL32(255, 90, 70, 200), 1.5f); }
        // the names stay at the left edge while the grid scrolls
        for (size_t r = 0; r < sids.size(); r++) {
            const float y = o.y + headH + (float)r * rowH;
            if (y + rowH < win.y || y > win.y + ImGui::GetWindowHeight()) continue;
            const float x = o.x + scrollX;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + nameW, y + rowH), cBg);
            dl->AddLine(ImVec2(x + nameW, y), ImVec2(x + nameW, y + rowH), IM_COL32(255, 255, 255, 30));
            dl->PushClipRect(ImVec2(x, y), ImVec2(x + nameW - 2, y + rowH), true);
            dl->AddText(ImVec2(x + 4, y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), cText, nameOf(sids[r]).c_str());
            dl->PopClipRect();
        }
        dl->AddRectFilled(ImVec2(o.x + scrollX, o.y), ImVec2(o.x + scrollX + nameW, o.y + headH), cBg);
        dl->AddText(ImVec2(o.x + scrollX + 4, o.y + 2), cDim, fmtDay(now, off).c_str());
        ImGui::EndChild();
    }
    ImGui::BeginChild("gsdet", ImVec2(0, 0), ImGuiChildFlags_Borders);
    details(a, g, ens);
    ImGui::EndChild();
}
