// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GK-2A LRIT (family 230) and HRIT (family 231) screens: the xRIT product list (full-disk pictures per channel put together from their
// segments, schedule and additional-data text), the receiver details, a "why not decoding" line, and the KMA decryption keys.
#include "ui/kit/mode_panel.h"
#include "app.h"
#include "ease/ease_ui.h"
#include "panels/gallery.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/gk2a/gk2a_rx.h"
#include "dect2/gk2a/gk2a_tel.h"
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

using dect2::Gk2aTelemetry;
using dect2::goes::Product;
namespace goes = dect2::goes;

const char* const kFilters[] = {"All products", "Pictures", "Text"};

struct State {
    bool loaded = false, autoSave = true, saveRaw = true, stretch = true, keysApplied = false, cbSet = false;
    int filter = 0;
    bool follow = true;
    uint64_t selId = 0;
    gfx::Image* tex = nullptr;
    uint64_t texId = 0, texVersion = 0;
    int texW = 0, texH = 0;
    double texTime = 0;
    std::set<uint64_t> saved;
    std::string msg;
};
State S[2];

int lk(const App& a) { return a.family == 231 ? 1 : 0; }
const char* key(int link, const char* what) { static char b[2][64]; snprintf(b[link], sizeof b[link], "gk2a%s%s", link ? "H" : "L", what); return b[link]; }
dect2::Gk2aReceiver& rxOf(App& a, int link) { return link ? a.engine.gk2aHrit() : a.engine.gk2aLrit(); }
const Gk2aTelemetry& telOf(const App& a) { return a.rx.gk2a; }
bool live(const App& a) { return a.rx.standard == (lk(a) ? 232 : 231); }   // the engine reports its standard code minus one

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void loadState(int link) {
    State& s = S[link];
    if (s.loaded) return;
    s.loaded = true;
    s.autoSave = plat::prefs().getB(key(link, "AutoSave"), true);
    s.saveRaw = plat::prefs().getB(key(link, "SaveRaw"), true);
    s.stretch = plat::prefs().getB(key(link, "Stretch"), true);
    s.filter = std::clamp((int)plat::prefs().getI(key(link, "Filter"), 0), 0, 2);
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

bool isImage(const Product& p) { return p.w > 0 || p.hdr.hasImage; }
bool isText(const Product& p) { return !isImage(p); }
bool passes(const State& s, const Product& p) { return s.filter == 0 || (s.filter == 1 ? isImage(p) : isText(p)); }

std::shared_ptr<Product> selected(const State& s, const Gk2aTelemetry& t) {
    if (s.follow) {
        for (const auto& p : t.products) if (isImage(*p) && passes(s, *p)) return p;
        for (const auto& p : t.products) if (passes(s, *p)) return p;
        return nullptr;
    }
    for (const auto& p : t.products) if (p->id == s.selId) return p;
    return nullptr;
}

bool writeFile(const std::string& path, const uint8_t* d, size_t n) {
    std::ofstream o(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!o) return false;
    o.write((const char*)d, (std::streamsize)n);
    return (bool)o;
}

std::string gkDir(const char* sub) {
    const std::string dir = plat::dataDir() + "/gk2a" + (sub && *sub ? std::string("/") + sub : std::string());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    return dir;
}

std::string safeName(std::string n) {
    for (char& c : n) if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    const size_t dot = n.rfind('.');
    if (dot != std::string::npos && dot + 6 >= n.size()) n.resize(dot);
    return n;
}

// 8 bit grey with an optional percentile stretch (1 % .. 99 % of the rows received)
void toPixels(const Product& p, bool stretch, std::vector<uint32_t>& px, int& w, int& h, int step) {
    w = p.w / step; h = p.h / step;
    px.assign((size_t)w * h, 0xFF000000u);
    uint8_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = (uint8_t)i;
    if (stretch) {
        uint64_t hist[256] = {};
        uint64_t n = 0;
        for (int y = 0; y < p.h; y += 2) if (p.rowDone[(size_t)y]) for (int x = 0; x < p.w; x += 2) { hist[p.pix[(size_t)y * p.w + x]]++; n++; }
        if (n > 100) {
            uint64_t acc = 0; int lo = 0, hi = 255;
            for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc * 100 >= n * 1) { lo = i; break; } }
            acc = 0;
            for (int i = 255; i >= 0; i--) { acc += hist[i]; if (acc * 100 >= n * 1) { hi = i; break; } }
            if (hi > lo + 8) for (int i = 0; i < 256; i++) lut[i] = (uint8_t)std::clamp((i - lo) * 255 / (hi - lo), 0, 255);
        }
    }
    for (int y = 0; y < h; y++) {
        const uint8_t* r = p.pix.data() + (size_t)y * step * p.w;
        for (int x = 0; x < w; x++) { const uint32_t v = lut[r[(size_t)x * step]]; px[(size_t)y * w + x] = 0xFF000000u | v << 16 | v << 8 | v; }
    }
}

