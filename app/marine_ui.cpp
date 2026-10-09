// Marine screens: the NAVTEX messages grouped by station with the text panel, the DSC calls (distress in red, the position on a small
// map), the weather fax picture (zoom, pan, slant, save as PNG) and a tuning aid (the audio spectrum with the tones each service expects).
#include "app.h"
#include "adsb_map.h"
#include "dect2/marine_tel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace {

using dect2::DscCall;
using dect2::NavtexMessage;

struct State {
    bool loaded = false;
    int view = 0;                    // 0 NAVTEX, 1 DSC, 2 fax, 3 tuning
    bool follow = true;              // show the service the receiver hears until the user picks a view
    int lastActive = -1;
    int service = 0;                 // 0 auto, 1 NAVTEX, 2 DSC, 3 fax
    int lpmIdx = 0, iocIdx = 0;      // 0 = detect
    float slant = 0;
    bool autoSlant = true;
    int pushedService = -1, pushedLpm = -1, pushedIoc = -1;
    double pushedFreq = -1;
    float pushedSlant = 1e9f;
    int pushedAuto = -1;
    std::string navSel;              // header + time of the selected NAVTEX message
    std::string dscSelKey;
    std::string mapKey;              // the call the small map was last centred on
    adsbmap::View map;
    // the fax picture
    gfx::Image* img = nullptr;
    int texW = 0, texH = 0, imgW = 0, imgH = 0;
    uint64_t imgSeq = 0, telImgSeq = ~0ull;
    dect2::FaxImage last;            // the copy kept for saving
    float zoom = 0;                  // 0 = fit the width
    ImVec2 pan{0, 0};
    std::string saveMsg;
};
State S;

const int kLpm[] = {0, 60, 90, 120, 240};
const int kIoc[] = {0, 576, 288};

bool live(const App& a) { return a.engine.running() && a.rx.standard == 16; }   // the engine reports its standard code minus one

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.service = std::max(0, std::min(3, (int)d.getI("marineService", 0)));
    S.lpmIdx = std::max(0, std::min(4, (int)d.getI("marineLpm", 0)));
    S.iocIdx = std::max(0, std::min(2, (int)d.getI("marineIoc", 0)));
    S.autoSlant = d.getB("marineAutoSlant", true);
    S.slant = (float)d.getD("marineSlant", 0.0);
    S.map.zoom = 5;
}

std::string utcText(int64_t t) {
    if (t <= 0) return "-";
    time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", &g);
    return b;
}

std::string navKey(const NavtexMessage& m) { return m.header + "|" + std::to_string(m.rxTime) + "|" + std::to_string((long long)(m.rxSec * 10)); }
std::string dscKey(const DscCall& c) { return c.fromMmsi + "|" + std::to_string((long long)(c.rxSec * 10)); }

// B2 subject colours: warnings warm, forecasts and ice cool, search and rescue red
ImU32 subjectColour(char s) {
    switch (s) {
    case 'A': return IM_COL32(230, 160, 60, 255);    // navigational warning
    case 'B': return IM_COL32(235, 110, 80, 255);    // meteorological warning
    case 'C': return IM_COL32(120, 190, 235, 255);   // ice report
    case 'D': return IM_COL32(235, 70, 70, 255);     // search and rescue
    case 'E': return IM_COL32(100, 175, 235, 255);   // forecast
    case 'L': return IM_COL32(230, 160, 60, 255);    // more navigational warnings
    case 'Z': return IM_COL32(150, 154, 158, 255);   // QRU, nothing to send
    default: return IM_COL32(130, 200, 150, 255);
    }
}

void subjectBadge(char s) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), subjectColour(s), 3.f);
    const char t[2] = {s, 0};
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(p.x + (h - ts.x) * 0.5f, p.y), IM_COL32(10, 12, 14, 255), t);
    ImGui::Dummy(ImVec2(h, h));
}

// ---------------------------------------------------------------- NAVTEX

