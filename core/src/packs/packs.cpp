// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/packs/packs.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include "dect2/geo/tilecache.h"

namespace dect2 {
namespace packs {

namespace fs = std::filesystem;

static std::vector<std::string> splitTabs(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == '\t') { v.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    v.push_back(cur);
    return v;
}

// a relative path that stays inside the root: no "..", no drive or leading slash
static bool safeRel(const std::string& r) {
    if (r.empty() || r[0] == '/' || r[0] == '\\' || r.find(':') != std::string::npos || r.find('\\') != std::string::npos) return false;
    size_t i = 0;
    while (i <= r.size()) {
        const size_t j = r.find('/', i);
        const std::string part = r.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (part.empty() || part == "..") return false;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return true;
}

void finishTilePack(Pack& p) {
    p.tiles = true;
    if (p.tileUrl.empty()) p.tileUrl = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
    if (p.tileDir.empty()) p.tileDir = "maps/osm";
    p.maxZoom = deepestZoomFitting(p.south, p.west, p.north, p.east, p.minZoom, p.maxZoom);
    const AreaPlan a = planArea(p.south, p.west, p.north, p.east, p.minZoom, p.maxZoom);
    p.tileCount = a.count;
    p.estBytes = a.tooMany ? 0 : a.estBytes;
}

std::vector<PackFile> tilePackFiles(const Pack& p) {
    std::vector<PackFile> out;
    const AreaPlan a = planArea(p.south, p.west, p.north, p.east, p.minZoom, p.maxZoom);
    for (const TileKey& k : a.tiles) {
        PackFile f;
        f.url = p.tileUrl;
        auto rep = [&](const char* tag, int v) {
            for (size_t pos; (pos = f.url.find(tag)) != std::string::npos;) f.url.replace(pos, strlen(tag), std::to_string(v));
        };
        rep("{z}", k.z); rep("{x}", k.x); rep("{y}", k.y);
        f.rel = p.tileDir + "/" + tileRelPath(k.z, k.x, k.y);
        out.push_back(std::move(f));
    }
    return out;
}

static Pack file1(const char* id, const char* title, const char* cat, const char* source, const char* lic, const char* attr, uint64_t est, const char* url, const char* rel,
                  Root root = Root::Data) {
    Pack p;
    p.id = id; p.title = title; p.category = cat; p.source = source; p.licence = lic; p.attribution = attr; p.estBytes = est; p.root = root;
    PackFile f; f.url = url; f.rel = rel;
    p.files.push_back(f);
    return p;
}

std::vector<Pack> builtinCatalog() {
    std::vector<Pack> v;
    const char* osm = "OpenStreetMap tile servers";
    const char* osmLic = "ODbL 1.0 (data), tile usage policy: small areas only";
    const char* osmAttr = "(c) OpenStreetMap contributors";
    struct R { const char* id; const char* name; double s, w, n, e; int z0, z1; };
    const R regions[] = {
        {"tiles-world", "World overview", -85, -180, 85, 180, 0, 5},
        {"tiles-europe", "Europe", 34, -12, 72, 40, 0, 6},
        {"tiles-lt", "Lithuania", 53.85, 20.9, 56.5, 26.9, 0, 10},
        {"tiles-lv", "Latvia", 55.6, 20.9, 58.1, 28.3, 0, 10},
        {"tiles-ee", "Estonia", 57.5, 21.7, 59.8, 28.3, 0, 10},
        {"tiles-pl", "Poland", 48.9, 14.1, 55.0, 24.2, 0, 9},
        {"tiles-de", "Germany", 47.2, 5.8, 55.1, 15.1, 0, 9},
        {"tiles-uk", "United Kingdom and Ireland", 49.8, -10.7, 61.0, 2.0, 0, 9},
        {"tiles-uae", "United Arab Emirates", 22.4, 51.5, 26.2, 56.5, 0, 10},
    };
    for (const R& r : regions) {
        Pack p;
        p.id = r.id; p.title = std::string("Map tiles: ") + r.name; p.category = "Map tiles"; p.source = osm; p.licence = osmLic; p.attribution = osmAttr;
        p.south = r.s; p.west = r.w; p.north = r.n; p.east = r.e; p.minZoom = r.z0; p.maxZoom = r.z1;
        finishTilePack(p);
        v.push_back(std::move(p));
    }
    const char* ck = "CelesTrak (celestrak.org)";
    const char* ckL = "Public data, free use with credit";
    const char* ckA = "T.S. Kelso, CelesTrak";
    struct T { const char* id; const char* name; const char* group; const char* file; uint64_t est; };
    const T tles[] = {
        {"tle-weather", "Weather satellites", "weather", "weather.txt", 20 << 10}, {"tle-stations", "Space stations", "stations", "stations.txt", 6 << 10},
        {"tle-orbcomm", "Orbcomm", "orbcomm", "orbcomm.txt", 8 << 10}, {"tle-amateur", "Amateur radio satellites", "amateur", "amateur.txt", 30 << 10},
        {"tle-globalstar", "Globalstar", "globalstar", "globalstar.txt", 8 << 10},
    };
    for (const T& t : tles)
        v.push_back(file1(t.id, (std::string("Satellite elements: ") + t.name).c_str(), "Satellites", ck, ckL, ckA, t.est,
                          (std::string("https://celestrak.org/NORAD/elements/gp.php?GROUP=") + t.group + "&FORMAT=tle").c_str(), (std::string("tle/") + t.file).c_str()));
    {
        Pack p;
        p.id = "airports"; p.title = "Airports and their frequencies"; p.category = "Airports"; p.source = "OurAirports (davidmegginson.github.io/ourairports-data)";
        p.licence = "Public domain"; p.attribution = "David Megginson and contributors, OurAirports"; p.estBytes = 14u << 20; p.post = "airports";
        PackFile a, f;
        a.url = "https://davidmegginson.github.io/ourairports-data/airports.csv"; a.rel = "packs/airports/airports.csv";
        f.url = "https://davidmegginson.github.io/ourairports-data/airport-frequencies.csv"; f.rel = "packs/airports/airport-frequencies.csv";
        p.files = {a, f};
        v.push_back(std::move(p));
    }
    for (const char* cc : {"DE", "GB", "FR", "PL", "NL", "LT", "AT", "CH"}) {
        std::string id = std::string("dab-tii-") + cc;   // pack and dataset ids are lower case (datasets::validDatasetId); the file keeps the country's capitals
        for (char& c : id) c = (char)std::tolower((unsigned char)c);
        v.push_back(file1(id.c_str(), (std::string("DAB transmitter list (TII): ") + cc).c_str(), "Transmitters", "OnAir-channels (github.com/adswill/OnAir-channels)",
                          "CC0 (public domain)", "OnAir-channels contributors", 40 << 10,
                          (std::string("https://raw.githubusercontent.com/adswill/OnAir-channels/main/dab-tii/") + cc + ".csv").c_str(), (std::string("dab-tii-") + cc + ".csv").c_str(), Root::Cache));
    }
    return v;
}

std::vector<Pack> parseCatalogTsv(const std::string& text, std::string* err) {
    std::vector<Pack> v;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        n++;
        if (line.empty() || line[0] == '#') continue;
        const auto c = splitTabs(line);
        if (c.size() < 8) { if (err) *err = "line " + std::to_string(n) + ": needs 8 or more tab separated fields"; continue; }
        if (!safeRel(c[7])) { if (err) *err = "line " + std::to_string(n) + ": the path must stay inside the data folder"; continue; }
        if (c[6].compare(0, 8, "https://") != 0 && c[6].compare(0, 7, "file://") != 0) { if (err) *err = "line " + std::to_string(n) + ": the address must be https"; continue; }
        PackFile f;
        f.url = c[6]; f.rel = c[7];
        if (c.size() > 8 && !c[8].empty()) f.bytes = strtoull(c[8].c_str(), nullptr, 10);
        if (c.size() > 9) f.sha256 = c[9];
        auto it = std::find_if(v.begin(), v.end(), [&](const Pack& p) { return p.id == c[0]; });
        if (it == v.end()) {
            Pack p;
            p.id = c[0]; p.title = c[1]; p.category = c[2]; p.source = c[3]; p.licence = c[4]; p.attribution = c[5];
            v.push_back(std::move(p));
            it = v.end() - 1;
        }
        it->files.push_back(f);
        it->estBytes += f.bytes;
    }
    return v;
}

Fetcher curlFetcher() {
    return [](const std::string& url, const std::string& out, uint64_t maxBytes, const std::atomic<bool>& cancel, const std::function<void()>& tick) {
        return curlDownloadFile(url, out, maxBytes, 3 * 3600, cancel, tick);
    };
}

PackManager::PackManager(std::string d, std::string c) : data_(std::move(d)), cache_(std::move(c)), fetch_(curlFetcher()) {}
PackManager::~PackManager() { cancel_ = true; if (th_.joinable()) th_.join(); }

void PackManager::setCatalog(std::vector<Pack> c) { cat_ = std::move(c); }
const Pack* PackManager::find(const std::string& id) const {
    for (const Pack& p : cat_) if (p.id == id) return &p;
    return nullptr;
}

std::string PackManager::activeId() const { std::lock_guard<std::mutex> lk(mu_); return active_; }
std::string PackManager::error() const { std::lock_guard<std::mutex> lk(mu_); return err_; }
void PackManager::cancel() { cancel_ = true; }
void PackManager::join() { if (th_.joinable()) th_.join(); }

uint64_t PackManager::freeBytes() const {
    std::error_code ec;
    fs::create_directories(u8file(data_), ec);
    const auto s = fs::space(u8file(data_), ec);
    return ec ? 0 : (uint64_t)s.available;
}

bool PackManager::diskCheck(const Pack& p, std::string& why) const {
    const uint64_t need = p.estBytes + p.estBytes / 8 + kReserveBytes;
    const uint64_t fr = freeBytes();
    if (fr < need) {
        char b[160];
        snprintf(b, sizeof b, "not enough free disk space: %.0f MB free, about %.0f MB needed", fr / 1048576.0, need / 1048576.0);
        why = b;
        return false;
    }
    return true;
}

static std::string recPath(const std::string& dir, const std::string& id) { return dir + "/" + id + ".rec"; }

Installed PackManager::installed(const std::string& id) const {
    Installed r;
    r.id = id;
    std::ifstream f(u8file(recPath(metaDir(), id)), std::ios::binary);
    if (!f) return r;
    std::string line;
    if (!std::getline(f, line)) return r;
    const auto h = splitTabs(line);
    if (h.size() < 2) return r;
    r.when = atoll(h[0].c_str());
    r.bytes = strtoull(h[1].c_str(), nullptr, 10);
    while (std::getline(f, line)) if (!line.empty()) r.files++;
    r.ok = true;
    return r;
}

bool PackManager::writeRecord(const Pack& p, const std::vector<PackFile>& files, const std::vector<std::string>& hashes, uint64_t bytes) const {
    std::error_code ec;
    fs::create_directories(u8file(metaDir()), ec);
    const std::string path = recPath(metaDir(), p.id), tmp = path + ".part";
    {
        std::ofstream o(u8file(tmp), std::ios::binary | std::ios::trunc);
        o << (long long)std::time(nullptr) << "\t" << bytes << "\t" << (p.root == Root::Cache ? "cache" : "data") << "\n";
        for (size_t i = 0; i < files.size(); i++) o << files[i].rel << "\t" << hashes[i] << "\n";
        if (!o) return false;
    }
    fs::rename(u8file(tmp), u8file(path), ec);
    return !ec;
}

bool PackManager::start(const std::string& id, std::string* why) {
    auto no = [&](const std::string& s) { if (why) *why = s; return false; };
    const State s = state_.load();
    if (s == State::Downloading || s == State::Verifying) return no("a download is already running");
    const Pack* p = find(id);
    if (!p) return no("unknown pack");
    std::string w;
    if (!diskCheck(*p, w)) return no(w);
    if (th_.joinable()) th_.join();
    cancel_ = false;
    doneBytes_ = 0; doneFiles_ = 0; totalBytes_ = p->estBytes; totalFiles_ = 0;
    { std::lock_guard<std::mutex> lk(mu_); err_.clear(); active_ = id; }
    state_ = State::Downloading;
    Pack copy = *p;
    th_ = dect2::namedThread("onair-packs", [this, copy] { run(copy); });
    return true;
}

void PackManager::run(Pack p) {
    auto fail = [&](const std::string& e) { { std::lock_guard<std::mutex> lk(mu_); err_ = e; } state_ = State::Failed; };
    std::vector<PackFile> files = p.tiles ? tilePackFiles(p) : p.files;
    if (files.empty()) { fail("nothing to download (the area is too large)"); return; }
    totalFiles_ = (long)files.size();
    std::vector<std::string> hashes(files.size(), "-");
    uint64_t done = 0;
    std::error_code ec;
    for (size_t i = 0; i < files.size(); i++) {
        if (cancel_) { state_ = State::Cancelled; return; }
        const PackFile& f = files[i];
        if (!safeRel(f.rel)) { fail("a path in the pack is not allowed: " + f.rel); return; }
        const std::string dst = filePath(p, f), part = dst + ".part";
        fs::create_directories(u8file(dst).parent_path(), ec);
        // resume: a file that is already there and right is kept
        const auto have = fs::is_regular_file(u8file(dst), ec) ? fs::file_size(u8file(dst), ec) : 0;
        bool good = have > 0 && (f.bytes == 0 || have == f.bytes);
        if (good && !p.tiles && !f.sha256.empty()) good = sha256File(dst) == f.sha256;
        if (good) {
            if (!p.tiles) hashes[i] = sha256File(dst);
            done += have; doneBytes_ = done; doneFiles_ = (long)i + 1;
            continue;
        }
        fs::remove(u8file(part), ec);
        const uint64_t maxB = f.bytes ? f.bytes + (f.bytes >> 6) + 4096 : (p.tiles ? (4u << 20) : (1024ull << 20));
        const int rc = fetch_(f.url, part, maxB, cancel_, [&] {
            std::error_code e2;
            const auto sz = fs::file_size(u8file(part), e2);
            if (!e2) doneBytes_ = done + sz;
        });
        if (rc == -1 || cancel_) { fs::remove(u8file(part), ec); state_ = State::Cancelled; return; }
        if (rc != 0) { fs::remove(u8file(part), ec); fail("download of " + f.rel + " failed (error " + std::to_string(rc) + ")"); return; }
        const auto sz = fs::file_size(u8file(part), ec);
        if (ec || sz == 0 || (f.bytes && sz != f.bytes)) { fs::remove(u8file(part), ec); fail(f.rel + ": the downloaded file has the wrong size, not used"); return; }
        if (!p.tiles) {
            const std::string h = sha256File(part);
            if (!f.sha256.empty() && h != f.sha256) { fs::remove(u8file(part), ec); fail(f.rel + ": the SHA-256 does not match the expected one, deleted"); return; }
            hashes[i] = h;
        }
        fs::remove(u8file(dst), ec);
        fs::rename(u8file(part), u8file(dst), ec);
        if (ec) { fail("cannot move " + f.rel + " into place: " + ec.message()); return; }
        done += sz; doneBytes_ = done; doneFiles_ = (long)i + 1;
        if (p.tiles && gapMs_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(gapMs_));
    }
    totalBytes_ = done;
    state_ = State::Verifying;
    if (!writeRecord(p, files, hashes, done)) { fail("cannot write the record of the pack"); return; }
    if (!p.post.empty() && post_) {
        std::string msg;
        if (!post_(p, msg)) { fail(msg.empty() ? "the step after the download failed" : msg); return; }
    }
    state_ = State::Done;
}

VerifyResult PackManager::verify(const std::string& id) {
    VerifyResult r;
    const Pack* p = find(id);
    std::ifstream f(u8file(recPath(metaDir(), id)), std::ios::binary);
    if (!p || !f) { r.detail = "not installed"; return r; }
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        const auto c = splitTabs(line);
        if (c.size() < 2 || !safeRel(c[0])) continue;
        r.checked++;
        const std::string path = root(*p) + "/" + c[0];
        std::error_code ec;
        if (!fs::is_regular_file(u8file(path), ec) || fs::file_size(u8file(path), ec) == 0) { r.missing++; if (r.detail.empty()) r.detail = c[0] + " is missing"; continue; }
        if (c[1] != "-" && sha256File(path) != c[1]) { r.bad++; if (r.detail.empty()) r.detail = c[0] + " does not match its checksum"; }
    }
    if (r.detail.empty()) r.detail = "all " + std::to_string(r.checked) + " files are intact";
    return r;
}

bool PackManager::remove(const std::string& id) {
    const Pack* p = find(id);
    const std::string rp = recPath(metaDir(), id);
    std::ifstream f(u8file(rp), std::ios::binary);
    if (!p || !f) return false;
    std::string line;
    std::getline(f, line);
    std::error_code ec;
    while (std::getline(f, line)) {
        const auto c = splitTabs(line);
        if (c.empty() || !safeRel(c[0])) continue;
        fs::remove(u8file(root(*p) + "/" + c[0]), ec);
        // empty folders of tiles go too, up to the pack's own folder
        auto d = u8file(root(*p) + "/" + c[0]).parent_path();
        for (int k = 0; p->tiles && k < 4 && !ec && fs::is_empty(d, ec) && !ec; k++) { fs::remove(d, ec); d = d.parent_path(); }
    }
    f.close();
    fs::remove(u8file(rp), ec);
    return true;
}

}  // namespace packs
}  // namespace dect2