bool saveProduct(int link, const Product& p, std::string& where) {
    State& s = S[link];
    const std::string dir = gkDir("");
    std::lock_guard<std::mutex> lk2(p.mu);
    if (p.w > 0 && p.h > 0 && p.rowsDone > 0) {
        std::vector<uint32_t> px;
        int w, h;
        toPixels(p, s.stretch, px, w, h, 1);
        where = dir + "/" + safeName(p.name) + ".png";
        return gfx::writePng(where.c_str(), reinterpret_cast<const uint8_t*>(px.data()), w, h);
    }
    if (p.data.empty() && p.text.empty()) return false;
    where = dir + "/" + safeName(p.name) + ".txt";
    return writeFile(where, reinterpret_cast<const uint8_t*>(p.text.empty() ? (const char*)p.data.data() : p.text.data()), p.text.empty() ? p.data.size() : p.text.size());
}

void autoSave(int link, const Gk2aTelemetry& t) {
    State& s = S[link];
    if (!s.autoSave) return;
    for (const auto& p : t.products) {
        if (s.saved.count(p->id)) continue;
        bool done;
        { std::lock_guard<std::mutex> l(p->mu); done = p->complete; }
        if (!done) continue;
        s.saved.insert(p->id);
        std::string where;
        if (saveProduct(link, *p, where)) s.msg = "saved " + where;
    }
}

void upload(int link, const Product& p) {
    State& s = S[link];
    const double now = nowSec();
    const bool same = p.id == s.texId;
    uint64_t ver;
    { std::lock_guard<std::mutex> l(p.mu); ver = p.version; }
    if (same && (ver == s.texVersion || (now - s.texTime < 0.5 && s.tex))) return;
    if (!gGfx) return;
    s.texTime = now; s.texVersion = ver;
    if (!same) { delete s.tex; s.tex = nullptr; s.texId = p.id; s.texW = s.texH = 0; }
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> l(p.mu);
        if (p.w <= 0 || p.h <= 0) return;
        toPixels(p, s.stretch, px, w, h, std::max(1, (std::max(p.w, p.h) + 4095) / 4096));
    }
    if (!s.tex || s.texW != w || s.texH != h) { delete s.tex; s.tex = gGfx->createImage(w, h, 0xFF000000u); s.texW = w; s.texH = h; }
    if (s.tex) s.tex->update(0, 0, w, h, px.data());
}

