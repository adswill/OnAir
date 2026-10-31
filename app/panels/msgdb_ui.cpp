// The Messages window (View > Messages): the unified database of decoded messages (core/src/msgdb) with search, filters, a timeline, export and the
// retention settings. The database is fed by a tap on the automation event bus; voice and encrypted content never reaches it.
#include "dect2/common/time_fmt.h"
#include "../app.h"
#include "dect2/common/u8file.h"
#include "dect2/automation/engine.h"
#include "dect2/msgdb/msgdb.h"
#include "dect2/export/exporters.h"
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
namespace mdb = dect2::msgdb;
namespace au = dect2::automation;
namespace fs = std::filesystem;

struct St {
    bool started = false;
    bool record = true;
    int maxRowsK = 1000;     // thousand rows
    int maxMB = 512;
    int keepDays = 0;
    char text[160] = "";
    char ident[64] = "";
    int modeSel = 0, range = 0;
    bool posOnly = false, relevance = false;
    float fLoMHz = 0, fHiMHz = 0;   // frequency range filter in MHz (0 = open)
    char source[96] = "";        // source (recording) file filter
    int page = 0;
    std::vector<mdb::Record> rows;
    bool oneLine = true;   // no row has a line break: every row is as tall, so only the visible ones are drawn (ImGuiListClipper)
    std::vector<std::string> modes;
    int64_t matches = 0;
    mdb::Stats stats;
    double lastQ = 0;
    bool dirty = true;
    int sel = -1;
    bool clearAsk = false;
    std::string note;
    std::string err;
};
St S;
constexpr int kPage = 300;

std::string dir() { return plat::dataDir() + "/messages"; }

void saveSettings() {
    std::error_code ec;
    fs::create_directories(fs::path(reinterpret_cast<const char8_t*>(dir().c_str())), ec);
    std::ofstream o(dect2::u8file(dir() + "/settings.txt"));
    o << "record=" << S.record << "\nmax_rows_k=" << S.maxRowsK << "\nmax_mb=" << S.maxMB << "\nkeep_days=" << S.keepDays << "\n";
}

void loadSettings() {
    std::ifstream f(dect2::u8file(dir() + "/settings.txt"));
    std::string l;
    while (std::getline(f, l)) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq);
        const int v = atoi(l.c_str() + eq + 1);
        if (k == "record") S.record = v != 0;
        else if (k == "max_rows_k") S.maxRowsK = std::max(0, std::min(v, 100000));
        else if (k == "max_mb") S.maxMB = std::max(0, std::min(v, 100000));
        else if (k == "keep_days") S.keepDays = std::max(0, std::min(v, 36500));
    }
}

void applySettings() {
    mdb::Limits l;
    l.maxRows = (int64_t)S.maxRowsK * 1000;
    l.maxBytes = (int64_t)S.maxMB << 20;
    l.maxAgeDays = S.keepDays;
    mdb::global().setLimits(l);
    const au::FamilyMask mask = au::familyBit(au::FAdsb) | au::familyBit(au::FAcars) | au::familyBit(au::FVdl2) | au::familyBit(au::FHfdl) | au::familyBit(au::FPager) | au::familyBit(au::FAis) | au::familyBit(au::FAprs) |
                                au::familyBit(au::FSonde) | au::familyBit(au::FLora) | au::familyBit(au::FBle) | au::familyBit(au::FRid) | au::familyBit(au::FDsc) | au::familyBit(au::FSession) | au::familyBit(au::FMsg);
    if (S.record) au::global().setTap([](const au::Event& e) { mdb::global().enqueue(e); }, mask);
    else au::global().setTap(nullptr, 0);
}

void start() {
    S.started = true;
    loadSettings();
    std::error_code ec;
    fs::create_directories(fs::path(reinterpret_cast<const char8_t*>(dir().c_str())), ec);
    if (!mdb::global().open(dir() + "/messages.db", &S.err)) return;
    applySettings();
    mdb::global().prune();
    mdb::global().start();
}

std::string when(double t) {
    return dect2::timefmt::show((int64_t)t, dect2::timefmt::Style::DateClock);
}

