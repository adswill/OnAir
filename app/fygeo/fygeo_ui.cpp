// FengYun geostationary screens (FY-4A / FY-4B LRIT, FY-4A HRIT-II/III, FY-2 S-VISSR): the same product list and picture viewer as GOES and Elektro-L,
// the DVB-S2 / BPSK lock stages of the link, the bit error ratio of S-VISSR and a plain-text hint when nothing decodes.
#include "ui/kit/mode_panel.h"
#include "app.h"
#include "panels/gallery.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/fygeo/fygeo_rx.h"
#include "dect2/fygeo/fygeo_tel.h"
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

using dect2::FygeoTelemetry;
using dect2::goes::Product;
namespace goes = dect2::goes;

const char* const kFilters[] = {"All products", "Images", "Text"};

struct State {
    bool loaded = false;
    bool autoSave = true;
    int filter = 0;
    bool follow = true;
    uint64_t selId = 0;
    int appliedRate[3] = {-1, -1, -1};
    int sat = 0;                               // the LRIT mode's satellite: 0 FY-4A, 1 FY-4B
    bool satApplied = false;
    float symKsym[4] = {0, 0, 0, 0};          // custom symbol rate (ksym/s) a link; 0 = the link's own
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
    S.autoSave = plat::prefs().getB("fygeoAutoSave", true);
    S.filter = std::clamp((int)plat::prefs().getI("fygeoFilter", 0), 0, 2);
    S.sat = std::clamp((int)plat::prefs().getI("fygeoSat", 0), 0, 1);
    for (int l = 0; l < 4; l++) S.symKsym[l] = (float)plat::prefs().getD(("fygeoSym" + std::to_string(l)).c_str(), 0.0);
}

// F: the mode (0 FY-4 LRIT, 1 FY-4A HRIT-II/III, 2 FY-2 S-VISSR; family 232 + F, active standard 233 + F). The link (the satellite of the LRIT mode) comes from the receiver.
template <int F> bool live(const App& a) { return a.rx.standard == 233 + F; }
template <int F> int linkNow(const App& a) { return F == 0 ? (live<F>(a) ? a.rx.fygeo.link : S.sat) : F == 1 ? dect2::kFy4aHrit : dect2::kFy2Svissr; }

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

bool isImage(const Product& p) { return p.w > 0; }
bool isText(const Product& p) { return !isImage(p) && (!p.text.empty() || p.kind == goes::KText || p.kind == goes::KAdmin); }
bool passes(const Product& p) { return S.filter == 0 || (S.filter == 1 ? isImage(p) : isText(p)); }

