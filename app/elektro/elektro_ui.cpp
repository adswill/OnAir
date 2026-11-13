// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Elektro-L N3 / N4 / N5 HRIT / LRIT screens: the same xRIT product list as GOES, with satellite and link presets.
#include "ui/kit/mode_panel.h"
#include "app.h"
#include "panels/gallery.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/elektro/elektro_rx.h"
#include "dect2/elektro/elektro_tel.h"
#include "dect2/goes/goes_proto.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using dect2::ElektroTelemetry;
using dect2::GoesTelemetry;
using dect2::goes::Product;
namespace goes = dect2::goes;

const char* const kFilters[] = {"All products", "Images", "Text"};
const char* const kCraft[] = {"Elektro-L N3 (76°E)", "Elektro-L N4 (165.8°E)", "Elektro-L N5 (14.5°W)"};
const char* const kLink[] = {"HRIT 665.4 ksym/s", "HRIT 1.0 Msym/s", "LRIT 128 ksym/s", "LRIT 64 ksym/s", "GGAK 5 ksym/s (1693 MHz)", "LRIT 294 ksym/s", "HRIT 1.15 Msym/s OQPSK"};

struct State {
    bool loaded = false;
    bool autoSave = true;
    int filter = 0;
    bool follow = true;
    uint64_t selId = 0;
    int craft = 0, link = 0, appliedCraft = -1, appliedLink = -1;
    gfx::Image* tex = nullptr;
    uint64_t texId = 0, texVersion = 0;
    int texW = 0, texH = 0, texStep = 1;
    double texTime = 0;
    bool texFailed = false;
    std::set<uint64_t> saved;
    std::string msg;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.autoSave = plat::prefs().getB("elektroAutoSave", true);
    S.filter = std::clamp((int)plat::prefs().getI("elektroFilter", 0), 0, 2);
    S.craft = std::clamp((int)plat::prefs().getI("elektroCraft", 0), 0, 2);
    S.link = std::clamp((int)plat::prefs().getI("elektroLink", 0), 0, 6);
}

bool live(const App& a) { return a.rx.standard == 81; }  // family 80 / stdMode 82

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

bool isImage(const Product& p) { return p.w > 0 || (p.hdr.hasImage && (p.hdr.noaaCompression == 2 || p.hdr.noaaCompression == 5)); }
bool isText(const Product& p) { return !isImage(p) && (!p.text.empty() || p.kind == goes::KEmwin || p.kind == goes::KAdmin || p.kind == goes::KText); }
bool passes(const Product& p) { return S.filter == 0 || (S.filter == 1 ? isImage(p) : isText(p)); }

std::shared_ptr<Product> selected(const GoesTelemetry& t) {
    if (S.follow) {
        for (const auto& p : t.products) if (isImage(*p) && passes(*p)) return p;
        for (const auto& p : t.products) if (passes(*p)) return p;
        return nullptr;
    }
    for (const auto& p : t.products) if (p->id == S.selId) return p;
    return nullptr;
}

bool writeFile(const std::string& path, const uint8_t* d, size_t n) {
    std::ofstream o(std::filesystem::path(reinterpret_cast<const uint8_t*>(path.c_str()) ? reinterpret_cast<const char8_t*>(path.c_str()) : u8""), std::ios::binary);
    if (!o) return false;
    o.write((const char*)d, (std::streamsize)n);
    return (bool)o;
}

std::string elektroDir() {
    const std::string dir = plat::dataDir() + "/elektro";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    return dir;
}

std::string fileName(const Product& p, const char* ext) {
    std::string n = p.name;
    for (char& c : n) if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    const size_t dot = n.rfind('.');
    if (dot != std::string::npos && dot + 6 >= n.size()) n.resize(dot);
    if (n.empty()) n = "elektro-" + std::to_string(p.id);
    return n + ext;
}

