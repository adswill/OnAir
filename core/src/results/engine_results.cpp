// Decoded results that outlive mode switches, retunes, restarts and the program itself (engine.h: clearResults ... refreshIdle).
// Every receiver keeps its lists through reset() and configure(); here they are cleared on the user's Clear, and saved to and loaded
// from one file per receiver (results_io.h format: a file of another build fails its signature check and is discarded).
#include "dect2/common/thread_name.h"
#include "dect2/engine/engine.h"
#include "dect2/results/results_io.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace dect2 {

namespace {

std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str())); }

bool readFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// a new file next to the old one, then renamed over it: a crash in the middle leaves the old file whole
bool writeFile(const std::filesystem::path& p, const std::string& data) {
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

} // namespace

#define DECT2_RESULT_SLOT(nm, a, rx) ResultSlot{nm, a, [this] { return rx.saveResults(); }, [this](const std::string& b) { return rx.loadResults(b); }, [this] { rx.clearResults(); }}

// One entry per receiver that keeps results: its file name and its active standard (activeStandard(), Engine::resetReceiver)
std::vector<Engine::ResultSlot> Engine::resultSlots() {
    return {
        DECT2_RESULT_SLOT("dmr", 10, rxDmr_),
        DECT2_RESULT_SLOT("adsb", 12, rxAdsb_),
        DECT2_RESULT_SLOT("sonde", 14, rxSonde_),
        DECT2_RESULT_SLOT("ais", 15, rxAis_),
        DECT2_RESULT_SLOT("marine", 16, rxMarine_),
        DECT2_RESULT_SLOT("acars", 17, rxAcars_),
        DECT2_RESULT_SLOT("inmarsat-c", 18, rxInmc_),
        DECT2_RESULT_SLOT("aero", 19, rxAero_),
        DECT2_RESULT_SLOT("iridium", 20, rxIridium_),
        DECT2_RESULT_SLOT("mesh", 21, rxMesh_),
        DECT2_RESULT_SLOT("pager", 24, rxPager_),
        DECT2_RESULT_SLOT("packet", 25, rxPacket_),
        DECT2_RESULT_SLOT("hfdig", 26, rxHfdig_),
        DECT2_RESULT_SLOT("airband", 27, rxAirband_),
        DECT2_RESULT_SLOT("vdl2", 28, rxVdl2_),
        DECT2_RESULT_SLOT("vdl4", 83, rxVdl4_),
        DECT2_RESULT_SLOT("ism", 29, rxIsm_),
        DECT2_RESULT_SLOT("ndb", 30, rxNdb_),
        DECT2_RESULT_SLOT("m17", 32, rxM17_),
        DECT2_RESULT_SLOT("cell", 33, rxCell_),
        DECT2_RESULT_SLOT("dstar", 34, rxDstar_),
        DECT2_RESULT_SLOT("cw", 35, rxCw_),
        DECT2_RESULT_SLOT("hfdl", 36, rxHfdl_),
        DECT2_RESULT_SLOT("psk", 37, rxPsk_),
        DECT2_RESULT_SLOT("js8", 38, rxJs8_),
        DECT2_RESULT_SLOT("nxdn", 39, rxNxdn_),
        DECT2_RESULT_SLOT("gsmr", 40, rxGsmr_),
        DECT2_RESULT_SLOT("uplink", 44, rxUplink_),
        DECT2_RESULT_SLOT("orbcomm", 45, rxOrb_),
        DECT2_RESULT_SLOT("dpmr", 46, rxDpmr_),
        DECT2_RESULT_SLOT("tetra", 47, rxTetra_),
        DECT2_RESULT_SLOT("ale", 50, rxAle_),
        DECT2_RESULT_SLOT("p25", 51, rxP25_),
        DECT2_RESULT_SLOT("ble", 52, rxBle_),
        DECT2_RESULT_SLOT("iono", 53, rxIono_),
        DECT2_RESULT_SLOT("wifi", 55, rxWifi_),
        DECT2_RESULT_SLOT("rid", 57, rxRid_),
        DECT2_RESULT_SLOT("nr", 58, rxNr_),
        DECT2_RESULT_SLOT("time", 59, rxTime_),
        DECT2_RESULT_SLOT("meteor", 60, rxMeteor_),
        DECT2_RESULT_SLOT("goes", 61, rxGoes_),
        DECT2_RESULT_SLOT("cubesat", 62, rxCube_),
        DECT2_RESULT_SLOT("globalstar", 63, rxGs_),
        DECT2_RESULT_SLOT("vdes", 64, rxVdes_),
        DECT2_RESULT_SLOT("hrpt", 65, rxHrpt_),
        DECT2_RESULT_SLOT("zigbee", 66, rxZb_),
        DECT2_RESULT_SLOT("flarm", 67, rxFlarm_),
        DECT2_RESULT_SLOT("hfbeacon", 68, rxHfb_),
        DECT2_RESULT_SLOT("dji", 69, rxDji_),
        DECT2_RESULT_SLOT("nbiot", 70, rxNb_),
        DECT2_RESULT_SLOT("halow", 71, rxHl_),
        DECT2_RESULT_SLOT("codar", 73, rxCd_),
        DECT2_RESULT_SLOT("multitone", 76, rxMt_),
        DECT2_RESULT_SLOT("same", 77, rxSame_),
        DECT2_RESULT_SLOT("ant", 78, rxAnt_),
        DECT2_RESULT_SLOT("tetrapol", 80, rxTetrapol_),
        DECT2_RESULT_SLOT("elektro", 81, rxEl_),
        DECT2_RESULT_SLOT("lbj", 82, rxLbj_),
        DECT2_RESULT_SLOT("klw", 147, rxKlw_),
        DECT2_RESULT_SLOT("tdcs", 148, rxTdcs_),
        DECT2_RESULT_SLOT("uat", 84, rxUat_),
        DECT2_RESULT_SLOT("loraraw", 72, rxLr_),
        DECT2_RESULT_SLOT("wisun", 85, rxWs_),
    };
}