std::shared_ptr<Product> selected(const FygeoTelemetry& t) {
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

std::string fygeoDir() {
    const std::string dir = plat::dataDir() + "/fengyun";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    return dir;
}

std::string fileName(const Product& p, const char* ext) {
    std::string n = p.name;
    for (char& c : n) if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    const size_t dot = n.rfind('.');
    if (dot != std::string::npos && dot + 6 >= n.size()) n.resize(dot);
    if (n.empty()) n = "fengyun-" + std::to_string(p.id);
    return n + ext;
}

bool saveProduct(const Product& p, std::string& where) {
    const std::string dir = fygeoDir();
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

void autoSave(const FygeoTelemetry& t) {
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
            // a picture that is still arriving (S-VISSR grows a line at a time over 25 minutes): only the lines received so far
            int last = p.h;
            if (!p.complete && p.rowsDone > 0) {
                last = 0;
                for (int y = p.h - 1; y >= 0; y--) if (p.rowDone[(size_t)y]) { last = y + 1; break; }
                last = std::min(p.h, std::max(last, 8));
            }
            step = std::max(1, (std::max(p.w, last) + 4095) / 4096);
            w = p.w / step; h = last / step;
            if (h < 1) h = 1;
            px.resize((size_t)w * h);
            for (int y = 0; y < h; y++) {
                const uint8_t* r = p.pix.data() + (size_t)y * step * p.w;
                for (int x = 0; x < w; x++) { const uint32_t v = r[(size_t)x * step]; px[(size_t)y * w + x] = 0xFF000000u | v << 16 | v << 8 | v; }
            }
        } else if (p.complete && !p.data.empty()) {
            char name[64];
            snprintf(name, sizeof name, "/fygeo_%llu.img", (unsigned long long)p.id);
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
        ImGui::BeginChild("##fypic", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
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
    ImGui::BeginChild("##fytxt", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(txt.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    ImGui::EndChild();
}

const char* const kDish[4] = {
    "FY-4A LRIT (1697 MHz, DVB-S2 QPSK 2/5, 90 ksym/s) is a narrow signal: a dish of about 1 m with an LNA and a 1.7 GHz band-pass filter at the feed, pointed at 104.7°E.",
    "FY-4B LRIT (1697 MHz, DVB-S2 QPSK 8/9, 120 ksym/s): a dish of about 1 m with an LNA and a 1.7 GHz band-pass filter at the feed, pointed at FY-4B (its slot changes, check its current longitude).",
    "FY-4A HRIT-II/III (1679 MHz, DVB-S2 QPSK 5/6, 1 Msym/s) needs more gain: a dish of about 1.2 m or more with an LNA and a filter at the feed, pointed at 104.7°E.",
    "FY-2 S-VISSR (1687.5 MHz, BPSK 660 ksym/s) with a dish of about 1.2 m, an LNA and a filter at the feed, pointed at FY-2G (105°E) or FY-2H (79°E). A picture takes about 25 minutes to scan.",
};
const char* const kSat[4] = {"FY-4A, 104.7°E", "FY-4B", "FY-4A, 104.7°E", "FY-2G 105°E / FY-2H 79°E"};

template <int F> void tab(App& a) {
    loadState();
    const FygeoTelemetry& t = a.rx.fygeo;
    const bool on = live<F>(a);
    const int L = linkNow<F>(a);
    if (on) autoSave(t);
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##fyf", &S.filter, kFilters, 3)) { plat::prefs().setI("fygeoFilter", S.filter); plat::prefs().flush(); }
    flowNext(8 * gUi);
    ImGui::Checkbox("Newest", &S.follow);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the newest image as it arrives (choosing a product in the list turns this off)");
    flowNext(8 * gUi);
    auto sel = on ? selected(t) : nullptr;
    if (ImGui::Button("Save") && sel) { std::string where; S.msg = saveProduct(*sel, where) ? "saved " + where : "nothing to save yet"; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The product shown, into <data folder>/fengyun/: images as PNG, other files as they came, text as .txt");
    flowNext(5 * gUi);
    if (ImGui::Checkbox("Save automatically", &S.autoSave)) { plat::prefs().setB("fygeoAutoSave", S.autoSave); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every product once it is complete, into <data folder>/fengyun/");
    flowNext(5 * gUi);
    if (ImGui::Button("Clear")) { a.engine.fygeo().clearProducts(); S.msg.clear(); S.selId = 0; S.follow = true; }
    flowEnd();
    if (!S.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.msg.c_str()); ImGui::PopTextWrapPos(); }
    if (on && !t.hint.empty() && t.state < 3) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", t.hint.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!sel) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !a.engine.running() ? kDish[L] : on ? dect2::fygeoSummary(t).c_str() : "starting");
        ImGui::TextDisabled("%s", "Receive only: nothing is transmitted.");
        ImGui::PopTextWrapPos();
        return;
    }
    showProduct(a, *sel);
}

template <int F> void list(App& a) {
    loadState();
    if (!live<F>(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const FygeoTelemetry& t = a.rx.fygeo;
    if (t.products.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no products yet"); ImGui::PopTextWrapPos(); return; }
    if (!ImGui::BeginTable("##fyl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) return;
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nvirtual channel %d, APID %d", p->name.c_str(), p->vcid, p->apid);
        ImGui::PopID();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", goes::kindName(p->kind));
        ImGui::TableNextColumn();
        if (p->w > 0 && h > 0) ImGui::TextDisabled("%d%%%s", rows * 100 / h, errors ? " !" : "");
        else ImGui::TextDisabled("%s", complete ? (errors ? "errors" : (bytes >= 10240 ? (std::to_string(bytes / 1024) + " kB").c_str() : (std::to_string(bytes) + " B").c_str())) : "...");
    }
    ImGui::EndTable();
}

template <int F> void receiver(App& a) {
    if (!live<F>(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const FygeoTelemetry& t = a.rx.fygeo;
    const int L = t.link;
    kvRow(a, "state", "%s", dect2::fygeoSummary(t).c_str());
    if (!t.hint.empty()) {
        ImGui::TextDisabled("Why nothing decodes yet"); kvColumn(130 * gUi);
        ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", t.hint.c_str()); ImGui::PopTextWrapPos();
    }
    kvRow(a, "satellite", "%s", kSat[L]);
    kvRow(a, "link", "%s, %.4g MHz, %s", dect2::fygeoLinkName(L), dect2::fygeoMhz(L), dect2::fygeoModcodName(L));
    kvRow(a, "carrier", "offset %+.2f kHz, %s, Es/N0 %.1f dB (ideal threshold %.1f dB)", t.cfoHz / 1e3, t.carrierLock ? "locked" : "searching", t.snrDb, dect2::fygeoIdealEsN0(L));
    if (dect2::fygeoIsS2(L)) {
        kvRow(a, "DVB-S2", "%s", t.signalInfo.c_str());
        kvRow(a, "lock stages", "spectrum %s, timing %s, carrier %s, frames %s, LDPC/BCH %s, TS %s", t.s2Spectrum ? "yes" : "no", t.s2Timing ? "yes" : "no", t.s2Carrier ? "yes" : "no",
              t.s2Frame ? "yes" : "no", t.s2Fec ? "yes" : "no", t.s2Ts ? "yes" : "no");
        kvRow(a, "quality", "MER %.1f dB, bit errors before the code %s", t.s2MerDb, t.s2PreFecBer >= 0 ? (std::to_string(t.s2PreFecBer)).c_str() : "-");
        kvRow(a, "frames", "%llu good, %llu failed", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        kvRow(a, "transport", "%llu TS packets, %llu CADUs%s", (unsigned long long)t.tsPackets, (unsigned long long)t.caduCount, t.pids.empty() ? "" : (", PID " + std::to_string(t.pids[0].first)).c_str());
        if (t.scid >= 0) kvRow(a, "VCDU", "spacecraft %d, %llu VCDUs, %llu fill, %llu gaps", t.scid, (unsigned long long)t.vcdus, (unsigned long long)t.fillVcdus, (unsigned long long)t.vcduGaps);
        kvRow(a, "files", "%llu, %llu with errors, %llu picture segments, %llu pictures complete, %llu encrypted", (unsigned long long)t.files, (unsigned long long)t.fileErrors,
              (unsigned long long)t.imageSegments, (unsigned long long)t.imagesDone, (unsigned long long)t.encryptedFiles);
    } else {
        kvRow(a, "symbol clock", "%.1f ksym/s (%+.0f ppm)", t.symbolRate / 1e3, (t.symbolRate / dect2::fygeoSymbolRate(L) - 1) * 1e6);
        kvRow(a, "frames", "%llu lines, %llu syncs (%llu refused), line counter %d, %d pictures", (unsigned long long)t.svFrames, (unsigned long long)t.svSyncs, (unsigned long long)t.svRejected, t.svLine, t.svImages);
        if (t.svPreambleBer >= 0) kvRow(a, "bit errors", "%.2e from the 10000 known preamble bits%s", t.svPreambleBer, t.svDataBer >= 0 ? (", " + std::to_string(t.svDataBer) + " from the zero fill of the frame").c_str() : "");
    }
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    if (!t.log.empty()) {
        ImGui::Spacing(); ImGui::TextDisabled("Log");
        ImGui::PushFont(a.mono, 0);
        for (size_t i = 0; i < t.log.size() && i < 20; i++) { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.log[i].c_str()); ImGui::PopTextWrapPos(); }
        ImGui::PopFont();
    }
}

template <int F> void panels(App& a) {
    std::vector<cf32> pts;
    if (live<F>(a)) pts = a.rx.fygeo.constellation;
    const float H = ImGui::GetContentRegionAvail().y;
    const float sz = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::BeginGroup();
    captionFit(sz, "Symbols");
    scatter("##fyc", pts, ImVec2(sz, sz), 2.0, pal::okGreen());
    ImGui::EndGroup();
}

template <int F> void status(App& a) {
    const bool run = a.engine.running(), on = live<F>(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const FygeoTelemetry& t = a.rx.fygeo;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(F < 2 ? "DVB-S2" : "Sync", !on ? 0 : t.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    lamp("Data", !on ? 0 : t.state >= 3 ? 1 : t.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::fygeoSummary(t).c_str()); ImGui::PopFont();
}

template <int F> void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = dect2::fygeoLinkName(linkNow<F>(a));
    if (live<F>(a)) l2 = dect2::fygeoSummary(a.rx.fygeo);
}

void galleryPublish(const FygeoTelemetry& t) {
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
        const std::string key = "fengyun-" + std::to_string((long long)time(nullptr)) + "-" + std::to_string(p->id);
        if (p->w > 0 && p->h > 0 && p->pix.size() >= (size_t)p->w * p->h) gallery::publishGray(key, "FengYun", (double)time(nullptr), p->w, p->h, p->pix.data(), m);
    }
}

template <int F> void tick(App& a) {
    loadState();
    if (F == 0 && !S.satApplied) { a.engine.fygeo().setLritSatellite(S.sat); a.tune.synth.modeOpt[0] = S.sat; S.satApplied = true; }
    const int L = linkNow<F>(a);
    if (S.appliedRate[F] != L * 1000000 + (int)(S.symKsym[L] * 10) + 1) {
        a.engine.fygeo().setSymbolRate((double)S.symKsym[L] * 1e3);
        S.appliedRate[F] = L * 1000000 + (int)(S.symKsym[L] * 10) + 1;
    }
    if (live<F>(a)) { autoSave(a.rx.fygeo); galleryPublish(a.rx.fygeo); }
}

template <int F> void tuner(App& a, bool& retune) {
    loadState();
    if (F == 0) {
        ImGui::TextDisabled("Satellite");
        sameLineIf(90 * gUi, 5 * gUi);
        ImGui::SetNextItemWidth(250 * gUi);
        const char* const sats[] = {"FY-4A (90 ksym/s, QPSK 2/5)", "FY-4B (120 ksym/s, QPSK 8/9)"};
        if (ImGui::Combo("##fysat", &S.sat, sats, 2)) {
            plat::prefs().setI("fygeoSat", S.sat); plat::prefs().flush();
            a.engine.fygeo().setLritSatellite(S.sat);
            a.tune.synth.modeOpt[0] = S.sat;       // the built-in test signal follows
            retune = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("FY-4A sends LRIT as DVB-S2 QPSK 2/5 at 90 ksym/s, FY-4B as QPSK 8/9 at 120 ksym/s (both around 1697 MHz). The symbol rate follows the choice.");
        flowNext(12 * gUi);
    }
    const int L = linkNow<F>(a);
    ImGui::TextDisabled("Frequency");
    sameLineIf(90 * gUi, 5 * gUi);
    char lab[32];
    snprintf(lab, sizeof lab, "%.4g MHz", dect2::fygeoMhz(L));
    if (ImGui::Button(lab)) { tuneFreq(a, dect2::fygeoMhz(L)); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kDish[L]);
    if (F < 2) {
        flowNext(8 * gUi);
        if (ImGui::Button(F == 1 ? "1697 MHz (LRIT)" : "1679 MHz (HRIT)")) { tuneFreq(a, F == 1 ? 1697.0 : 1679.0); retune = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("FengYun-4 sends LRIT around 1697 MHz and HRIT around 1679 MHz");
    }
    flowNext(12 * gUi);
    ImGui::TextDisabled("Symbol rate");
    sameLineIf(90 * gUi, 5 * gUi);
    ImGui::SetNextItemWidth(130 * gUi);
    char fmt[48];
    if (S.symKsym[L] > 0) snprintf(fmt, sizeof fmt, "%%.3f ksym/s");
    else snprintf(fmt, sizeof fmt, "auto (%.0f ksym/s)", dect2::fygeoSymbolRate(L) / 1e3);
    if (ImGui::InputFloat("##fysym", &S.symKsym[L], 0.f, 0.f, fmt)) {
        S.symKsym[L] = std::max(0.f, S.symKsym[L]);
        plat::prefs().setD(("fygeoSym" + std::to_string(L)).c_str(), S.symKsym[L]); plat::prefs().flush();
        a.engine.fygeo().setSymbolRate((double)S.symKsym[L] * 1e3); S.appliedRate[F] = L * 1000000 + (int)(S.symKsym[L] * 10) + 1;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = the link's own rate, found automatically. Type a rate in ksym/s only when a satellite changes it; there is no limit.");
    flowEnd();
}

template <int F> void meters(const App& a, std::vector<ModeMeter>& out) {
    const FygeoTelemetry& t = a.rx.fygeo;
    const int L = linkNow<F>(a);
    const double ideal = dect2::fygeoIdealEsN0(L);
    out.push_back({"Es/N0  dB", "%.1f", t.snrDb, -5, 20, t.snrDb >= ideal + 3 ? 1 : t.snrDb >= ideal ? 2 : 3});
    if (dect2::fygeoIsS2(L)) {
        const double fps = t.rateHist.empty() ? 0.0 : t.rateHist.back();
        out.push_back({"FRAMES/s", "%.0f", fps, 0, 60, t.state >= 3 ? 1 : 0});
    } else {
        out.push_back({"BER  %", "%.2f", std::max(0.0, t.svPreambleBer) * 100, 0, 25, t.svPreambleBer < 0 ? 0 : t.svPreambleBer < 0.01 ? 1 : t.svPreambleBer < 0.05 ? 2 : 3});
    }
}

// Simple view: the picture / products with their controls, and the signal and the product list beside them
template <int F> void command(App& a) {
    modepanel::panes({{"##fygcpic", nullptr, [&a] { tab<F>(a); }, 2.2f, 300.f},
                      {"##fygcsig", tr("Signal"), [&a] { receiver<F>(a); ImGui::Spacing(); list<F>(a); }, 1.f, 240.f}}, 340.f);
}

} // namespace

#define FY_UI(NAME, F) \
    extern const ModeUi NAME; \
    const ModeUi NAME = { \
        .sideTitle = "PRODUCTS", \
        .tabName = "Products", \
        .tabIcon = Ic::Camera, \
        .tab = tab<F>, \
        .receiver = receiver<F>, \
        .list = list<F>, \
        .panels = panels<F>, \
        .status = status<F>, \
        .summary = summary<F>, \
        .tuner = tuner<F>, \
        .tick = tick<F>, \
        .meters = meters<F>, \
        .command = command<F>, \
    }
FY_UI(kFy4LritUi, 0);
FY_UI(kFy4aHritUi, 1);
FY_UI(kFy2SvissrUi, 2);