bool saveProduct(const Product& p, std::string& where) {
    const std::string dir = elektroDir();
    std::lock_guard<std::mutex> lk(p.mu);
    if (p.w > 0 && p.h > 0) {
        std::vector<uint32_t> px(p.pix.size());
        for (size_t i = 0; i < px.size(); i++) { const uint32_t v = p.pix[i]; px[i] = 0xFF000000u | v << 16 | v << 8 | v; }
        where = dir + "/" + fileName(p, ".png");
        return gfx::writePng(where.c_str(), reinterpret_cast<const uint8_t*>(px.data()), p.w, p.h);
    }
    if (p.data.empty()) return false;
    const char* ext = ".bin";
    const int c = p.hdr.hasNoaa ? p.hdr.noaaCompression : 0;
    if (c == 2) ext = ".jpg"; else if (c == 5) ext = ".gif"; else if (c == 10 || (p.data.size() > 2 && p.data[0] == 'P' && p.data[1] == 'K')) ext = ".zip";
    else if (!p.text.empty()) ext = ".txt";
    where = dir + "/" + fileName(p, ext);
    bool ok = writeFile(where, p.data.data(), p.data.size());
    if (ok && !p.innerName.empty() && !p.text.empty()) {
        std::string inner = p.innerName;
        for (char& ch : inner) if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
        ok = writeFile(dir + "/" + inner, reinterpret_cast<const uint8_t*>(p.text.data()), p.text.size());
    }
    return ok;
}

void autoSave(const GoesTelemetry& t) {
    if (!S.autoSave) return;
    for (const auto& p : t.products) {
        if (S.saved.count(p->id)) continue;
        bool done;
        { std::lock_guard<std::mutex> lk(p->mu); done = p->complete; }
        if (!done) continue;
        S.saved.insert(p->id);
        std::string where;
        if (saveProduct(*p, where)) S.msg = "saved " + where;
    }
}

void upload(const Product& p) {
    const double now = nowSec();
    const bool same = p.id == S.texId;
    uint64_t ver;
    { std::lock_guard<std::mutex> lk(p.mu); ver = p.version; }
    if (same && (ver == S.texVersion || (now - S.texTime < 0.5 && S.tex))) return;
    if (same && S.texFailed) return;
    if (!gGfx) return;
    S.texTime = now; S.texVersion = ver; S.texFailed = false;
    if (!same) { delete S.tex; S.tex = nullptr; S.texId = p.id; S.texW = S.texH = 0; }
    std::vector<uint32_t> px;
    int w = 0, h = 0, step = 1;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        if (p.w > 0 && p.h > 0) {
            step = std::max(1, (std::max(p.w, p.h) + 4095) / 4096);
            w = p.w / step; h = p.h / step;
            px.resize((size_t)w * h);
            for (int y = 0; y < h; y++) {
                const uint8_t* r = p.pix.data() + (size_t)y * step * p.w;
                for (int x = 0; x < w; x++) { const uint32_t v = r[(size_t)x * step]; px[(size_t)y * w + x] = 0xFF000000u | v << 16 | v << 8 | v; }
            }
        } else if (p.complete && !p.data.empty()) {
            char name[64];
            snprintf(name, sizeof name, "/elektro_%llu.img", (unsigned long long)p.id);
            const std::string path = plat::cacheDir() + name;
            if (!writeFile(path, p.data.data(), p.data.size()) || !plat::decodeImage(path, w, h, px) || w <= 0 || h <= 0 || w > 8192 || h > 8192) { S.texFailed = true; return; }
        } else return;
    }
    if (!S.tex || S.texW != w || S.texH != h) { delete S.tex; S.tex = gGfx->createImage(w, h, 0xFF000000u); S.texW = w; S.texH = h; }
    S.texStep = step;
    if (S.tex) S.tex->update(0, 0, w, h, px.data());
}