void navtexView(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float lw = std::min(380.f * gUi, W * 0.42f);
    std::vector<const NavtexMessage*> rows;
    for (const auto& m : t.navtex) rows.push_back(&m);
    std::stable_sort(rows.begin(), rows.end(), [](const NavtexMessage* x, const NavtexMessage* y) {
        if (x->station != y->station) return x->station < y->station;
        return x->rxSec > y->rxSec;      // newest first within a station
    });
    const NavtexMessage* sel = nullptr;
    for (const auto* m : rows) if (navKey(*m) == S.navSel) sel = m;
    if (!sel && !t.navtex.empty()) sel = &t.navtex.back();

    ImGui::BeginChild("##nav_l", ImVec2(lw, H), ImGuiChildFlags_Borders);
    if (rows.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", live(a) ? "no NAVTEX message yet (one takes a minute or more)" : "start the receiver"); ImGui::PopTextWrapPos(); }
    char cur = 0;
    bool open = true;
    for (const auto* m : rows) {
        if (m->station != cur) {
            cur = m->station;
            int n = 0;
            for (const auto* x : rows) n += x->station == cur;
            char h[48];
            snprintf(h, sizeof h, "Station %c  (%d)###st%c", cur, n, cur);
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            open = ImGui::CollapsingHeader(h);
        }
        if (!open) continue;
        ImGui::PushID(navKey(*m).c_str());
        subjectBadge(m->subject);
        ImGui::SameLine(0, 6 * gUi);
        char b[160];
        snprintf(b, sizeof b, "%s  %s%s", m->header.c_str(), m->subjectName.c_str(), m->complete ? "" : "  (cut short)");
        if (ImGui::Selectable(b, m == sel)) S.navSel = navKey(*m);
        ImGui::SameLine(lw - 120 * gUi);
        if (m->errors) ImGui::TextColored(m->cer < 0.02f ? pal::warnAmber() : pal::badRed(), "%.1f %%", 100.f * m->cer);
        else ImGui::TextColored(pal::okGreen(), "clean");
        ImGui::SameLine(lw - 64 * gUi);
        ImGui::TextDisabled("%s", utcText(m->rxTime).c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##nav_r", ImVec2(0, H), ImGuiChildFlags_Borders);
    if (!sel) {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("the text of the selected message shows here"); ImGui::PopTextWrapPos(); }
    } else {
        ImGui::TextColored(pal::heading(), "%s", sel->header.c_str());
        ImGui::SameLine(); ImGui::TextDisabled("%s", sel->subjectName.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("station %c, number %s, %u characters, %u unreadable (%.1f %%)%s, heard %d x, last at %s UTC",
                            sel->station, sel->number >= 0 ? std::to_string(sel->number).c_str() : "?", sel->chars, sel->errors,
                            100.f * sel->cer, sel->complete ? "" : ", no NNNN", sel->repeats, utcText(sel->rxTime).c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("copy text")) ImGui::SetClipboardText(("ZCZC " + sel->header + "\n" + sel->text + "\nNNNN").c_str());
        if (sel->textCut) { ImGui::SameLine(); ImGui::TextDisabled("older message: only the start is kept"); }
        ImGui::Separator();
        ImGui::BeginChild("##nav_txt");
        ImGui::PushFont(a.mono, 0);
        ImGui::TextWrapped("%s", sel->text.c_str());
        ImGui::PopFont();
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- DSC

void dscDetail(App& a, const DscCall* c, ImVec2 size) {
    if (!c) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", live(a) ? "click a call" : "start the receiver"); ImGui::PopTextWrapPos(); } return; }
    auto kv = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        ImGui::TextDisabled("%s", k); kvColumn(96 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    if (c->distress) ImGui::TextColored(pal::badRed(), "%s", c->formatName.c_str());
    else ImGui::TextColored(pal::heading(), "%s", c->formatName.c_str());
    kv("category", c->categoryName);
    kv("from", c->fromMmsi.empty() ? "unreadable" : c->fromMmsi);
    kv("to", c->to);
    kv("in distress", c->distressMmsi);
    kv("nature", c->natureName);
    kv("position", c->posText);
    if (c->hasTime) { char b[16]; snprintf(b, sizeof b, "%02d:%02d UTC", c->utcHour, c->utcMin); kv("time", b); }
    kv("telecommand", c->telecmdText);
    kv("frequency rx", c->freqRx);
    kv("frequency tx", c->freqTx);
    kv("end", c->eos == 117 ? "acknowledgement requested" : c->eos == 122 ? "acknowledgement" : c->eos == 127 ? "other call" : "");
    char b[64];
    snprintf(b, sizeof b, "%s, %d symbols lost%s", c->eccOk ? "check ok" : "check failed", c->erasures, c->vhf ? ", VHF ch 70" : ", MF/HF");
    kv("received", b);
    kv("heard", utcText(c->rxTime) + " UTC");
    if (!c->hasPos) return;
    ImGui::Spacing();
    const float mh = std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi);
    if (mh < 60 * gUi || size.x < 60 * gUi) return;
    if (S.mapKey != dscKey(*c)) { S.map.lat = c->lat; S.map.lon = c->lon; S.mapKey = dscKey(*c); }   // centre on a newly selected call, then the user may drag
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    adsbmap::draw(S.map, ImVec2(ImGui::GetContentRegionAvail().x, mh));
    const ImVec2 q = adsbmap::project(c->lat, c->lon);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + ImGui::GetContentRegionAvail().x + 1, p0.y + mh), true);
    const ImU32 col = c->distress ? IM_COL32(235, 70, 70, 255) : IM_COL32(110, 205, 120, 255);
    dl->AddCircleFilled(q, 6 * gUi, col);
    dl->AddCircle(q, 11 * gUi, col, 24, 2.f);
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 * gUi, p0.y + 6 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(24 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(24 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + mh));
}

void dscView(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float dw = std::min(320.f * gUi, W * 0.32f);
    const DscCall* sel = nullptr;
    for (const auto& c : t.dsc) if (dscKey(c) == S.dscSelKey) sel = &c;
    if (!sel && !t.dsc.empty()) sel = &t.dsc.back();
    ImGui::BeginChild("##dsc_l", ImVec2(W - dw - 8 * gUi, H));
    // fixed widths that fit the content and a horizontal scroll bar when the pane is narrow, so no column is cut
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##dsc_t", 7, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("UTC");
        ImGui::TableSetupColumn("Call");
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("To");
        ImGui::TableSetupColumn("Position");
        ImGui::TableSetupColumn("Check");
        ImGui::TableHeadersRow();
        for (size_t i = t.dsc.size(); i-- > 0;) {    // newest first
            const DscCall& c = t.dsc[i];
            ImGui::TableNextRow();
            if (c.distress) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(120, 28, 28, 150));
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            const std::string k = dscKey(c);
            if (ImGui::Selectable(utcText(c.rxTime).c_str(), sel == &c, ImGuiSelectableFlags_SpanAllColumns)) S.dscSelKey = k;
            ImGui::PopID();
            ImGui::TableNextColumn();
            std::string nm = c.formatName;
            if (!c.natureName.empty()) nm += ": " + c.natureName;
            if (c.distress) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.5f, 1), "%s", nm.c_str()); else ImGui::TextUnformatted(nm.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.categoryName.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.fromMmsi.empty() ? "?" : c.fromMmsi.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.to.c_str());
            ImGui::TableNextColumn(); if (c.hasPos) ImGui::TextUnformatted(c.posText.c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (c.eccOk) ImGui::TextColored(pal::okGreen(), "ok"); else ImGui::TextColored(pal::badRed(), "bad");
        }
        ImGui::EndTable();
    }
    if (t.dsc.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", live(a) ? "no DSC call yet" : "start the receiver"); ImGui::PopTextWrapPos(); }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##dsc_r", ImVec2(0, H), ImGuiChildFlags_Borders);
    dscDetail(a, sel, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
}

// ---------------------------------------------------------------- fax

void uploadFax(App& a) {
    if (!live(a) || !gGfx) return;
    if (a.rx.marine.fax.imageSeq == S.telImgSeq) return;     // ask for a copy only when the report says the picture changed
    dect2::FaxImage im;
    uint64_t seq = S.imgSeq;
    if (!a.engine.marine().latestImage(im, seq)) { S.telImgSeq = a.rx.marine.fax.imageSeq; return; }
    S.imgSeq = seq;
    S.telImgSeq = a.rx.marine.fax.imageSeq;
    if (im.width <= 0 || im.height <= 0 || im.pix.size() < (size_t)im.width * im.height) return;
    const int th = std::max(1500, im.height);
    if (!S.img || S.texW != im.width || S.texH < im.height) {
        delete S.img;
        S.img = gGfx->createImage(im.width, th, 0xFFFFFFFFu);
        S.texW = im.width; S.texH = th;
    }
    std::vector<uint32_t> px((size_t)im.width * im.height);
    for (size_t i = 0; i < px.size(); i++) { const uint32_t g = im.pix[i]; px[i] = 0xFF000000u | (g << 16) | (g << 8) | g; }
    S.img->update(0, 0, im.width, im.height, px.data());
    S.imgW = im.width; S.imgH = im.height;
    S.last = std::move(im);
}

void saveFax() {
    if (S.last.width <= 0 || S.last.height <= 0) return;
    const std::string path = saveFileDialog("weather-fax.png");
    if (path.empty()) return;
    std::vector<uint8_t> rgba((size_t)S.last.width * S.last.height * 4);
    for (size_t i = 0; i < S.last.pix.size() && i * 4 + 3 < rgba.size(); i++) {
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = S.last.pix[i];
        rgba[i * 4 + 3] = 255;
    }
    S.saveMsg = gfx::writePng(path.c_str(), rgba.data(), S.last.width, S.last.height) ? "saved " + path : "could not write " + path;
}

void faxView(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const dect2::MarineFaxInfo& f = t.fax;
    static const char* const lpmN[] = {"lpm: detect", "60 lpm", "90 lpm", "120 lpm", "240 lpm"};
    static const char* const iocN[] = {"IOC: start tone", "IOC 576", "IOC 288"};
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::Combo("##mflpm", &S.lpmIdx, lpmN, 5)) plat::prefs().setI("marineLpm", S.lpmIdx);
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::Combo("##mfioc", &S.iocIdx, iocN, 3)) plat::prefs().setI("marineIoc", S.iocIdx);
    ImGui::SameLine();
    if (ImGui::Checkbox("auto slant", &S.autoSlant)) plat::prefs().setB("marineAutoSlant", S.autoSlant);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Measure the sample clock error from the phasing lines and straighten the picture.\nThe slider is added on top.");
    ImGui::SameLine(); ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::SliderFloat("##mfsl", &S.slant, -300.f, 300.f, "slant %+.1f ppm")) plat::prefs().setD("marineSlant", S.slant);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Straighten a slanted picture by hand. Applies to the lines already received too. Double-click to type.");
    ImGui::SameLine();
    if (ImGui::SmallButton("0")) { S.slant = 0; plat::prefs().setD("marineSlant", 0.0); }
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::SmallButton("fit")) { S.zoom = 0; S.pan = ImVec2(0, 0); }
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1")) { S.zoom = 1; }
    ImGui::SameLine(0, 14 * gUi);
    ImGui::BeginDisabled(S.last.width <= 0);
    if (ImGui::SmallButton("save PNG")) saveFax();
    ImGui::EndDisabled();
    static const char* const st[] = {"waiting for the start tone", "phasing", "receiving", "stopped"};
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s, IOC %d, %d lpm, %d lines, %d px wide, slant %+.1f ppm, black at %.0f Hz (%+.0f Hz off), last tone %.0f Hz",
                        st[std::max(0, std::min(3, f.state))], f.ioc, f.lpm, f.lines, f.width, f.slantPpm, f.blackHz, f.blackHz - 1500.0, f.toneHz); ImGui::PopTextWrapPos(); }
    if (!S.saveMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("  %s", S.saveMsg.c_str()); }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 box = ImGui::GetContentRegionAvail();
    if (box.x < 20 || box.y < 20) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + box.x, p0.y + box.y), IM_COL32(18, 19, 21, 255));
    ImGui::InvisibleButton("##faxpic", box);
    const bool hov = ImGui::IsItemHovered();
    if (!S.img || S.imgW <= 0) {
        const char* msg = !live(a) ? "start the receiver" : f.state == 0 ? "waiting for a fax start tone (300 Hz for IOC 576, 675 Hz for IOC 288)" : "phasing: the picture starts after the phasing lines";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (box.x - ts.x) * 0.5f, p0.y + box.y * 0.5f), IM_COL32(150, 154, 158, 255), msg);
        return;
    }
    const float fit = box.x / (float)S.imgW;
    float z = S.zoom > 0 ? S.zoom : fit;
    if (hov && ImGui::GetIO().MouseWheel != 0) {     // zoom around the pointer
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float nz = std::max(fit * 0.25f, std::min(8.f, z * std::pow(1.15f, ImGui::GetIO().MouseWheel)));
        S.pan.x = (m.x - p0.x) - ((m.x - p0.x) - S.pan.x) * nz / z;
        S.pan.y = (m.y - p0.y) - ((m.y - p0.y) - S.pan.y) * nz / z;
        S.zoom = nz; z = nz;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        S.pan.x += d.x; S.pan.y += d.y;
    }
    const float w = S.imgW * z, h = S.imgH * z;
    if (S.zoom <= 0) {
        S.pan.x = 0;
        if (h > box.y) S.pan.y = box.y - h;       // fitted: keep the newest lines in view
        else S.pan.y = 0;
    }
    S.pan.x = std::min(0.f, std::max(S.pan.x, std::min(0.f, box.x - w)));
    S.pan.y = std::min(0.f, std::max(S.pan.y, std::min(0.f, box.y - h)));
    const ImVec2 q0(p0.x + S.pan.x, p0.y + S.pan.y), q1(q0.x + w, q0.y + h);
    dl->PushClipRect(p0, ImVec2(p0.x + box.x, p0.y + box.y), true);
    dl->AddImage(S.img->texture(), q0, q1, ImVec2(0, 0), ImVec2(1, (float)S.imgH / (float)S.texH));
    dl->PopClipRect();
    char b[48];
    snprintf(b, sizeof b, "%dx%d  %.0f %%", S.imgW, S.imgH, z * 100);
    dl->AddText(ImVec2(p0.x + 6 * gUi, p0.y + box.y - ImGui::GetTextLineHeight() - 4 * gUi), IM_COL32(200, 60, 60, 255), b);
}

