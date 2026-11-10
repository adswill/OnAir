#include "ease_ui.h"
#include "dect2/ease/ease.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/inmsat/inmsat_satid.h"
#include "i18n/i18n.h"
#include "dect2/ease/repeaters.h"
#include "dect2/engine/updater.h"
#include <atomic>
#include <thread>
#include <mutex>
#include "dect2/audio/audioout.h"
#include "dect2/ease/nit.h"
#include "platform/platform.h"
#include "codec/speech_codec_ui.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace easeui {

int preselectSat(App& a, const char* prefKey, const double* slotLon, int count, SatPick& st) {
    const ModeTuning* mt = modeTuning(a.family + 2);
    plat::Prefs& p = plat::prefs();
    const auto act = dect2::ease::preselectAction(p.has(prefKey), receiverLocationSet(), a.engine.running(), a.freqMhz, mt ? mt->defMhz : a.freqMhz);
    if (act == dect2::ease::Preselect::LeaveAlone) { p.setI(prefKey, 0); return -1; }
    if (act != dect2::ease::Preselect::Pick) return -1;
    double la, lo;
    receiverLocation(la, lo);
    const dect2::ease::GeoPick g = dect2::ease::highestGeo(la, lo, slotLon, count);
    if (g.index < 0) return -1;
    st.prevMhz = a.freqMhz; st.undoable = true;
    p.setI(prefKey, g.index + 1);
    savePrefs(a);
    return g.index;
}

bool satPointing(App& a, double slotLon, SatPick& st) {
    if (!receiverLocationSet()) return false;
    double la, lo, az = 0, el = 0;
    receiverLocation(la, lo);
    dect2::inmsat::geoLook(la, lo, slotLon, az, el);
    ImGui::PushTextWrapPos(0);
    if (el < 0) ImGui::TextDisabled("%s", tr("This satellite is below your horizon."));
    else ImGui::TextDisabled(tr("Point the antenna %s (%.0f deg), %.1f deg up."), tr(dect2::ease::compass8(az)), az, el);
    ImGui::PopTextWrapPos();
    bool undo = false;
    if (st.undoable) {
        ImGui::TextDisabled("%s", tr("Chosen from your position as the highest satellite."));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Undo##easesat"))) { undo = true; st.undoable = false; }
    }
    return undo;
}