void showProduct(App& a, const Product& p) {
    int rows, h, segs, segsT;
    bool complete, errors;
    uint64_t bytes;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        rows = p.rowsDone; h = p.h; segs = p.segsDone; segsT = p.segsTotal; complete = p.complete; errors = p.errors; bytes = p.bytes;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.name.c_str()); ImGui::PopFont();
    char info[256];
    if (p.w > 0) snprintf(info, sizeof info, "%s, %d x %d, %d bits, %s, %d of %d lines, segment %d of %d%s%s", goes::kindName(p.kind), p.w, h, p.hdr.bpp,
                          goes::compressionName(p.hdr), rows, h, segs, segsT, complete ? "" : ", receiving", errors ? ", with errors" : "");
    else snprintf(info, sizeof info, "%s, %s, %llu bytes, VC %d%s%s", goes::kindName(p.kind), goes::compressionName(p.hdr), (unsigned long long)bytes, p.vcid,
                  complete ? "" : ", receiving", errors ? ", with errors" : "");
    ImGui::TextDisabled("%s", info);
    ImGui::PopTextWrapPos();
    if (isImage(p)) {
        upload(p);
        if (!S.tex || S.texId != p.id) { ImGui::TextDisabled("%s", S.texFailed ? "this picture could not be decoded here (it is saved as it came)" : "waiting for the picture"); return; }
        ImGui::BeginChild("##elpic", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        const ImVec2 av = ImGui::GetContentRegionAvail();
        const float z = std::max(0.05f, std::min(av.x / S.texW, std::max(av.y, 100.f) / S.texH));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddImage(S.tex->texture(), p0, ImVec2(p0.x + S.texW * z, p0.y + S.texH * z));
        ImGui::Dummy(ImVec2(S.texW * z, S.texH * z));
        ImGui::EndChild();
        return;
    }
    std::string txt;
    { std::lock_guard<std::mutex> lk(p.mu); txt = p.text; if (!p.innerName.empty()) txt = p.innerName + ":\n" + txt; }
    if (txt.empty()) { ImGui::TextDisabled("%s", complete ? "no text in this product (it can still be saved)" : "receiving"); return; }
    for (char& c : txt) if (c == '\r') c = ' '; else if ((unsigned char)c < 32 && c != '\n' && c != '\t') c = '.';
    ImGui::BeginChild("##eltxt", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(txt.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    ImGui::EndChild();
}

void ggakTab(App& a) {
    const dect2::GgakTelemetry& t = a.rx.elektro.ggak;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", "GGAK is space-weather telemetry (magnetometer and particle detectors), not pictures. 5 ksym/s PCM/PM on 1693 MHz. Receive only.");
    ImGui::PopTextWrapPos();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    ImGui::TextUnformatted(dect2::elektroSummary(a.rx.elektro).c_str());
    if (t.magOk) ImGui::Text("B  %.0f  %.0f  %.0f nT   |B| %.0f nT  (first int16 triple after the header; unverified mapping)", t.bx, t.by, t.bz, t.bmag);
    if (!ImGui::BeginTable("##ggakf", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupColumn("Marker");
    ImGui::TableSetupColumn("Sub");
    ImGui::TableSetupColumn("Timestamp");
    ImGui::TableSetupColumn("CRC");
    ImGui::TableSetupColumn("B (nT)");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    for (const auto& f : t.frames) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", f.marker);
        ImGui::TableNextColumn(); ImGui::Text("%d", f.sub);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)f.timestamp);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(f.crcOk ? "ok" : "bad");
        ImGui::TableNextColumn();
        if (f.magOk) ImGui::Text("%.0f %.0f %.0f", f.bx, f.by, f.bz);
        else ImGui::TextDisabled("—");
    }
    ImGui::EndTable();
}

void tab(App& a) {
    loadState();
    if (dect2::elektroIsGgak(S.link)) { ggakTab(a); return; }
    const GoesTelemetry& t = a.rx.elektro.hrit;
    const bool on = live(a);
    if (on) autoSave(t);
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##elf", &S.filter, kFilters, 3)) { plat::prefs().setI("elektroFilter", S.filter); plat::prefs().flush(); }
    flowNext(8 * gUi);
    ImGui::Checkbox("Newest", &S.follow);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the newest image as it arrives (choosing a product in the list turns this off)");
    flowNext(8 * gUi);
    auto sel = on ? selected(t) : nullptr;
    if (ImGui::Button("Save") && sel) { std::string where; S.msg = saveProduct(*sel, where) ? "saved " + where : "nothing to save yet"; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The product shown, into <data folder>/elektro/: images as PNG, pictures and ZIP files as they came, text as .txt");
    flowNext(5 * gUi);
    if (ImGui::Checkbox("Save automatically", &S.autoSave)) { plat::prefs().setB("elektroAutoSave", S.autoSave); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every product once it is complete, into <data folder>/elektro/");
    flowNext(5 * gUi);
    if (ImGui::Button("Clear")) { a.engine.elektro().clearProducts(); S.msg.clear(); S.selId = 0; S.follow = true; }
    flowEnd();
    if (!S.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.msg.c_str()); ImGui::PopTextWrapPos(); }
    if (!sel) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !a.engine.running() ? "Start the receiver. Elektro-L HRIT needs a dish (about 1 m, or a 1.7 GHz grid antenna) pointed at N3, N4 or N5, with an LNA and filter at the feed. Receive only."
                                                      : on ? dect2::elektroSummary(a.rx.elektro).c_str() : "starting");
        ImGui::PopTextWrapPos();
        return;
    }
    showProduct(a, *sel);
}