// ---------------------------------------------------------------- tuning aid

void tuningView(App& a, ImVec2 size) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 9, 10, 255));
    const float lo = t.specLoHz, hi = t.specHiHz;
    const float l = p0.x + 40 * gUi, r = p1.x - 8 * gUi, top = p0.y + 6 * gUi, bot = p1.y - 20 * gUi;
    if (r - l < 40 || bot - top < 30) { ImGui::Dummy(size); return; }
    auto X = [&](double hz) { return (float)(l + (hz - lo) / (hi - lo) * (r - l)); };
    float dmax = -200, dmin = 200;
    for (float v : t.spectrumDb) { dmax = std::max(dmax, v); dmin = std::min(dmin, v); }
    if (t.spectrumDb.empty()) { dmax = 0; dmin = -60; }
    const float yTop = std::ceil((dmax + 5) / 10) * 10, yBot = std::max(yTop - 80, std::floor(dmin / 10) * 10);
    auto Y = [&](float db) { return bot - (db - yBot) / std::max(1.f, yTop - yBot) * (bot - top); };
    const ImU32 grid = IM_COL32(48, 52, 54, 255), txt = IM_COL32(120, 124, 128, 255);
    char b[32];
    for (int hz = -500; hz <= 3500; hz += 500) {
        dl->AddLine(ImVec2(X(hz), top), ImVec2(X(hz), bot), grid);
        snprintf(b, sizeof b, "%d", hz);
        dl->AddText(ImVec2(X(hz) - 12 * gUi, bot + 3 * gUi), txt, b);
    }
    for (float db = yBot; db <= yTop; db += 10) {
        dl->AddLine(ImVec2(l, Y(db)), ImVec2(r, Y(db)), grid);
        snprintf(b, sizeof b, "%.0f", db);
        dl->AddText(ImVec2(p0.x + 4 * gUi, Y(db) - 7 * gUi), txt, b);
    }
    // the tones each service expects, relative to the dial frequency (USB for fax)
    struct Mark { double hz; const char* label; ImU32 col; int row; };   // row: label line, so that close markers do not overlap
    const ImU32 cf = IM_COL32(110, 170, 235, 170), cn = IM_COL32(230, 180, 70, 170), ct = IM_COL32(160, 120, 220, 170);
    const Mark marks[] = {{-85, "FSK", cn, 0}, {85, "", cn, 0}, {1500, "black", cf, 0}, {2300, "white", cf, 0},
                          {300, "start 576", ct, 1}, {675, "start 288", ct, 0}, {450, "stop", ct, 2}};
    for (const Mark& m : marks) {
        dl->AddLine(ImVec2(X(m.hz), top), ImVec2(X(m.hz), bot), m.col, 1.5f);
        if (m.label[0]) dl->AddText(ImVec2(X(m.hz) + 3 * gUi, top + 2 * gUi + m.row * ImGui::GetTextLineHeight()), m.col, m.label);
    }
    if (t.toneHighHz != 0 || t.toneLowHz != 0) {     // the FSK tones the receiver found
        for (double hz : {t.toneLowHz, t.toneHighHz}) dl->AddLine(ImVec2(X(hz), top), ImVec2(X(hz), bot), IM_COL32(120, 230, 140, 230), 2.f);
    }
    const size_t n = t.spectrumDb.size();
    if (n > 1) {
        std::vector<ImVec2> pts(n);
        for (size_t i = 0; i < n; i++) pts[i] = ImVec2(X(lo + (hi - lo) * (double)i / (double)(n - 1)), std::max(top, std::min(bot, Y(t.spectrumDb[i]))));
        dl->AddPolyline(pts.data(), (int)n, IM_COL32(140, 230, 160, 255), 0, 1.3f);
    } else {
        dl->AddText(ImVec2(l + 10, top + 10), txt, live(a) ? "no spectrum yet" : "start the receiver");
    }
    ImGui::Dummy(size);
}