bool autoBandControl(App& a, const char* prefKey, uint64_t decoded) {
    struct St { bool loaded = false, on = false; std::unique_ptr<dect2::ease::AutoBand> ab; double lastMhz = 0; std::string note; };
    static std::map<std::string, St> all;
    St& s = all[prefKey];
    if (!s.loaded) { s.loaded = true; s.on = plat::prefs().getB(prefKey, false); }
    if (ImGui::Checkbox(tr("Auto band"), &s.on)) {
        plat::prefs().setB(prefKey, s.on);
        s.ab.reset(); s.lastMhz = 0; s.note.clear();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Listens 30 s on each band used in your region, settles where devices are heard and keeps sampling the others. One radio can still miss a device that sends rarely. Tuning by hand turns it off."));
    if (!s.on) {
        if (!s.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", s.note.c_str()); }
        return true;
    }
    if (!s.ab) s.ab = std::make_unique<dect2::ease::AutoBand>(dect2::ease::ismBandsFor(plat::prefs().getS("locCountry", "")));
    if (!a.engine.running()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Starts when the receiver runs.")); return false; }
    if (s.lastMhz > 0 && std::fabs(a.freqMhz - s.lastMhz) > 0.0001) {   // someone tuned: that choice wins
        s.on = false; plat::prefs().setB(prefKey, false); s.ab.reset(); s.lastMhz = 0;
        s.note = tr("Auto band off: you tuned by hand.");
        return true;
    }
    const int idx = s.ab->update(ImGui::GetTime(), decoded);
    const double mhz = s.ab->bandMhz(idx);
    if (std::fabs(a.freqMhz - mhz) > 0.0001) tuneFreq(a, mhz);
    s.lastMhz = a.freqMhz;
    ImGui::SameLine();
    ImGui::TextDisabled(s.ab->settled() ? tr("On %.2f MHz, where decodes arrive.") : tr("Looking for devices, now on %.2f MHz."), mhz);
    return false;
}

namespace {
namespace fs = std::filesystem;
struct RepList { bool loaded = false; std::vector<dect2::ease::Repeater> rows; std::string msg; bool fromOr = false; };
RepList gRep;
std::string repPath() { return plat::dataDir() + "/repeaters/list.csv"; }
std::string orCachePath() { return plat::dataDir() + "/repeaters/openrepeater.json"; }   // the downloaded copy (CC0 data from openrepeater.org)
std::string orKeyPath() { return plat::dataDir() + "/repeaters/openrepeater.key"; }      // the user's own API key
std::string readAll(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}
bool writeAll(const std::string& path, const std::string& data) {
    std::error_code ec; fs::create_directories(fs::u8path(path).parent_path(), ec);
    std::ofstream o(fs::u8path(path), std::ios::binary | std::ios::trunc);
    o << data; return (bool)o;
}
// Open Repeater download on request: the user's own API key, 30 requests a day per key, so one request per click (up to 3 pages) and a cache
struct OrJob { std::atomic<bool> busy{false}; std::mutex mu; std::string result, err; bool done = false; std::thread th; };
OrJob gOr;
char gKey[96] = {};
bool gKeyLoaded = false;
std::string cachedStamp() {
    std::error_code ec; const auto t = fs::last_write_time(fs::u8path(orCachePath()), ec);
    if (ec) return "";
    const auto sc = std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t tt = std::chrono::system_clock::to_time_t(sc);
    char b[32]; std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&tt));
    return b;
}
void orStart(double lat, double lon, std::string key) {
    if (gOr.busy.exchange(true)) return;
    if (gOr.th.joinable()) gOr.th.join();
    { std::lock_guard<std::mutex> lk(gOr.mu); gOr.done = false; gOr.err.clear(); gOr.result.clear(); }
    gOr.th = std::thread([lat, lon, key] {
        std::string all = "[", err;
        const int limit = 100; int total = 0;
        std::atomic<bool> never{false};
        for (int page = 1; page <= 3; page++) {
            std::string body;
            const int rc = dect2::curlRun({"-sS", "-L", "--max-time", "30", "--max-filesize", "4000000", "--max-redirs", "3", "--proto", "=https", "--proto-redir", "=https",
                                           "-H", "X-API-Key: " + key, "-H", "Accept: application/json", "-w", "\n%{http_code}",
                                           dect2::ease::openRepeaterSearchUrl(lat, lon, 300, page, limit)}, &body, never);
            const size_t nl = body.rfind('\n');
            const std::string code = nl == std::string::npos ? "" : body.substr(nl + 1);
            if (nl != std::string::npos) body.resize(nl);
            if (rc != 0) { err = "could not reach openrepeater.org"; break; }
            if (code == "429") { err = "Open Repeater's daily limit for this key (30 requests) is used up; try again after midnight UTC"; break; }
            if (code == "401" || code == "403") { err = "Open Repeater did not accept the API key"; break; }
            int skipped = 0; std::string perr;
            const auto rows = dect2::ease::parseOpenRepeaterJson(body, skipped, perr);
            if (!perr.empty()) { err = "Open Repeater: " + perr; break; }
            // keep the rows as a plain array of the fields the list uses
            for (const auto& r : rows) {
                char b[64]; snprintf(b, sizeof b, "{\"frequency\":%.6f,\"lat\":%.5f,\"lng\":%.5f,", r.mhz, r.lat, r.lon);
                auto esc = [](const std::string& x) { std::string o; for (char c : x) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((unsigned char)c >= 32) o += c; } return o; };
                if (total++) all += ",";
                all += std::string(b) + "\"callsign\":\"" + esc(r.call) + "\",\"city\":\"" + esc(r.where) + "\",\"mode\":\"" + esc(r.mode) + "\"}";
            }
            if ((int)rows.size() + skipped < limit) break;
        }
        all += "]";
        std::lock_guard<std::mutex> lk(gOr.mu);
        if (err.empty() && total == 0) err = "Open Repeater listed no repeaters within 300 km";
        gOr.err = err; gOr.result = err.empty() ? all : std::string(); gOr.done = true;
        gOr.busy = false;
    });
}
bool orRead() {
    const std::string t = readAll(orCachePath());
    if (t.empty()) return false;
    int skipped = 0; std::string err;
    auto v = dect2::ease::parseOpenRepeaterJson(t, skipped, err);
    if (!err.empty() || v.empty()) return false;
    gRep.rows = std::move(v); gRep.fromOr = true;
    char b[128]; snprintf(b, sizeof b, tr("%d repeaters from Open Repeater, downloaded %s"), (int)gRep.rows.size(), cachedStamp().c_str());
    gRep.msg = b;
    return true;
}
bool repRead(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    std::stringstream ss; ss << in.rdbuf();
    int skipped = 0; std::string err;
    auto v = dect2::ease::parseRepeaterCsv(ss.str(), skipped, err);
    if (!err.empty()) { gRep.msg = err; return false; }
    gRep.rows = std::move(v); gRep.fromOr = false;
    char b[96]; snprintf(b, sizeof b, tr("%d repeaters, %d rows skipped"), (int)gRep.rows.size(), skipped);
    gRep.msg = b;
    return true;
}
}