void list(App& a) {
    loadState();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (dect2::elektroIsGgak(S.link)) {
        const dect2::GgakTelemetry& t = a.rx.elektro.ggak;
        if (t.frames.empty()) { ImGui::TextDisabled("no GGAK frames yet"); return; }
        for (const auto& f : t.frames) {
            ImGui::Text("m%d/%d %s", f.marker, f.sub, f.crcOk ? "crc" : "bad");
        }
        return;
    }
    const GoesTelemetry& t = a.rx.elektro.hrit;
    if (t.products.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no products yet"); ImGui::PopTextWrapPos(); return; }
    if (!ImGui::BeginTable("##ell", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupColumn("Product", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Done");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    const auto cur = selected(t);
    for (const auto& p : t.products) {
        if (!passes(*p)) continue;
        int rows, h; bool complete, errors; uint64_t bytes;
        { std::lock_guard<std::mutex> lk(p->mu); rows = p->rowsDone; h = p->h; complete = p->complete; errors = p->errors; bytes = p->bytes; }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)p->id);
        if (ImGui::Selectable(p->name.c_str(), cur && cur->id == p->id, ImGuiSelectableFlags_SpanAllColumns)) { S.selId = p->id; S.follow = false; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nvirtual channel %d, APID %d, %s", p->name.c_str(), p->vcid, p->apid, goes::compressionName(p->hdr));
        ImGui::PopID();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", goes::kindName(p->kind));
        ImGui::TableNextColumn();
        if (p->w > 0 && h > 0) ImGui::TextDisabled("%d%%%s", rows * 100 / h, errors ? " !" : "");
        else ImGui::TextDisabled("%s", complete ? (errors ? "errors" : (bytes >= 10240 ? (std::to_string(bytes / 1024) + " kB").c_str() : (std::to_string(bytes) + " B").c_str())) : "...");
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const ElektroTelemetry& e = a.rx.elektro;
    if (dect2::elektroIsGgak(e.link)) {
        const dect2::GgakTelemetry& t = e.ggak;
        kvRow(a, "state", "%s", dect2::elektroSummary(e).c_str());
        kvRow(a, "satellite", "%s, %s", dect2::elektroCraftName(e.craft), dect2::elektroCraftPos(e.craft));
        kvRow(a, "link", "GGAK PCM/PM 5 ksym/s, 1693 MHz (receive only)");
        kvRow(a, "carrier", "offset %+.2f kHz, %s, lock %.2f, Es/N0 %.1f dB", t.cfoHz / 1e3, t.carrierLock ? "locked" : "searching", t.lockMetric, t.snrDb);
        kvRow(a, "frames", "%llu good CRC, %llu bad, last marker %d / sub %d", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.lastMarker, t.lastSub);
        kvRow(a, "timestamp", "%llu", (unsigned long long)t.lastTimestamp);
        if (t.magOk) kvRow(a, "B (unverified)", "%.0f  %.0f  %.0f nT, |B| %.0f nT", t.bx, t.by, t.bz, t.bmag);
        kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
        if (!t.log.empty()) {
            ImGui::Spacing(); ImGui::TextDisabled("Log");
            ImGui::PushFont(a.mono, 0);
            for (size_t i = 0; i < t.log.size() && i < 20; i++) { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.log[i].c_str()); ImGui::PopTextWrapPos(); }
            ImGui::PopFont();
        }
        return;
    }
    const GoesTelemetry& t = e.hrit;
    kvRow(a, "state", "%s", dect2::elektroSummary(e).c_str());
    kvRow(a, "satellite", "%s, %s", dect2::elektroCraftName(e.craft), dect2::elektroCraftPos(e.craft));
    kvRow(a, "link", "%s", dect2::elektroLinkName(e.link));
    kvRow(a, "carrier", "offset %+.2f kHz, %s, lock %.2f, Es/N0 %.1f dB", t.cfoHz / 1e3, t.carrierLock ? "locked" : "searching", t.lockMetric, t.snrDb);
    const double want = dect2::elektroSymbolRate(e.link);
    kvRow(a, "symbol clock", "%.1f ksym/s (%+.0f ppm)", t.symbolRate / 1e3, (t.symbolRate / want - 1) * 1e6);
    kvRow(a, "code", "K = 7, r = 1/2, second output %s", t.invertedG2 ? "inverted (CCSDS)" : "plain");
    kvRow(a, "frames", "%llu good, %llu failed RS, %llu bytes corrected (%.3f%% now)%s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
          (unsigned long long)t.rsCorrected, t.rsByteErrors * 100, t.frameLock ? ", in sync" : "");
    if (t.scid >= 0) kvRow(a, "VCDU", "spacecraft %d, %llu VCDUs, %llu fill, %llu gaps", t.scid, (unsigned long long)t.vcdus, (unsigned long long)t.fillVcdus, (unsigned long long)t.vcduGaps);
    kvRow(a, "packets", "%llu, %llu CRC errors", (unsigned long long)t.packets, (unsigned long long)t.crcErrors);
    kvRow(a, "files", "%llu, %llu with errors, %zu products kept", (unsigned long long)t.files, (unsigned long long)t.fileErrors, t.products.size());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    smart5GeoCard(a);   // round-5 smart checks (app/smart5, docs/SMART5-UI-3.9.md)
}

void panels(App& a) {
    const GoesTelemetry& t = dect2::elektroIsGgak(S.link) ? GoesTelemetry{} : a.rx.elektro.hrit;
    std::vector<cf32> pts;
    if (live(a) && dect2::elektroIsGgak(S.link)) pts = a.rx.elektro.ggak.constellation;
    else if (live(a)) pts = t.constellation;
    const float H = ImGui::GetContentRegionAvail().y;
    const float sz = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::BeginGroup();
    captionFit(sz, "Symbols");
    scatter("##elc", pts, ImVec2(sz, sz), 2.0, pal::okGreen());
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const ElektroTelemetry& e = a.rx.elektro;
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : e.hrit.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", !on ? 0 : e.hrit.state >= 3 ? 1 : e.hrit.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::elektroSummary(e).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = dect2::elektroCraftName(S.craft);
    if (live(a)) l2 = dect2::elektroSummary(a.rx.elektro);
}

void galleryPublish(const GoesTelemetry& t) {
    static std::set<uint64_t> done;
    for (const auto& p : t.products) {
        if (done.count(p->id)) continue;
        bool fin;
        { std::lock_guard<std::mutex> lk(p->mu); fin = p->complete; }
        if (!fin) continue;
        done.insert(p->id);
        gallery::Meta m;
        m.detail = p->name;
        std::lock_guard<std::mutex> lk(p->mu);
        const std::string key = "elektro-" + std::to_string((long long)time(nullptr)) + "-" + std::to_string(p->id);
        if (p->w > 0 && p->h > 0 && p->pix.size() >= (size_t)p->w * p->h) gallery::publishGray(key, "Elektro-L", (double)time(nullptr), p->w, p->h, p->pix.data(), m);
        else if (p->hdr.hasNoaa && (p->hdr.noaaCompression == 2 || p->hdr.noaaCompression == 5)) gallery::publishFile(key, "Elektro-L", (double)time(nullptr), p->data, m);
    }
}

void tick(App& a) {
    loadState();
    if (S.appliedCraft != S.craft) { a.engine.elektro().setCraft(S.craft); S.appliedCraft = S.craft; }
    if (S.appliedLink != S.link) { a.engine.elektro().setLink(S.link); S.appliedLink = S.link; }
    if (live(a)) autoSave(a.rx.elektro.hrit);
    if (live(a)) galleryPublish(a.rx.elektro.hrit);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Satellite");
    sameLineIf(90 * gUi, 5 * gUi);
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::Combo("##elcraft", &S.craft, kCraft, 3)) {
        plat::prefs().setI("elektroCraft", S.craft); plat::prefs().flush();
        a.engine.elektro().setCraft(S.craft); S.appliedCraft = S.craft;
        tuneFreq(a, dect2::elektroCraftMhz(S.craft)); retune = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Elektro-L N3 at 76°E, N4 at 165.8°E, N5 at 14.5°W. All three send L-band HRIT around 1691 MHz.");
    flowNext(12 * gUi);
    ImGui::TextDisabled("Link");
    sameLineIf(90 * gUi, 5 * gUi);
    ImGui::SetNextItemWidth(180 * gUi);
    if (ImGui::Combo("##ellink", &S.link, kLink, 7)) {
        plat::prefs().setI("elektroLink", S.link); plat::prefs().flush();
        a.engine.elektro().setLink(S.link); S.appliedLink = S.link;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("HRIT 665.4 ksym/s is the usual Elektro-L L-band broadcast. LRIT is slower (64 or 128 ksym/s), sometimes on 1693 MHz.");
    flowNext(12 * gUi);
    ImGui::TextDisabled("Frequency");
    sameLineIf(90 * gUi, 5 * gUi);
    if (ImGui::Button("1691 MHz")) { tuneFreq(a, 1691.0); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("L-band HRIT (and often LRIT). Point a dish with an LNA at the satellite; receive only, nothing is transmitted.");
    flowNext(8 * gUi);
    if (ImGui::Button("1693 MHz")) { tuneFreq(a, 1693.0); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Alternate LRIT listing for some Elektro-L spacecraft.");
    flowEnd();
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const GoesTelemetry& t = a.rx.elektro.hrit;
    out.push_back({"Es/N0  dB", "%.1f", t.snrDb, 0, 15, t.snrDb >= 6 ? 1 : t.snrDb >= 3 ? 2 : 3});
    const double fps = t.rateHist.empty() ? 0.0 : t.rateHist.back();
    out.push_back({"FRAMES/s", "%.0f", fps, 0, 60, t.state >= 3 ? 1 : 0});
}

} // namespace

// Simple view: the picture with its controls, and the signal and the products / channels beside it
static void command(App& a) {
    modepanel::panes({{"##elkcpic", nullptr, [&a] { tab(a); }, 2.2f, 300.f},
                      {"##elkcsig", tr("Signal"), [&a] { receiver(a); ImGui::Spacing(); list(a); }, 1.f, 240.f}}, 340.f);
}

extern const ModeUi kElektroUi;
const ModeUi kElektroUi = {
    .sideTitle = "PRODUCTS",
    .tabName = "Products",
    .tabIcon = Ic::Camera,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
    .meters = meters,
    .command = command,
};