void tuningPanel(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const ImVec2 av = ImGui::GetContentRegionAvail();
    tuningView(a, ImVec2(av.x, std::max(120.f * gUi, av.y - 3 * ImGui::GetTextLineHeightWithSpacing())));
    if (t.toneHighHz != 0 || t.toneLowHz != 0)
        { ImGui::PushTextWrapPos(0); ImGui::Text("FSK tones %+.0f / %+.0f Hz, shift %.0f Hz (170 expected), centre %+.0f Hz, %.1f baud, %.0f dB over the noise",
                    t.toneLowHz, t.toneHighHz, t.shiftHz, t.cfoHz, t.baudEst, t.fskLevelDb); ImGui::PopTextWrapPos(); }
    else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no FSK signal found: NAVTEX and HF DSC sit on the dial frequency, 85 Hz either side"); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("VHF DSC (channel 70) is FM with audio tones of 1300 and 2100 Hz: it does not show in this spectrum."); ImGui::PopTextWrapPos(); }
}

// the analysis row: the tuning aid and the newest message or the fax picture
void panels(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    const float sw = std::max(200.f, W * 0.6f - gap);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(sw, "Tuning aid: Hz from the dial frequency, with the tones each service expects");
    tuningView(a, ImVec2(sw, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    const float rw = std::max(120.f, ImGui::GetContentRegionAvail().x - gap * 0.5f);
    if (live(a) && t.serviceActive == 3 && S.img && S.imgW > 0) {
        captionFit(rw, "Fax: %d lines", t.fax.lines);
        const float z = std::min(rw / S.imgW, plotH / std::max(1, S.imgH));
        const float w = S.imgW * z, h = S.imgH * z;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddImage(S.img->texture(), p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, (float)S.imgH / (float)S.texH));
        ImGui::Dummy(ImVec2(rw, plotH));
    } else {
        captionFit(rw, "Last decoded");
        ImGui::BeginChild("##mar_last", ImVec2(rw, plotH), ImGuiChildFlags_Borders);
        const NavtexMessage* m = t.navtex.empty() ? nullptr : &t.navtex.back();
        const DscCall* c = t.dsc.empty() ? nullptr : &t.dsc.back();
        if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("start the receiver"); ImGui::PopTextWrapPos(); }
        else if (c && (!m || c->rxSec >= m->rxSec)) {
            if (c->distress) ImGui::TextColored(pal::badRed(), "%s", c->text.c_str()); else ImGui::TextWrapped("%s", c->text.c_str());
        } else if (m) {
            ImGui::TextColored(pal::heading(), "%s  %s", m->header.c_str(), m->subjectName.c_str());
            ImGui::PushFont(a.mono, 0); ImGui::TextWrapped("%s", m->text.c_str()); ImGui::PopFont();
        } else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("nothing decoded yet"); ImGui::PopTextWrapPos(); }
        ImGui::EndChild();
    }
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- the hooks

