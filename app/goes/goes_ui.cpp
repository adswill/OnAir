// GOES HRIT screens: the product list (full disk, mesoscale, CONUS and relayed images, EMWIN text, admin messages) on the right; the
// selected product in the Products tab: an image that grows as its lines arrive (Rice and plain images; JPEG / GIF once complete) or
// its text (EMWIN ZIP files unpacked); Save, and Save automatically into <data folder>/goes/. The receiver's state and the constellation.
// A dish (about 1 m, or a grid) with an LNA at the feed is needed: a note says so while nothing is received.
#include "app.h"
#include "panels/gallery.h"
#include "gfx/gfx.h"
#include "map/wx_overlay.h"
#include "platform/platform.h"
#include "dect2/goes/goes_proto.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/goes/goes_tel.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using dect2::GoesTelemetry;
using dect2::goes::Product;
namespace goes = dect2::goes;

const char* const kFilters[] = {"All products", "Images", "Text"};

struct State {
    bool loaded = false, caduSet = false;
    bool autoSave = true;
    bool saveCadu = false;                // write the good frames to <data folder>/goes/frames.cadu
    int filter = 0;
    bool follow = true;                   // show the newest image as it comes
    uint64_t selId = 0;
    // the texture of the shown product
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
    S.autoSave = plat::prefs().getB("goesAutoSave", true);
    S.filter = std::clamp((int)plat::prefs().getI("goesFilter", 0), 0, 2);
    S.saveCadu = plat::prefs().getB("goesSaveCadu", false);
}

// good frames as 1024 byte CADUs (sync word, then the derandomised 1020 bytes), the file other decoders write too: compare or feed it back
std::mutex gCaduMu;
std::ofstream gCadu;
void setCaduSaving(App& a, bool on) {
    {
        std::lock_guard<std::mutex> lk(gCaduMu);
        if (gCadu.is_open()) gCadu.close();
        if (on) {
            const std::string path = plat::dataDir() + "/goes/frames.cadu";
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>((plat::dataDir() + "/goes").c_str())), ec);
            gCadu.open(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary | std::ios::app);
        }
    }
    a.engine.goes().setCaduCallback(on ? std::function<void(const uint8_t*)>([](const uint8_t* f) {
        std::lock_guard<std::mutex> lk(gCaduMu);
        if (gCadu.is_open()) gCadu.write((const char*)f, 1024);
    }) : std::function<void(const uint8_t*)>());
}

bool live(const App& a) { return a.rx.standard == 61; }   // the engine reports its standard code minus one

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

// ---------------------------------------------------------------- saving
bool writeFile(const std::string& path, const uint8_t* d, size_t n) {
    std::ofstream o(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!o) return false;
    o.write((const char*)d, (std::streamsize)n);
    return (bool)o;
}

std::string goesDir() {
    const std::string dir = plat::dataDir() + "/goes";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    return dir;
}

// the annotation as a file name, its extension replaced by `ext`
std::string fileName(const Product& p, const char* ext) {
    std::string n = p.name;
    for (char& c : n) if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    const size_t dot = n.rfind('.');
    if (dot != std::string::npos && dot + 6 >= n.size()) n.resize(dot);
    if (n.empty()) n = "goes-" + std::to_string(p.id);
    return n + ext;
}

