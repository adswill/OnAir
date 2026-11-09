#include "ease_ui.h"
#include "dect2/ease/ease.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/inmsat/inmsat_satid.h"
#include "i18n/i18n.h"
#include "dect2/ease/repeaters.h"
#include "platform/platform.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
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
struct RepList { bool loaded = false; std::vector<dect2::ease::Repeater> rows; std::string msg; };
RepList gRep;
std::string repPath() { return plat::dataDir() + "/repeaters/list.csv"; }
bool repRead(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    std::stringstream ss; ss << in.rdbuf();
    int skipped = 0; std::string err;
    auto v = dect2::ease::parseRepeaterCsv(ss.str(), skipped, err);
    if (!err.empty()) { gRep.msg = err; return false; }
    gRep.rows = std::move(v);
    char b[96]; snprintf(b, sizeof b, tr("%d repeaters, %d rows skipped"), (int)gRep.rows.size(), skipped);
    gRep.msg = b;
    return true;
}
}

void repeaterPicker(App& a, const char* mode) {
    if (!gRep.loaded) { gRep.loaded = true; std::error_code ec; if (fs::exists(fs::u8path(repPath()), ec)) repRead(repPath()); }
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
    if (!gRep.msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", gRep.msg.c_str()); }
    if (gRep.rows.empty()) {
        ImGui::PushTextWrapPos(380 * gUi);
        ImGui::TextDisabled("%s", tr("OnAir ships no repeater list. Export one as CSV from a repeater directory you use (columns for the output frequency and the position), and import it here. It stays on this computer."));
        ImGui::PopTextWrapPos();
    } else if (!receiverLocationSet()) {
        ImGui::TextDisabled("%s", tr("Set your position (View > Map) to see the ones near you."));
    } else {
        double la, lo; receiverLocation(la, lo);
        const auto near = dect2::ease::nearestRepeaters(gRep.rows, mode, la, lo);
        if (near.empty()) ImGui::TextDisabled("%s", tr("None within 300 km in this list."));
        else if (ImGui::BeginTable("##rept", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(420 * gUi, 260 * gUi))) {
            ImGui::TableSetupColumn(tr("Call")); ImGui::TableSetupColumn(tr("Place")); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("km"); ImGui::TableSetupColumn("CC");
            ImGui::TableHeadersRow();
            int id = 0;
            for (const auto& n : near) {
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
        if (ImGui::SmallButton(tr("Hide##codecnote"))) plat::prefs().setB(key.c_str(), true);
        return;
    }
}

}