void tick(App& a) {
    loadState();
    if (!a.engine.running()) { S.pushedService = S.pushedLpm = S.pushedIoc = S.pushedAuto = -1; S.pushedFreq = -1; S.pushedSlant = 1e9f; return; }
    dect2::MarineReceiver& r = a.engine.marine();
    if (S.pushedService != S.service) { r.setService(S.service); S.pushedService = S.service; }
    // a test signal or a recording has no dial frequency: auto then runs every decoder
    const DeviceInfo::Kind k = a.devices.empty() ? DeviceInfo::Synthetic : a.devices[(size_t)std::max(0, std::min((int)a.devices.size() - 1, a.devIdx))].kind;
    const double fz = k == DeviceInfo::Synthetic || k == DeviceInfo::File ? 0.0 : a.freqMhz * 1e6;
    if (S.pushedFreq != fz) { r.setFrequencyHz(fz); S.pushedFreq = fz; }
    if (S.pushedLpm != S.lpmIdx) { r.setFaxLpm(kLpm[S.lpmIdx]); S.pushedLpm = S.lpmIdx; }
    if (S.pushedIoc != S.iocIdx) { r.setFaxIoc(kIoc[S.iocIdx]); S.pushedIoc = S.iocIdx; }
    if (S.pushedSlant != S.slant) { r.setFaxSlantPpm(S.slant); S.pushedSlant = S.slant; }
    if (S.pushedAuto != (int)S.autoSlant) { r.setFaxAutoSlant(S.autoSlant); S.pushedAuto = S.autoSlant; }
    if (live(a)) uploadFax(a);
}