void repeaterPicker(App& a, const char* mode) {
    if (!gRep.loaded) {   // the cached download or the imported CSV, whichever is newer
        gRep.loaded = true; std::error_code ec;
        const bool hasCsv = fs::exists(fs::u8path(repPath()), ec), hasOr = fs::exists(fs::u8path(orCachePath()), ec);
        const bool orNewer = hasOr && (!hasCsv || fs::last_write_time(fs::u8path(orCachePath()), ec) > fs::last_write_time(fs::u8path(repPath()), ec));
        if (orNewer) { if (!orRead() && hasCsv) repRead(repPath()); } else if (hasCsv) repRead(repPath());
    }
    if (!gKeyLoaded) { gKeyLoaded = true; std::string k = readAll(orKeyPath()); while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back(); snprintf(gKey, sizeof gKey, "%s", k.c_str()); }
    {   // a finished download: keep the copy and use it
        std::lock_guard<std::mutex> lk(gOr.mu);
        if (gOr.done) {
            gOr.done = false;
            if (!gOr.err.empty()) gRep.msg = gOr.err;
            else if (writeAll(orCachePath(), gOr.result)) orRead();
        }
    }
    const std::string pid = std::string("##rep") + mode;
    if (ImGui::SmallButton(tr("Repeaters near me"))) ImGui::OpenPopup(pid.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lists the repeaters closest to your position from a list you import. Clicking one tunes to it."));
    if (!ImGui::BeginPopup(pid.c_str())) return;
    if (ImGui::SmallButton(tr("Import list..."))) {
        const std::string p = plat::openFileDialog();
        if (!p.empty() && repRead(p)) {
            std::error_code ec;
            fs::create_directories(fs::u8path(plat::dataDir() + "/repeaters"), ec);
            fs::copy_file(fs::u8path(p), fs::u8path(repPath()), fs::copy_options::overwrite_existing, ec);
        }
    }
    ImGui::SameLine();
    {
        double la = 0, lo = 0; const bool have = receiverLocationSet(); if (have) receiverLocation(la, lo);
        const bool busy = gOr.busy.load();
        ImGui::BeginDisabled(busy || !have || !gKey[0]);
        if (ImGui::SmallButton(busy ? tr("Downloading...") : tr("Download from Open Repeater"))) { orStart(la, lo, gKey); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Downloads the repeaters within 300 km of your position from openrepeater.org (CC0 data) with your own API key (free account, 30 requests a day), and keeps a copy on this computer. Needs your position (View > Map) and the key below."));
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::InputTextWithHint("##orkey", tr("Open Repeater API key"), gKey, sizeof gKey, ImGuiInputTextFlags_Password)) writeAll(orKeyPath(), gKey);
        ImGui::TextDisabled("%s", tr("Repeater data: Open Repeater (openrepeater.org), CC0. Your key and the list stay on this computer; nothing is bundled."));
    }
    if (!gRep.msg.empty()) ImGui::TextDisabled("%s", gRep.msg.c_str());
    if (gRep.rows.empty()) {
        ImGui::PushTextWrapPos(380 * gUi);
        ImGui::TextDisabled("%s", tr("OnAir ships no repeater list. Download one from Open Repeater with your own free API key, or export a CSV from a repeater directory you use (columns for the output frequency and the position) and import it here. It stays on this computer."));
        ImGui::PopTextWrapPos();
    } else if (!receiverLocationSet()) {
        ImGui::TextDisabled("%s", tr("Set your position (View > Map) to see the ones near you."));
    } else {
        double la, lo; receiverLocation(la, lo);
        const auto nearRep = dect2::ease::nearestRepeaters(gRep.rows, mode, la, lo);
        if (nearRep.empty()) ImGui::TextDisabled("%s", tr("None within 300 km in this list."));
        else if (ImGui::BeginTable("##rept", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(420 * gUi, 260 * gUi))) {
            ImGui::TableSetupColumn(tr("Call")); ImGui::TableSetupColumn(tr("Place")); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("km"); ImGui::TableSetupColumn("CC");
            ImGui::TableHeadersRow();
            int id = 0;
            for (const auto& n : nearRep) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::PushID(id++);
                if (ImGui::Selectable((n.r->call.empty() ? "-" : n.r->call.c_str()), false, ImGuiSelectableFlags_SpanAllColumns)) { tuneFreq(a, n.r->mhz); ImGui::CloseCurrentPopup(); }
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n.r->where.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.4f", n.r->mhz);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", n.km);
                ImGui::TableNextColumn(); if (n.r->colourCode >= 0) ImGui::Text("%d", n.r->colourCode); else ImGui::TextUnformatted("-");
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndPopup();
}

bool nitTransponders(App& a, int kind, bool locked, bool inverted, int& pol) {
    pol = -1;
    const dect2::SiSnapshot* si = a.ts.si.get();
    if (!locked || !si) return false;
    const auto rows = dect2::ease::nitRows(*si, kind, a.ts.tsid, a.freqMhz, inverted);
    if (rows.size() < 2) return false;   // only itself: nothing to go to
    bool tuned = false;
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Transponders this network announces"));
    if (ImGui::BeginTable("##nitrows", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, std::min(200 * gUi, (rows.size() + 1.5f) * ImGui::GetFrameHeight())))) {
        ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn(tr("Pol")); ImGui::TableSetupColumn("kSym/s"); ImGui::TableSetupColumn(tr("Mod")); ImGui::TableSetupColumn("FEC"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        int id = 0;
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.3f", r.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(kind == 1 ? dect2::siPolName(r.pol) : "");
            ImGui::TableNextColumn(); ImGui::Text("%.0f", r.symbolRateK);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::siModName(r.kind, r.modulation));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.fec >= 0 ? dect2::siFecName(r.fec) : "");
            ImGui::TableNextColumn();
            ImGui::PushID(id++);
            if (r.current) ImGui::TextDisabled("%s", tr("this one"));
            else if (r.tuneMhz > 0 && ImGui::SmallButton(tr("Tune"))) { tuneFreq(a, r.tuneMhz); pol = r.pol; tuned = true; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return tuned;
}

namespace {
struct AlignSt { bool open = false, tone = false; dect2::ease::AlignMeter m; std::unique_ptr<dect2::AudioOut> out; double phase = 0; float amp = 0; int family = -1; };
AlignSt gAlign;

void alignStopTone() { if (gAlign.out) { gAlign.out->stop(); gAlign.out.reset(); } gAlign.amp = 0; }

void alignToneTick(App& a, bool wanted) {
    if (!wanted) { alignStopTone(); return; }
    if (!gAlign.out) {
        gAlign.out = std::make_unique<dect2::AudioOut>();
        if (!gAlign.out->start(48000)) { gAlign.out.reset(); gAlign.tone = false; return; }
        gAlign.out->setStartThreshold(48000 / 20);
    }
    gAlign.out->setVolume(a.volume);
    gAlign.out->setMuted(a.muted);
    int n = 48000 / 15 - gAlign.out->bufferedFrames();
    if (n < 96) return;
    if (n > 2048) n = 2048;
    float buf[2048 * 2];
    const float w = 2.f * 3.14159265f * (float)gAlign.m.toneHz() / 48000.f;
    for (int i = 0; i < n; i++) {
        gAlign.amp += 0.004f * (0.15f - gAlign.amp);
        const float v = gAlign.amp * std::sin((float)gAlign.phase);
        buf[2 * i] = buf[2 * i + 1] = v;
        gAlign.phase += w;
        if (gAlign.phase > 2 * 3.14159265) gAlign.phase -= 2 * 3.14159265;
    }
    gAlign.out->write(buf, n);
}
}

void alignButton(App& a, const char* id, float db, bool valid) {
    AlignSt& g = gAlign;
    if (g.family != a.family) { g.family = a.family; g.open = false; g.tone = false; g.m = dect2::ease::AlignMeter(); }
    if (ImGui::SmallButton(tr("Align"))) { g.open = !g.open; if (g.open) g.m.resetPeak(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A large level bar with peak hold, for pointing a dish."));
    if (!g.open) { g.tone = false; alignToneTick(a, false); return; }
    g.m.update(db, valid && a.engine.running());
    ImGui::SetNextWindowSize(ImVec2(380 * gUi, 0), ImGuiCond_Once);
    const std::string title = std::string(tr("Align the dish")) + "###align" + id;
    if (!ImGui::Begin(title.c_str(), &g.open, ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); alignToneTick(a, false); return; }
    char b[48]; snprintf(b, sizeof b, "%.1f dB", g.m.level());
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(valid ? b : tr("no signal yet"));
    ImGui::PopFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos(), size(340 * gUi, 44 * gUi);
    ImGui::Dummy(size);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(35, 38, 42, 255), 3 * gUi);
    dl->AddRectFilled(p, ImVec2(p.x + size.x * g.m.fraction(), p.y + size.y), IM_COL32(90, 200, 120, 255), 3 * gUi);
    if (g.m.hasPeak()) {
        const float x = p.x + size.x * g.m.peakFraction();
        dl->AddLine(ImVec2(x, p.y - 2), ImVec2(x, p.y + size.y + 2), IM_COL32(240, 220, 90, 255), 3 * gUi);
    }
    if (g.m.hasPeak()) { ImGui::TextDisabled(tr("Peak %.1f dB"), g.m.peak()); ImGui::SameLine(); }
    if (ImGui::SmallButton(tr("Reset peak"))) g.m.resetPeak();
    ImGui::Checkbox(tr("Sound tone"), &g.tone);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Rises with the level, so you can aim without looking at the screen. Off until ticked."));
    ImGui::TextDisabled("%s", tr("Move the dish in small steps and let the bar settle after each one."));
    ImGui::End();
    alignToneTick(a, g.tone && g.open && a.engine.running());
}

bool stationFillOnce(App& a, const char* doneKey, std::string& call, std::string& locator) {
    plat::Prefs& p = plat::basePrefs();
    if (p.getB(doneKey, false)) return false;
    double la = 0, lo = 0;
    const bool have = receiverLocationSet();
    if (have) receiverLocation(la, lo);
    if (!dect2::ease::stationFill(call, locator, {p.getS("ham.call", ""), a.pskr.call, a.wsprNet.call}, p.getS("ham.grid", ""), have, la, lo)) return false;
    if (!call.empty() && !locator.empty()) p.setB(doneKey, true);
    return true;
}

void codecNotice(App& a) {
    for (const ModeDef& m : allModes()) {
        if (!modeSelected(a, m)) continue;
        if (!m.name || !modeCodecMissing(m)) return;
        const std::string key = std::string("codecNoteHidden.") + m.name;
        if (plat::prefs().getB(key.c_str(), false)) return;
        const std::string n = m.name;
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("The voice codec for this mode is not installed. Calls and data still decode, the sound stays off."));
        ImGui::PopTextWrapPos();
        const CodecHelp c = n == "NXDN" ? CodecHelp::NxdnAmbe : n.rfind("dPMR", 0) == 0 ? CodecHelp::DpmrAmbe : n == "D-STAR" ? CodecHelp::DstarAmbe : n.rfind("P25", 0) == 0 ? CodecHelp::P25Imbe : CodecHelp::DmrAmbe;
        if (n != "TETRA") { codecHelpButton(c, tr("How to install")); ImGui::SameLine(); }
        appcodec::discordCodecButton(true); ImGui::SameLine();
        if (ImGui::SmallButton(tr("Hide##codecnote"))) plat::prefs().setB(key.c_str(), true);
        return;
    }
}

}