void showProduct(App& a, int link, const Product& p) {
    State& s = S[link];
    int rows, h, segs, segsT, locked, keyNo;
    bool complete, errors, needKey;
    uint64_t bytes;
    std::string note;
    {
        std::lock_guard<std::mutex> l(p.mu);
        rows = p.rowsDone; h = p.h; segs = p.segsDone; segsT = p.segsTotal; complete = p.complete; errors = p.errors; bytes = p.bytes;
        locked = p.segsLocked; needKey = p.needKey; note = p.note; keyNo = p.keyNo;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.name.c_str()); ImGui::PopFont();
    char info[320];
    if (isImage(p)) snprintf(info, sizeof info, "%s, %d x %d, %d bits, JPEG 2000, %d of %d segments%s%s%s", p.channelIdx >= 0 ? goes::gk2aChannelName(p.channelIdx) : goes::kindName(p.kind), p.w, h, p.hdr.bpp, segs, segsT,
                           locked ? (", " + std::to_string(locked) + " encrypted (key " + std::to_string(keyNo) + ")").c_str() : "", complete ? "" : ", receiving", errors ? ", with errors" : "");
    else snprintf(info, sizeof info, "%s, %llu bytes, VC %d%s%s", goes::kindName(p.kind), (unsigned long long)bytes, p.vcid, complete ? "" : ", receiving", errors ? ", with errors" : "");
    ImGui::TextDisabled("%s", info);
    if (!note.empty()) ImGui::TextColored(needKey ? pal::warnAmber() : pal::badRed(), "%s", note.c_str());
    ImGui::PopTextWrapPos();
    if (isImage(p)) {
        if (rows == 0) { ImGui::TextDisabled("%s", needKey ? "waiting for the decryption key (Import keys)" : "waiting for the picture"); return; }
        upload(link, p);
        if (!s.tex || s.texId != p.id) { ImGui::TextDisabled("waiting for the picture"); return; }
        ImGui::BeginChild("##gkpic", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        const ImVec2 av = ImGui::GetContentRegionAvail();
        const float z = std::max(0.05f, std::min(av.x / s.texW, std::max(av.y, 100.f) / s.texH));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddImage(s.tex->texture(), p0, ImVec2(p0.x + s.texW * z, p0.y + s.texH * z));
        ImGui::Dummy(ImVec2(s.texW * z, s.texH * z));
        ImGui::EndChild();
        return;
    }
    std::string txt;
    { std::lock_guard<std::mutex> l(p.mu); txt = p.text; }
    if (txt.empty()) { ImGui::TextDisabled("%s", needKey ? "encrypted: import the KMA key file" : complete ? "no text in this product" : "receiving"); return; }
    for (char& c : txt) if (c == '\r') c = ' '; else if ((unsigned char)c < 32 && c != '\n' && c != '\t') c = '.';
    ImGui::BeginChild("##gktxt", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(txt.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    ImGui::EndChild();
}

void importKeyFile(App& a, int link) {
    State& s = S[link];
    const std::string f = plat::openFileDialog();
    if (f.empty()) return;
    std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>(f.c_str())), std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const int n = rxOf(a, link).importKeys(d);
    if (n <= 0) { s.msg = "no keys in that file (expected EncryptionKeyMessage.bin: a 2 byte count and then 2 byte index + 8 byte key each, or lines index:hexkey)"; return; }
    // kept in the data folder, read again at the next start; both GK-2A modes share it
    writeFile(gkDir("") + "/keys.bin", d.data(), d.size());
    rxOf(a, 1 - link).importKeys(d);
    s.msg = std::to_string(n) + " key(s) loaded";
}

void tab(App& a) {
    const int link = lk(a);
    State& s = S[link];
    loadState(link);
    const Gk2aTelemetry& t = telOf(a);
    const bool on = live(a);
    if (on) autoSave(link, t);
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##gkf", &s.filter, kFilters, 3)) { plat::prefs().setI(key(link, "Filter"), s.filter); plat::prefs().flush(); }
    flowNext(8 * gUi);
    ImGui::Checkbox("Newest", &s.follow);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the newest picture as it arrives (choosing a product in the list turns this off)");
    flowNext(8 * gUi);
    if (ImGui::Checkbox("Stretch", &s.stretch)) { plat::prefs().setB(key(link, "Stretch"), s.stretch); plat::prefs().flush(); s.texVersion = 0; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Spread the brightness of the picture over the full range (the channel counts use only part of it)");
    flowNext(8 * gUi);
    auto sel = on ? selected(s, t) : nullptr;
    if (ImGui::Button("Save") && sel) { std::string where; s.msg = saveProduct(link, *sel, where) ? "saved " + where : "nothing to save yet"; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The product shown, into <data folder>/gk2a/: pictures as PNG, text as .txt");
    flowNext(5 * gUi);
    if (ImGui::Checkbox("Save automatically", &s.autoSave)) { plat::prefs().setB(key(link, "AutoSave"), s.autoSave); plat::prefs().flush(); }
    flowNext(5 * gUi);
    if (ImGui::Checkbox("Save .lrit files", &s.saveRaw)) { plat::prefs().setB(key(link, "SaveRaw"), s.saveRaw); plat::prefs().flush(); s.cbSet = false; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every file as it was sent (headers and the still-encrypted or plain data), into <data folder>/gk2a/lrit/, so other xRIT tools can use them");
    flowNext(5 * gUi);
    if (ImGui::Button(link ? "Import keys" : "Import keys")) importKeyFile(a, link);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("KMA encrypts the pictures with DES. Choose your decryption key file (EncryptionKeyMessage.bin, as KMA issues it for your station and after the station-specific layer is removed, or lines of index:hexkey). The keys stay on this computer; none are built in.");
    flowNext(5 * gUi);
    if (ImGui::Button("Clear")) { rxOf(a, link).clearProducts(); s.msg.clear(); s.selId = 0; s.follow = true; }
    flowEnd();
    if (!s.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", s.msg.c_str()); ImGui::PopTextWrapPos(); }
    if (on && !t.hint.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", t.hint.c_str()); ImGui::PopTextWrapPos(); }
    if (!sel) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !a.engine.running()
            ? (link ? "Start the receiver. GK-2A HRIT (1695.4 MHz, QPSK 3 Msym/s) needs a dish of about 1.2 m or more with an LNA and filter at the feed, a radio that does at least 6.4 Msps (HackRF, Airspy R2, SDRplay), and GK-2A above your horizon (East Asia, Australia, western Pacific). Receive only."
                    : "Start the receiver. GK-2A LRIT (1692.14 MHz, BPSK 128 ksym/s) needs a dish of about 0.9 m or more (or a good grid) with an LNA and filter at the feed, and GK-2A above your horizon (East Asia, Australia, western Pacific). Receive only.")
            : on ? dect2::gk2aSummary(t).c_str() : "starting");
        ImGui::PopTextWrapPos();
        return;
    }
    showProduct(a, link, *sel);
}

void list(App& a) {
    const int link = lk(a);
    State& s = S[link];
    loadState(link);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const Gk2aTelemetry& t = telOf(a);
    if (t.products.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no products yet"); ImGui::PopTextWrapPos(); return; }
    if (!ImGui::BeginTable("##gkl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupColumn("Product", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Done");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    const auto cur = selected(s, t);
    for (const auto& p : t.products) {
        if (!passes(s, *p)) continue;
        int segs, segsT, locked; bool complete, errors; uint64_t bytes;
        { std::lock_guard<std::mutex> l(p->mu); segs = p->segsDone; segsT = p->segsTotal; locked = p->segsLocked; complete = p->complete; errors = p->errors; bytes = p->bytes; }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)p->id);
        if (ImGui::Selectable(p->name.c_str(), cur && cur->id == p->id, ImGuiSelectableFlags_SpanAllColumns)) { s.selId = p->id; s.follow = false; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nvirtual channel %d, APID %d", p->name.c_str(), p->vcid, p->apid);
        ImGui::PopID();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", p->channelIdx >= 0 ? goes::gk2aChannelName(p->channelIdx) : goes::kindName(p->kind));
        ImGui::TableNextColumn();
        if (isImage(*p)) {
            if (locked) ImGui::TextDisabled("%d/%d key?", segs + locked, segsT);
            else ImGui::TextDisabled("%d/%d%s", segs, segsT, errors ? " !" : "");
        } else ImGui::TextDisabled("%s", complete ? (errors ? "errors" : (bytes >= 10240 ? (std::to_string(bytes / 1024) + " kB").c_str() : (std::to_string(bytes) + " B").c_str())) : "...");
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    easeui::alignButton(a, "gk2a", a.rx.gk2a.snrDb, live(a) && a.rx.gk2a.snrDb > 0);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const Gk2aTelemetry& t = telOf(a);
    const int link = t.link;
    kvRow(a, "state", "%s", dect2::gk2aSummary(t).c_str());
    kvRow(a, "satellite", "GK-2A (GEO-KOMPSAT-2A), 128.2°E");
    kvRow(a, "link", "%s, %.2f MHz, %s %.0f ksym/s, RRC 0.5", dect2::gk2aLinkName(link), dect2::gk2aMhz(link), link ? "QPSK" : "BPSK", dect2::gk2aSymbolRate(link) / 1e3);
    kvRow(a, "carrier", "offset %+.2f kHz, %s, lock %.2f, Es/N0 %.1f dB", t.cfoHz / 1e3, t.carrierLock ? "locked" : "searching", t.lockMetric, t.snrDb);
    const double want = dect2::gk2aSymbolRate(link);
    kvRow(a, "symbol clock", "%.1f ksym/s (%+.0f ppm)", t.symbolRate / 1e3, (t.symbolRate / want - 1) * 1e6);
    kvRow(a, "code", "K = 7, r = 1/2, second output %s, RS(255,223) x4", t.invertedG2 ? "inverted (CCSDS)" : "plain");
    kvRow(a, "frames", "%llu good, %llu failed RS, %llu bytes corrected (%.3f%% now)%s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
          (unsigned long long)t.rsCorrected, t.rsByteErrors * 100, t.frameLock ? ", in sync" : "");
    if (t.scid >= 0) kvRow(a, "VCDU", "spacecraft 0x%02X, %llu VCDUs, %llu fill, %llu gaps", t.scid, (unsigned long long)t.vcdus, (unsigned long long)t.fillVcdus, (unsigned long long)t.vcduGaps);
    kvRow(a, "packets", "%llu, %llu CRC errors", (unsigned long long)t.packets, (unsigned long long)t.crcErrors);
    kvRow(a, "files", "%llu, %llu with errors, %zu products kept", (unsigned long long)t.files, (unsigned long long)t.fileErrors, t.products.size());
    kvRow(a, "keys", "%d loaded%s", t.keysLoaded, t.segmentsLocked ? (", " + std::to_string(t.segmentsLocked) + " picture segments waiting for a key").c_str() : "");
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    if (!t.hint.empty()) kvRow(a, "hint", "%s", t.hint.c_str());
    smart5GeoCard(a);   // round-5 smart checks (app/smart5, docs/SMART5-UI-3.9.md)
}

void panels(App& a) {
    std::vector<cf32> pts;
    if (live(a)) pts = telOf(a).constellation;
    const float H = ImGui::GetContentRegionAvail().y;
    const float sz = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::BeginGroup();
    captionFit(sz, "Symbols");
    scatter("##gkc", pts, ImVec2(sz, sz), 2.0, pal::okGreen());
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const Gk2aTelemetry& t = telOf(a);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", !on ? 0 : t.state >= 3 ? 1 : t.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::gk2aSummary(t).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = dect2::gk2aLinkName(lk(a));
    if (live(a)) l2 = dect2::gk2aSummary(telOf(a));
}

void galleryPublish(int link, const Gk2aTelemetry& t) {
    static std::set<uint64_t> done[2];
    for (const auto& p : t.products) {
        if (done[link].count(p->id)) continue;
        bool fin;
        { std::lock_guard<std::mutex> l(p->mu); fin = p->complete; }
        if (!fin) continue;
        done[link].insert(p->id);
        gallery::Meta m;
        m.detail = p->name;
        std::lock_guard<std::mutex> l(p->mu);
        const std::string k = std::string(link ? "gk2ahrit-" : "gk2alrit-") + std::to_string((long long)time(nullptr)) + "-" + std::to_string(p->id);
        if (p->w > 0 && p->h > 0 && p->pix.size() >= (size_t)p->w * p->h) gallery::publishGray(k, dect2::gk2aLinkName(link), (double)time(nullptr), p->w, p->h, p->pix.data(), m);
    }
}

void tick(App& a) {
    const int link = lk(a);
    State& s = S[link];
    loadState(link);
    if (!s.keysApplied) {   // the keys the user imported earlier
        s.keysApplied = true;
        std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>((gkDir("") + "/keys.bin").c_str())), std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!d.empty()) { rxOf(a, 0).importKeys(d); rxOf(a, 1).importKeys(d); }
    }
    if (!s.cbSet) {   // every whole file as it came, named by its annotation header
        s.cbSet = true;
        if (s.saveRaw) {
            const std::string dir = gkDir("lrit");
            rxOf(a, link).setFileCallback([dir](const std::vector<uint8_t>& f, int, int) {
                goes::Headers h;
                if (!goes::parseHeaders(f.data(), f.size(), h, 1)) return;
                std::string n = safeName(h.annotation.empty() ? "file-" + std::to_string(f.size()) : h.annotation);
                writeFile(dir + "/" + n + ".lrit", f.data(), f.size());
            });
        } else rxOf(a, link).setFileCallback(nullptr);
    }
    if (live(a)) autoSave(link, telOf(a));
    if (live(a)) galleryPublish(link, telOf(a));
}

void tuner(App& a, bool& retune) {
    const int link = lk(a);
    loadState(link);
    ImGui::TextDisabled("Frequency");
    sameLineIf(90 * gUi, 5 * gUi);
    char b[32];
    snprintf(b, sizeof b, "%.2f MHz", dect2::gk2aMhz(link));
    if (ImGui::Button(b)) { tuneFreq(a, dect2::gk2aMhz(link)); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("GK-2A at 128.2°E. Point a dish with an LNA at it; receive only, nothing is transmitted. The receiver finds the carrier offset, the symbol clock and the polarity by itself.");
    flowNext(12 * gUi);
    ImGui::TextDisabled("%s", link ? "QPSK 3 Msym/s, needs 6.4 Msps or more" : "BPSK 128 ksym/s, any rate from 0.3 Msps");
    flowEnd();
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const Gk2aTelemetry& t = telOf(a);
    out.push_back({"Es/N0  dB", "%.1f", t.snrDb, -5.0, 15.0, t.snrDb >= (t.link ? 5 : 2) ? 1 : t.snrDb >= (t.link ? 2 : -1) ? 2 : 3});
    const double fps = t.rateHist.empty() ? 0.0 : t.rateHist.back();
    out.push_back({"FRAMES/s", "%.0f", fps, 0.0, t.link ? 400.0 : 8.0, t.state >= 3 ? 1 : 0});
}

} // namespace

// Simple view: the picture with its controls, and the signal and the products / channels beside it
static void command(App& a) {
    modepanel::panes({{"##gk2acpic", nullptr, [&a] { tab(a); }, 2.2f, 300.f},
                      {"##gk2acsig", tr("Signal"), [&a] { receiver(a); ImGui::Spacing(); list(a); }, 1.f, 240.f}}, 340.f);
}

extern const ModeUi kGk2aLritUi;
const ModeUi kGk2aLritUi = {
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
extern const ModeUi kGk2aHritUi;
const ModeUi kGk2aHritUi = kGk2aLritUi;