mdb::Filter filter() {
    mdb::Filter f;
    f.text = S.text;
    f.ident = S.ident;
    if (S.modeSel > 0 && S.modeSel <= (int)S.modes.size()) f.mode = S.modes[(size_t)S.modeSel - 1];
    static const double span[] = {0, 3600, 86400, 7 * 86400, 30 * 86400};
    if (S.range > 0) f.since = au::wallNow() - span[S.range];
    f.posOnly = S.posOnly;
    f.freqLo = S.fLoMHz * 1e6; f.freqHi = S.fHiMHz * 1e6;
    f.source = S.source;
    f.relevance = S.relevance;
    f.limit = kPage;
    f.offset = S.page * kPage;
    return f;
}

void refresh() {
    mdb::Db& db = mdb::global();
    const mdb::Filter f = filter();
    S.rows = db.query(f);
    S.oneLine = true;
    for (const mdb::Record& r : S.rows) if (r.text.find('\n') != std::string::npos || r.mode.find('\n') != std::string::npos || r.ident.find('\n') != std::string::npos) { S.oneLine = false; break; }
    S.matches = db.count(f);
    S.stats = db.stats();
    S.modes = db.modes();
    if (S.modeSel > (int)S.modes.size()) S.modeSel = 0;
    S.sel = -1;
    S.dirty = false;
}

void exportButton(const char* label, const char* fmt, const char* ext) {
    if (!ImGui::Button(label)) return;
    const std::string nm = std::string("messages-") + std::to_string((long long)au::wallNow()) + ext;
    const std::string path = saveFileDialog(nm.c_str());
    if (path.empty()) return;
    mdb::Filter f = filter();
    f.oldestFirst = true;
    std::ofstream o(dect2::u8file(path), std::ios::binary);
    o << mdb::global().exportAs(f, fmt);
    o.flush();
    S.note = o ? std::string(tr("saved")) + " " + path : std::string(tr("could not write")) + " " + path;
}

// positions / text log through the shared exporters (core/src/export): fmt is a dect2::xport::Format
void exportGeoButton(const char* label, dect2::xport::Format fmt) {
    if (!ImGui::Button(label)) return;
    namespace xp = dect2::xport;
    mdb::Filter f = filter();
    f.oldestFirst = true; f.posOnly = fmt != xp::Format::Txt; f.limit = 100000; f.offset = 0;
    const std::vector<mdb::Record> rows = mdb::global().query(f);
    std::string data;
    if (fmt == xp::Format::Txt) {
        xp::Table t;
        t.cols = {"mode", "identity", "MHz", "message"};
        for (const mdb::Record& r : rows) t.add({r.mode, r.ident, r.freqHz > 0 ? xp::numText(r.freqHz / 1e6, 6) : "", r.text}, r.time);
        data = xp::toText(t);
    } else {
        std::vector<xp::GeoPoint> pts;
        for (const mdb::Record& r : rows) {
            xp::GeoPoint g; g.name = r.ident.empty() ? r.mode : r.ident; g.desc = r.mode + (r.text.empty() ? "" : ": " + r.text.substr(0, 120));
            g.lat = r.lat; g.lon = r.lon; g.time = r.time;
            pts.push_back(g);
        }
        data = fmt == xp::Format::Kml ? xp::toKml(pts, {}, "OnAir messages") : fmt == xp::Format::Gpx ? xp::toGpx(pts, {}, "OnAir messages") : xp::toGeoJson(pts);
    }
    const std::string path = saveFileDialog((std::string("messages-") + std::to_string((long long)au::wallNow()) + xp::formatExt(fmt)).c_str());
    if (path.empty()) return;
    std::ofstream o(dect2::u8file(path), std::ios::binary);
    o << data;
    o.flush();
    S.note = o ? std::string(tr("saved")) + " " + path : std::string(tr("could not write")) + " " + path;
}