#undef DECT2_RESULT_SLOT

std::vector<std::string> Engine::resultNames() const {
    std::vector<std::string> r;
    for (const ResultSlot& s : const_cast<Engine*>(this)->resultSlots()) r.push_back(s.name);
    return r;
}

void Engine::clearAllResults() {
    for (const ResultSlot& s : resultSlots()) s.clear();
    wide_.clearResults();
    refreshIdle();
}

void Engine::clearResults(int stdMode) {
    const int a = stdMode >= 2 ? stdMode - 1 : 0;
    for (const ResultSlot& s : resultSlots()) if (s.act == a) s.clear();
    if (WideModes::supports(a)) wide_.clearResults();
    refreshIdle();
}

int Engine::loadResults(const std::string& dir) {
    if (dir.empty()) return 0;
    std::lock_guard<std::mutex> lk(resIoMu_);
    int n = 0;
    for (const ResultSlot& s : resultSlots()) {
        const std::filesystem::path p = u8path(dir) / (std::string(s.name) + ".bin");
        std::string blob;
        if (!readFile(p, blob)) continue;
        if (s.load(blob)) { n++; resHash_[s.name] = results::detail::fnv1a(blob); continue; }
        std::error_code ec;
        std::filesystem::remove(p, ec);
        log(std::string("results: the saved ") + s.name + " list is from another version and was discarded");
    }
    refreshIdle();
    return n;
}

int Engine::saveResults(const std::string& dir) {
    if (dir.empty()) return 0;
    std::lock_guard<std::mutex> lk(resIoMu_);
    std::error_code ec;
    std::filesystem::create_directories(u8path(dir), ec);
    int n = 0;
    for (const ResultSlot& s : resultSlots()) {
        const std::string blob = s.save();
        const uint64_t h = results::detail::fnv1a(blob);
        auto it = resHash_.find(s.name);
        if (it != resHash_.end() && it->second == h) continue;
        if (!writeFile(u8path(dir) / (std::string(s.name) + ".bin"), blob)) continue;
        resHash_[s.name] = h;
        n++;
    }
    return n;
}

void Engine::stopResultsSaver() {
    {
        std::lock_guard<std::mutex> lk(resMu_);
        resStop_ = true;
    }
    resCv_.notify_all();
    if (resTh_.joinable()) resTh_.join();
    std::lock_guard<std::mutex> lk(resMu_);
    resStop_ = false;
}

void Engine::setResultsDir(const std::string& dir) {
    stopResultsSaver();
    {
        std::lock_guard<std::mutex> lk(resMu_);
        resDir_ = dir;
    }
    if (dir.empty()) return;
    loadResults(dir);
    resTh_ = dect2::namedThread("onair-results", [this, dir] {
        std::unique_lock<std::mutex> lk(resMu_);
        while (!resStop_) {
            resCv_.wait_for(lk, std::chrono::seconds(30), [this] { return resStop_; });
            if (resStop_) break;
            lk.unlock();
            saveResults(dir);
            lk.lock();
        }
    });
}

void Engine::flushResults() {
    stopResultsSaver();
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(resMu_);
        dir = resDir_;
    }
    saveResults(dir);
}

void Engine::refreshIdle() {
    if (running_) return;
    const int m = stdMode_.load();
    if (m < 2) return;
    RxTelemetry t;
    uint64_t seqs[kPollSeqSlots] = {};
    if (pollMode(m - 1, t, seqs) != 1) return;
    std::lock_guard<std::mutex> lk(rxMu_);
    publishRx(std::move(t));
}

} // namespace dect2