void tab(App& a) {
    loadState();
    const int act = live(a) ? a.rx.marine.serviceActive : 0;
    if (S.follow && act != S.lastActive && act != 0) S.view = act == 1 ? 0 : act == 3 ? 2 : 1;
    S.lastActive = act;
    const int before = S.view;
    subNav("marv", S.view, {"NAVTEX", "DSC", "Fax", "Tuning"});
    if (S.view != before) S.follow = false;
    ImGui::SameLine(0, 20 * gUi);
    ImGui::TextDisabled("Service"); ImGui::SameLine();
    static const char* const sv[] = {"auto", "NAVTEX", "DSC", "weather fax"};
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::Combo("##marsv", &S.service, sv, 4)) plat::prefs().setI("marineService", S.service);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("auto: by the frequency (490, 518, 4209.5 kHz NAVTEX; the DSC frequencies; otherwise fax).\nWith the test signal or a recording every decoder runs.");
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) a.engine.marine().clearMessages();
    switch (S.view) {
    case 0: navtexView(a); break;
    case 1: dscView(a); break;
    case 2: faxView(a); break;
    default: tuningPanel(a); break;
    }
}

void list(App& a) {
    loadState();
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see messages"); ImGui::PopTextWrapPos(); } return; }
    const dect2::MarineTelemetry& t = a.rx.marine;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%llu NAVTEX, %llu DSC", (unsigned long long)t.navtexCount, (unsigned long long)t.dscCount); ImGui::PopTextWrapPos(); }
    ImGui::BeginChild("##mar_list");
    struct Row { double sec; int kind; size_t i; };
    std::vector<Row> rows;
    for (size_t i = 0; i < t.navtex.size(); i++) rows.push_back({t.navtex[i].rxSec, 0, i});
    for (size_t i = 0; i < t.dsc.size(); i++) rows.push_back({t.dsc[i].rxSec, 1, i});
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.sec > y.sec; });
    int n = 0;
    for (const Row& r : rows) {
        if (n++ >= 60) break;
        ImGui::PushID(n);
        if (r.kind == 0) {
            const NavtexMessage& m = t.navtex[r.i];
            subjectBadge(m.subject); ImGui::SameLine(0, 6 * gUi);
            if (ImGui::Selectable((m.header + "  " + m.subjectName).c_str(), false)) { S.navSel = navKey(m); S.view = 0; S.follow = false; }
        } else {
            const DscCall& c = t.dsc[r.i];
            const std::string s = "DSC  " + c.formatName + "  " + (c.fromMmsi.empty() ? "?" : c.fromMmsi);
            if (c.distress) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.45f, 1));
            if (ImGui::Selectable(s.c_str(), false)) { S.dscSelKey = dscKey(c); S.view = 1; S.follow = false; }
            if (c.distress) ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    if (rows.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(t.serviceActive == 3 ? "receiving a fax: see the Fax view" : "nothing decoded yet"); ImGui::PopTextWrapPos(); }
    ImGui::EndChild();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::MarineTelemetry& t = a.rx.marine;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    static const char* const sv[] = {"nothing yet", "NAVTEX", "DSC MF/HF", "weather fax", "DSC VHF"};
    static const char* const ss[] = {"auto", "NAVTEX", "DSC", "weather fax"};
    kv("state", "%s", t.state == 0 ? "searching" : t.state == 1 ? "signal" : "decoding");
    kv("service", "%s (selector: %s)", sv[std::max(0, std::min(4, t.serviceActive))], ss[std::max(0, std::min(3, t.serviceSetting))]);
    kv("locked", "NAVTEX %s, DSC %s", t.navtexLocked ? "yes" : "no", t.dscLocked ? "yes" : "no");
    kv("messages", "%llu NAVTEX, %llu DSC calls", (unsigned long long)t.navtexCount, (unsigned long long)t.dscCount);
    kv("good / bad", "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (t.toneHighHz != 0 || t.toneLowHz != 0) {
        kv("FSK tones", "%+.0f / %+.0f Hz, shift %.0f Hz", t.toneLowHz, t.toneHighHz, t.shiftHz);
        kv("symbol rate", "%.2f baud", t.baudEst);
    }
    kv("signal", "%.1f dB SNR in 3 kHz, carrier error %+.0f Hz", t.snrDb, t.cfoHz);
    kv("fax", "state %d, %d lines, IOC %d, %d lpm, slant %+.1f ppm", t.fax.state, t.fax.lines, t.fax.ioc, t.fax.lpm, t.fax.slantPpm);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::MarineTelemetry& t = a.rx.marine;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signal", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(12 * gUi);
    lamp("Decoding", !on ? 0 : t.state == 2 ? 1 : t.dataValid ? 2 : 0); flowNext(10 * gUi);
    bool distress = false;
    for (const auto& c : t.dsc) distress |= c.distress;
    lamp("Distress", !on ? 0 : distress ? 3 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(on ? dect2::marineSummary(t).c_str() : run ? "starting" : "stopped");
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Marine";
    if (live(a)) l2 = dect2::marineSummary(a.rx.marine);
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("send"); ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    static const char* const sv[] = {"NAVTEX", "DSC MF/HF", "weather fax", "DSC VHF ch 70"};
    int s = std::max(1, std::min(4, sc.modeOpt[0] ? sc.modeOpt[0] : 1)) - 1;
    if (ImGui::Combo("##msv", &s, sv, 4)) { sc.modeOpt[0] = s + 1; changed = true; }
    flowNext();
    bool fade = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("HF fading", &fade)) { sc.modeOpt[1] = fade ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Two paths 1 ms apart, each fading with a 0.5 Hz Doppler spread.");
    flowNext(); ImGui::TextDisabled("SNR"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float snr = (float)sc.snrDb;
    if (ImGui::SliderFloat("##msnr", &snr, -10.f, 40.f, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    flowNext(); ImGui::TextDisabled("mistuned"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float mt = (float)sc.modeVal[0];
    if (ImGui::SliderFloat("##mmt", &mt, -100.f, 100.f, "%+.0f Hz")) { sc.modeVal[0] = mt; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    out.push_back({"SNR  dB", "%.0f", t.snrDb, -10, 40, t.snrDb >= 10 ? 1 : t.snrDb >= 0 ? 2 : 3});
    out.push_back({"NAVTEX", "%.0f", (double)t.navtexCount, 0, 20, t.navtexCount ? 1 : 0});
    out.push_back({"DSC", "%.0f", (double)t.dscCount, 0, 20, t.dscCount ? 1 : 0});
    out.push_back({"FAX LINES", "%.0f", (double)t.fax.lines, 0, 1500, t.fax.lines ? 1 : 0});
}

} // namespace

extern const ModeUi kMarineUi;
const ModeUi kMarineUi = {
    .sideTitle = "MESSAGES",
    .tabName = "Messages",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