void windowPanel(App& a) {
    firstWindowSize(980 * gUi, 620 * gUi);
    if (!ImGui::Begin(trw("Messages"), &a.msgdbOpen)) { ImGui::End(); return; }
    mdb::Db& db = mdb::global();
    if (!db.isOpen()) {
        ImGui::TextWrapped("%s %s", tr("The message database could not be opened:"), S.err.c_str());
        ImGui::End();
        return;
    }
    bool ch = false;
    ImGui::SetNextItemWidth(260 * gUi);
    ch |= ImGui::InputTextWithHint("##q", tr("Search text (words, prefix*)"), S.text, sizeof S.text);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    ch |= ImGui::InputTextWithHint("##id", tr("Identity"), S.ident, sizeof S.ident);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::BeginCombo("##mode", S.modeSel > 0 && S.modeSel <= (int)S.modes.size() ? S.modes[(size_t)S.modeSel - 1].c_str() : tr("All modes"))) {
        if (ImGui::Selectable(tr("All modes"), S.modeSel == 0)) { S.modeSel = 0; ch = true; }
        for (size_t i = 0; i < S.modes.size(); i++) if (ImGui::Selectable(S.modes[i].c_str(), S.modeSel == (int)i + 1)) { S.modeSel = (int)i + 1; ch = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    {
        const char* ranges[] = {tr("All time"), tr("Last hour"), tr("Last 24 hours"), tr("Last 7 days"), tr("Last 30 days")};
        ch |= ImGui::Combo("##range", &S.range, ranges, IM_ARRAYSIZE(ranges));
    }
    ImGui::SameLine();
    ch |= ImGui::Checkbox(tr("With position"), &S.posOnly);
    ImGui::SetNextItemWidth(90 * gUi); ch |= ImGui::InputFloat("##flo", &S.fLoMHz, 0, 0, "%.4f");
    ImGui::SameLine(); ImGui::TextUnformatted("-"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi); ch |= ImGui::InputFloat("##fhi", &S.fHiMHz, 0, 0, "%.4f");
    ImGui::SameLine(); ImGui::TextDisabled("%s", tr("MHz (0 = open)"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * gUi);
    ch |= ImGui::InputTextWithHint("##src", tr("Source file"), S.source, sizeof S.source);
    ImGui::SameLine();
    ch |= ImGui::Checkbox(tr("Best match first"), &S.relevance);
    if (ch) { S.page = 0; S.dirty = true; }
    if (S.dirty || ImGui::GetTime() - S.lastQ > 3.0) { S.lastQ = ImGui::GetTime(); refresh(); }

    ImGui::TextDisabled("%lld %s, %lld %s, %.1f MB", (long long)S.matches, tr("matching"), (long long)S.stats.rows, tr("stored"), (double)S.stats.bytes / 1048576.0);
    // a SameLine only before a button that is drawn: otherwise the table below would start on this line
    if (S.page > 0) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Newer"))) { S.page--; S.dirty = true; } }
    if ((S.page + 1) * kPage < S.matches) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Older"))) { S.page++; S.dirty = true; } }

    const float h = ImGui::GetContentRegionAvail().y - 136 * gUi;
    if (ImGui::BeginTable("msgs", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(120.f * gUi, h)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time (UTC)"), ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
        ImGui::TableSetupColumn(tr("Mode"), ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableSetupColumn(tr("Identity"), ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableSetupColumn(tr("MHz"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn(tr("Message"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        auto drawRow = [](size_t i) {
            const mdb::Record& r = S.rows[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char id[32];
            snprintf(id, sizeof id, "%s##%lld", when(r.time).c_str(), (long long)r.id);
            if (ImGui::Selectable(id, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.mode.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.ident.c_str());
            ImGui::TableNextColumn(); if (r.freqHz > 0) ImGui::Text("%.4f", r.freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.text.c_str());
        };
        if (S.oneLine) {   // a page of 300: only the rows in view are built (each one formats its time)
            ImGuiListClipper clip;
            clip.Begin((int)S.rows.size());
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) drawRow((size_t)i);
        } else
            for (size_t i = 0; i < S.rows.size(); i++) drawRow(i);
        ImGui::EndTable();
    }
    if (S.sel >= 0 && S.sel < (int)S.rows.size()) {
        const mdb::Record& r = S.rows[(size_t)S.sel];
        ImGui::BeginChild("detail", ImVec2(0, 84 * gUi), true);
        std::string e = r.extra;
        for (char& c : e) if (c == '\n') c = ' ';
        ImGui::TextWrapped("%s  %s  %s", r.type.c_str(), r.hasPos ? (std::to_string(r.lat).substr(0, 8) + ", " + std::to_string(r.lon).substr(0, 8)).c_str() : "", e.c_str());
        if (!r.source.empty()) ImGui::TextWrapped("%s %s", tr("Recording:"), r.source.c_str());
        for (const auto& m : allModes()) {   // the mode screen of the message, when the program has one with that name
            std::string a1 = m.name, b1 = r.mode;
            for (char& c : a1) c = (char)tolower((unsigned char)c);
            for (char& c : b1) c = (char)tolower((unsigned char)c);
            if (a1 != b1) continue;
            if (ImGui::SmallButton(tr("Open mode screen"))) selectMode(a, m.family, m.subId);
            break;
        }
        ImGui::EndChild();
    }
    exportButton(tr("Export CSV"), "csv", ".csv");
    ImGui::SameLine();
    exportButton(tr("Export JSON"), "json", ".json");
    ImGui::SameLine();
    exportButton(tr("Export ADIF"), "adif", ".adi");
    ImGui::SameLine();
    exportGeoButton(tr("Export TXT"), dect2::xport::Format::Txt);
    ImGui::SameLine();
    exportGeoButton(tr("Export KML"), dect2::xport::Format::Kml);
    ImGui::SameLine();
    exportGeoButton(tr("Export GPX"), dect2::xport::Format::Gpx);
    ImGui::SameLine();
    exportGeoButton(tr("Export GeoJSON"), dect2::xport::Format::GeoJson);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", S.note.c_str());
    if (ImGui::CollapsingHeader(trw("Storage"))) {
        bool sc = false;
        sc |= ImGui::Checkbox(tr("Record decoded messages"), &S.record);
        ImGui::SetNextItemWidth(120 * gUi); sc |= ImGui::InputInt(tr("Most rows (thousands, 0 = no limit)"), &S.maxRowsK, 0, 0);
        ImGui::SetNextItemWidth(120 * gUi); sc |= ImGui::InputInt(tr("Size cap in MB (0 = none)"), &S.maxMB, 0, 0);
        ImGui::SetNextItemWidth(120 * gUi); sc |= ImGui::InputInt(tr("Keep days (0 = forever)"), &S.keepDays, 0, 0);
        if (sc) {
            S.maxRowsK = std::max(0, S.maxRowsK); S.maxMB = std::max(0, S.maxMB); S.keepDays = std::max(0, S.keepDays);
            applySettings();
            saveSettings();
        }
        if (ImGui::Button(tr("Apply limits now"))) { db.prune(); S.dirty = true; }
        ImGui::SameLine();
        if (ImGui::Button(tr("Rebuild index"))) { db.rebuildIndex(); S.dirty = true; }
        ImGui::SameLine();
        if (!S.clearAsk) { if (ImGui::Button(tr("Clear all..."))) S.clearAsk = true; }
        else {
            if (ImGui::Button(tr("Really clear"))) { db.clear(); S.clearAsk = false; S.dirty = true; }
            ImGui::SameLine();
            if (ImGui::Button(tr("Keep"))) S.clearAsk = false;
        }
        ImGui::TextWrapped("%s %s/messages.db", tr("The database file:"), dir().c_str());
        ImGui::TextWrapped("%s", tr("Voice audio and encrypted traffic are never stored."));
    }
    ImGui::End();
}
}  // namespace

void msgdbSearchFor(App& a, const std::string& text) {
    if (!S.started) start();
    snprintf(S.text, sizeof S.text, "%s", text.c_str());
    S.ident[0] = 0; S.source[0] = 0; S.modeSel = 0; S.range = 0; S.posOnly = false; S.fLoMHz = S.fHiMHz = 0; S.relevance = true;
    S.page = 0; S.dirty = true;
    a.msgdbOpen = true;
}

std::vector<std::pair<std::string, std::string>> msgdbQuickSearch(const std::string& text, size_t n) {
    std::vector<std::pair<std::string, std::string>> out;   // {label, text to open the window with}
    if (!S.started) start();
    mdb::Db& db = mdb::global();
    if (!db.isOpen()) return out;
    mdb::Filter f;
    f.text = text; f.relevance = true; f.limit = (int)n;
    for (const mdb::Record& r : db.query(f)) {
        std::string t = r.text.substr(0, 60);
        for (char& c : t) if (c == '\n' || c == '\r') c = ' ';
        out.push_back({r.mode + " " + r.ident + (t.empty() ? "" : "  " + t), text});
    }
    return out;
}

void msgdbWindow(App& a) {
    if (a.rxId != 1) {   // one database for the program: RX 1's window
        if (App* f = receiverById(1)) { f->msgdbOpen = a.msgdbOpen; msgdbWindow(*f); a.msgdbOpen = f->msgdbOpen; }
        return;
    }
    if (!S.started) start();
    if (a.msgdbOpen) windowPanel(a);
}