// one product into the folder: images as PNG (grey), pictures and ZIP files as they came, text as .txt
bool saveProduct(const Product& p, std::string& where) {
    const std::string dir = goesDir();
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

// ---------------------------------------------------------------- the picture
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
            step = std::max(1, (std::max(p.w, p.h) + 4095) / 4096);   // big images shown at a fraction
            w = p.w / step; h = p.h / step;
            px.resize((size_t)w * h);
            for (int y = 0; y < h; y++) {
                const uint8_t* r = p.pix.data() + (size_t)y * step * p.w;
                for (int x = 0; x < w; x++) { const uint32_t v = r[(size_t)x * step]; px[(size_t)y * w + x] = 0xFF000000u | v << 16 | v << 8 | v; }
            }
        } else if (p.complete && !p.data.empty()) {
            char name[64];
            snprintf(name, sizeof name, "/goes_%llu.img", (unsigned long long)p.id);
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
    if (p.kind == goes::KFullDisk && p.w > 0 && wxoverlay::wanted(a, 10.0)) {   // the full disk on the map (View > Map), rows not received yet clear
        std::vector<uint32_t> px; int w = 0, hh = 0; uint64_t ver = 0;
        {
            std::lock_guard<std::mutex> lk(p.mu);
            const int step = std::max(1, (std::max(p.w, p.h) + 2047) / 2048);
            w = p.w / step; hh = p.h / step; ver = p.version;
            px.assign((size_t)w * hh, 0u);
            for (int y = 0; y < hh; y++) {
                const size_t sy = (size_t)y * step;
                if (sy >= p.rowDone.size() || !p.rowDone[sy]) continue;
                const uint8_t* r = p.pix.data() + sy * p.w;
                for (int x = 0; x < w; x++) { const uint32_t v = r[(size_t)x * step]; px[(size_t)y * w + x] = 0xFF000000u | v << 16 | v << 8 | v; }
            }
        }
        wxoverlay::setGeos(p.id, ver, p.name, w, hh, px);
    }
    if (isImage(p)) {
        upload(p);
        if (!S.tex || S.texId != p.id) { ImGui::TextDisabled("%s", S.texFailed ? "this picture could not be decoded here (it is saved as it came)" : "waiting for the picture"); return; }
        ImGui::BeginChild("##goespic", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
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
    ImGui::BeginChild("##goestxt", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(txt.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- tabs
void tab(App& a) {
    loadState();
    if (!S.caduSet) { S.caduSet = true; if (S.saveCadu) setCaduSaving(a, true); }
    const GoesTelemetry& t = a.rx.goes;
    const bool on = live(a);
    if (on) autoSave(t);
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##goesf", &S.filter, kFilters, 3)) { plat::prefs().setI("goesFilter", S.filter); plat::prefs().flush(); }
    flowNext(8 * gUi);
    ImGui::Checkbox("Newest", &S.follow);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the newest image as it arrives (choosing a product in the list turns this off)");
    flowNext(8 * gUi);
    auto sel = on ? selected(t) : nullptr;
    if (ImGui::Button("Save") && sel) { std::string where; S.msg = saveProduct(*sel, where) ? "saved " + where : "nothing to save yet"; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The product shown, into <data folder>/goes/: images as PNG, pictures and ZIP files as they came, text as .txt");
    flowNext(5 * gUi);
    if (ImGui::Checkbox("Save automatically", &S.autoSave)) { plat::prefs().setB("goesAutoSave", S.autoSave); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every product once it is complete, into <data folder>/goes/");
    flowNext(5 * gUi);
    if (ImGui::Checkbox(tr("Save CADU"), &S.saveCadu)) { plat::prefs().setB("goesSaveCadu", S.saveCadu); plat::prefs().flush(); setCaduSaving(a, S.saveCadu); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Every frame Reed-Solomon passes, as 1024 byte CADUs into <data folder>/goes/frames.cadu (the same kind of file SatDump writes), to compare decoders"));
    flowNext(5 * gUi);
    if (ImGui::Button(tr("Open CADU"))) {
        const std::string f = plat::openFileDialog();
        if (!f.empty()) {
            std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>(f.c_str())), std::ios::binary);
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (d.size() >= 1024) { a.engine.goes().importCadu(std::move(d)); S.msg = "CADU file read, decoding"; }
            else S.msg = "not a CADU file";
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Decode a .cadu file (1024 byte frames, from this program or SatDump) through Reed-Solomon, the packets and the files; GOES HRIT must be running"));
    flowNext(5 * gUi);
    if (ImGui::Button("Clear")) { a.engine.goes().clearProducts(); S.msg.clear(); S.selId = 0; S.follow = true; }
    flowEnd();
    if (!S.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.msg.c_str()); ImGui::PopTextWrapPos(); }
    if (!sel) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !a.engine.running() ? "Start the receiver. GOES HRIT needs a dish (about 1 m, or a 1.7 GHz grid antenna) pointed at the satellite, with an LNA and filter at the feed."
                                                      : on ? dect2::goesSummary(t).c_str() : "starting");
        ImGui::PopTextWrapPos();
        return;
    }
    showProduct(a, *sel);
}

void list(App& a) {
    loadState();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const GoesTelemetry& t = a.rx.goes;
    if (t.products.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no products yet"); ImGui::PopTextWrapPos(); return; }
    if (!ImGui::BeginTable("##goesl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) return;
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
    const GoesTelemetry& t = a.rx.goes;
    kvRow(a, "state", "%s", dect2::goesSummary(t).c_str());
    kvRow(a, "carrier", "offset %+.2f kHz, %s, lock %.2f, Es/N0 %.1f dB", t.cfoHz / 1e3, t.carrierLock ? "locked" : "searching", t.lockMetric, t.snrDb);
    kvRow(a, "symbol clock", "%.1f ksym/s (%+.0f ppm)", t.symbolRate / 1e3, (t.symbolRate / 927000.0 - 1) * 1e6);
    kvRow(a, "code", "K = 7, r = 1/2, second output %s; Reed-Solomon (255,223) x4, %s basis", t.invertedG2 ? "inverted (CCSDS)" : "plain", t.rsDualBasis ? "dual" : "conventional");
    kvRow(a, "frames", "%llu good, %llu failed RS, %llu bytes corrected (%.3f%% now)%s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
          (unsigned long long)t.rsCorrected, t.rsByteErrors * 100, t.frameLock ? ", in sync" : "");
    if (t.scid >= 0) kvRow(a, "VCDU", "spacecraft %d, %llu VCDUs, %llu fill, %llu gaps", t.scid, (unsigned long long)t.vcdus, (unsigned long long)t.fillVcdus, (unsigned long long)t.vcduGaps);
    if (!t.vcids.empty()) {
        std::string s;
        for (const auto& v : t.vcids) { if (!s.empty()) s += ", "; s += std::to_string(v.first) + ": " + std::to_string(v.second); }
        kvRow(a, "virtual channels", "%s", s.c_str());
    }
    kvRow(a, "packets", "%llu, %llu CRC errors", (unsigned long long)t.packets, (unsigned long long)t.crcErrors);
    kvRow(a, "files", "%llu, %llu with errors, %zu products kept", (unsigned long long)t.files, (unsigned long long)t.fileErrors, t.products.size());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    if (!t.log.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Log");
        ImGui::PushFont(a.mono, 0);
        for (size_t i = 0; i < t.log.size() && i < 40; i++) { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.log[i].c_str()); ImGui::PopTextWrapPos(); }
        ImGui::PopFont();
    }
}

void panels(App& a) {
    const GoesTelemetry& t = a.rx.goes;
    const float H = ImGui::GetContentRegionAvail().y;
    const float sz = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::BeginGroup();
    captionFit(sz, "Symbols");
    std::vector<cf32> pts;
    if (live(a)) pts = t.constellation;
    scatter("##goesc", pts, ImVec2(sz, sz), 2.0, pal::okGreen());
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const GoesTelemetry& t = a.rx.goes;
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", !on ? 0 : t.state >= 3 ? 1 : t.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::goesSummary(t).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "GOES HRIT";
    if (live(a)) l2 = dect2::goesSummary(a.rx.goes);
}

// finished pictures go to the gallery (View > Images): images as grey, JPEG / GIF as they came
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
        const std::string key = "goes-" + std::to_string((long long)time(nullptr)) + "-" + std::to_string(p->id);
        if (p->w > 0 && p->h > 0 && p->pix.size() >= (size_t)p->w * p->h) gallery::publishGray(key, "GOES HRIT", (double)time(nullptr), p->w, p->h, p->pix.data(), m);
        else if (p->hdr.hasNoaa && (p->hdr.noaaCompression == 2 || p->hdr.noaaCompression == 5)) gallery::publishFile(key, "GOES HRIT", (double)time(nullptr), p->data, m);
    }
}

void tick(App& a) {
    loadState();
    if (live(a)) autoSave(a.rx.goes);
    if (live(a)) galleryPublish(a.rx.goes);
}

void tuner(App& a, bool& retune) {
    ImGui::TextDisabled("Frequency");
    sameLineIf(90 * gUi, 5 * gUi);
    if (ImGui::Button("1694.1 MHz")) { tuneFreq(a, 1694.1); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("GOES-16 / 18 / 19 HRIT and EMWIN (BPSK 927 ksym/s). Point a dish (about 1 m, or a grid antenna) at the\n"
                                                  "satellite with an LNA and a 1.7 GHz filter at the feed; the signal is far below the noise of a plain antenna.");
    flowEnd();
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const GoesTelemetry& t = a.rx.goes;
    out.push_back({"Es/N0  dB", "%.1f", t.snrDb, 0, 15, t.snrDb >= 6 ? 1 : t.snrDb >= 3 ? 2 : 3});
    const double fps = t.rateHist.empty() ? 0.0 : t.rateHist.back();
    out.push_back({"FRAMES/s", "%.0f", fps, 0, 60, t.state >= 3 ? 1 : 0});
}

} // namespace

extern const ModeUi kGoesUi;
const ModeUi kGoesUi = {
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
};
