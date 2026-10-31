#include "voice/radioid_ui.h"
#include "app.h"
#include "dect2/common/thread_name.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "ui/ui_common.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using dect2::RadioIdKind;

namespace {

struct Slot {
    bool loaded = false;                 // the saved file was tried
    std::atomic<int> job{0};             // 0 idle, 1 downloading, 2 done, 3 failed
    std::thread th;
    std::mutex mu;
    std::string msg;
    std::string info;                    // what is loaded: file name, rows, skipped rows
    char path[512] = "";
};
Slot gSlot[2];
Slot& slotOf(RadioIdKind k) { return gSlot[k == RadioIdKind::Nxdn ? 1 : 0]; }
const char* nameOf(RadioIdKind k) { return k == RadioIdKind::Nxdn ? "nxdn" : "dmr"; }
std::string dirPath() { return plat::dataDir() + "/radioid"; }
std::string savedPath(RadioIdKind k) { return dirPath() + "/" + nameOf(k) + ".csv"; }

// parses the file, installs it when it holds ids; the message tells the user what happened
bool install(RadioIdKind k, const std::string& file, std::string& msg) {
    auto db = std::make_shared<dect2::RadioIdDb>();
    std::string err;
    if (!db->loadFile(file, err)) { msg = err; return false; }
    msg = std::to_string(db->size()) + " radio IDs" + (db->skipped() ? ", " + std::to_string(db->skipped()) + " rows skipped" : "");
    dect2::radioIdInstall(k, db);
    return true;
}

void startDownload(RadioIdKind k) {
    Slot& s = slotOf(k);
    if (s.job.load() == 1) return;
    if (s.th.joinable()) s.th.join();
    s.job = 1;
    s.th = dect2::namedThread("onair-radioid", [k, &s] {
        std::error_code ec;
        fs::create_directories(fs::u8path(dirPath()), ec);
        const std::string tmp = savedPath(k) + ".part";
        std::string msg;
        bool ok = plat::fetchUrl(dect2::radioIdUrl(k), tmp, "OnAir");
        if (!ok) msg = "the download did not work (offline, or the site is busy): download the file yourself (docs/RADIOID.md) and use Import";
        else {
            ok = install(k, tmp, msg);
            if (ok) { fs::rename(fs::u8path(tmp), fs::u8path(savedPath(k)), ec); if (ec) { msg += " (could not keep a copy for the next start)"; } }
        }
        fs::remove(fs::u8path(tmp), ec);
        { std::lock_guard<std::mutex> lk(s.mu); s.msg = msg; if (ok) s.info = msg; }
        s.job = ok ? 2 : 3;
    });
}

}  // namespace

namespace radioidui {

void tick(RadioIdKind k) {
    Slot& s = slotOf(k);
    if (!s.loaded) {
        s.loaded = true;
        std::error_code ec;
        const std::string p = savedPath(k);
        if (fs::exists(fs::u8path(p), ec)) { std::string m; if (install(k, p, m)) s.info = m; }
    }
    const int j = s.job.load();
    if (j == 2 || j == 3) { if (s.th.joinable()) s.th.join(); s.job = 0; }
}

std::string withName(RadioIdKind k, const std::string& idText, uint32_t id) {
    if (id == 0 || dect2::radioIdCount(k) == 0) return idText;
    const std::string n = dect2::radioIdLabel(k, id);
    return n.empty() ? idText : idText + "  " + n;
}

void section(RadioIdKind k) {
    Slot& s = slotOf(k);
    tick(k);
    const bool dmr = k == RadioIdKind::Dmr;
    ImGui::PushID(nameOf(k));
    if (ImGui::CollapsingHeader(trw("Names from a radio ID list"))) {
        const size_t n = dect2::radioIdCount(k);
        ImGui::PushTextWrapPos(0);
        if (n) ImGui::TextDisabled("%s: %s", tr("Loaded"), s.info.empty() ? (std::to_string(n) + " radio IDs").c_str() : s.info.c_str());
        else ImGui::TextDisabled("%s", dmr ? tr("No list loaded. Calls show the radio ID only. The public list is the DMR user database of radioid.net (amateur callsign and name per ID).")
                                            : tr("No list loaded. Calls show the unit ID only. The public list is the NXDN user database of radioid.net."));
        ImGui::PopTextWrapPos();
        const bool busy = s.job.load() == 1;
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(busy ? tr("Downloading ...##rid") : tr("Download##rid"))) startDownload(k);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", tr("Downloads the list when you click, never by itself. It is kept in the data folder and read at the next start. OnAir does not ship the list."), dect2::radioIdUrl(k));
        flowNext(6 * gUi);
        if (ImGui::Button(tr("Import file ...##rid"))) {
            const std::string p = openFileDialog();
            if (!p.empty()) {
                std::string msg;
                if (install(k, p, msg)) {
                    std::error_code ec;
                    fs::create_directories(fs::u8path(dirPath()), ec);
                    fs::copy_file(fs::u8path(p), fs::u8path(savedPath(k)), fs::copy_options::overwrite_existing, ec);
                    s.info = msg;
                }
                std::lock_guard<std::mutex> lk(s.mu); s.msg = msg;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A CSV with a radio ID column and callsign / name columns (radioid.net user.csv, or your own list)."));
        flowNext(6 * gUi);
        ImGui::BeginDisabled(n == 0);
        if (ImGui::Button(tr("Unload##rid"))) {
            dect2::radioIdInstall(k, nullptr);
            std::error_code ec; fs::remove(fs::u8path(savedPath(k)), ec);
            s.info.clear();
        }
        ImGui::EndDisabled();
        { std::lock_guard<std::mutex> lk(s.mu); if (!s.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", s.msg.c_str()); ImGui::PopTextWrapPos(); } }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Names are shown on this screen only and are not written to recordings or session files."));
        ImGui::PopTextWrapPos();
    }
    ImGui::PopID();
}

}  // namespace radioidui
