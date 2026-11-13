// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Offline data packs: lists what can be downloaded for use without internet (map tiles of a region, satellite elements, airports, transmitter lists),
// and downloads, checks, re-checks and deletes a pack. Nothing is downloaded unless the caller starts a pack (the user's click); no network code
// of its own: the transfer goes through a Fetcher (curl by default, a local copy in the tests).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {
namespace packs {

enum class Root { Data, Cache };   // under plat::dataDir() (kept) or plat::cacheDir() (system may clean it)

struct PackFile {
    std::string url;
    std::string rel;           // path under the root, "/" separated
    uint64_t bytes = 0;        // exact size when known (0: unknown, only "not empty" is checked)
    std::string sha256;        // pinned digest when known; otherwise the digest of the download is recorded and used by Verify
};

struct Pack {
    std::string id;            // "tle-weather", "tiles-lt"
    std::string title;
    std::string category;      // "Map tiles", "Satellites", "Airports", "Transmitters"
    std::string source;        // where it comes from (host / project)
    std::string licence;
    std::string attribution;   // the credit that has to stay visible
    uint64_t estBytes = 0;     // size shown before the download
    Root root = Root::Data;
    std::vector<PackFile> files;   // empty for a tile pack: made from the box and zooms
    std::string post;          // name of a step the application runs when all files are in place ("" none)
    // tile pack
    bool tiles = false;
    double south = 0, west = 0, north = 0, east = 0;
    int minZoom = 0, maxZoom = 0;
    std::string tileUrl;       // "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
    std::string tileDir;       // "maps/osm"
    long tileCount = 0;        // filled in by finishTilePack
};

// Fills estBytes, tileCount and maxZoom (capped so the plan stays within the tile limit) of a tile pack.
void finishTilePack(Pack& p);
// The files of a tile pack (empty when the box is over the limit).
std::vector<PackFile> tilePackFiles(const Pack& p);

std::vector<Pack> builtinCatalog();
// Extra packs from the user: a tab separated file, one pack file per line:
//   id title category source licence attribution url rel bytes sha256     (lines with the same id are files of one pack; # comments)
std::vector<Pack> parseCatalogTsv(const std::string& text, std::string* err = nullptr);

struct Installed {
    std::string id;
    long long when = 0;        // unix time of the download
    long files = 0;
    uint64_t bytes = 0;
    bool ok = false;
};

struct VerifyResult { long checked = 0, bad = 0, missing = 0; std::string detail; };

// Where a transfer goes. rc 0 = ok, -1 = cancelled, other = failed (message from error(rc)). tick() runs about every 100 ms.
using Fetcher = std::function<int(const std::string& url, const std::string& outFile, uint64_t maxBytes, const std::atomic<bool>& cancel, const std::function<void()>& tick)>;
Fetcher curlFetcher();   // https only, redirects https only

constexpr uint64_t kReserveBytes = 200ull << 20;   // a download leaves at least this much free

class PackManager {
public:
    enum class State { Idle, Downloading, Verifying, Done, Failed, Cancelled };
    PackManager(std::string dataRoot, std::string cacheRoot);
    ~PackManager();
    void setCatalog(std::vector<Pack> c);
    const std::vector<Pack>& catalog() const { return cat_; }
    const Pack* find(const std::string& id) const;
    void setFetcher(Fetcher f) { fetch_ = std::move(f); }
    void setPostStep(std::function<bool(const Pack&, std::string& msg)> f) { post_ = std::move(f); }
    void setTileGapMs(int ms) { gapMs_ = ms; }

    std::string root(const Pack& p) const { return p.root == Root::Cache ? cache_ : data_; }
    std::string filePath(const Pack& p, const PackFile& f) const { return root(p) + "/" + f.rel; }
    Installed installed(const std::string& id) const;
    // false (and why) when the files will not fit: needs estimated size + reserve free
    bool diskCheck(const Pack& p, std::string& why) const;
    uint64_t freeBytes() const;

    // Starts in the background; false when busy or unknown. Files already in place and correct are skipped (that is the resume).
    bool start(const std::string& id, std::string* why = nullptr);
    void cancel();
    State state() const { return state_.load(); }
    std::string activeId() const;
    std::string error() const;
    uint64_t doneBytes() const { return doneBytes_.load(); }
    uint64_t totalBytes() const { return totalBytes_.load(); }
    long doneFiles() const { return doneFiles_.load(); }
    long totalFiles() const { return totalFiles_.load(); }
    void join();   // waits for the running download (tests)

    VerifyResult verify(const std::string& id);
    bool remove(const std::string& id);   // deletes the files and the record; false when nothing was installed

private:
    void run(Pack p);
    std::string metaDir() const { return data_ + "/packs"; }
    bool writeRecord(const Pack& p, const std::vector<PackFile>& files, const std::vector<std::string>& hashes, uint64_t bytes) const;
    std::string data_, cache_;
    std::vector<Pack> cat_;
    Fetcher fetch_;
    std::function<bool(const Pack&, std::string&)> post_;
    int gapMs_ = 40;
    std::atomic<State> state_{State::Idle};
    std::atomic<uint64_t> doneBytes_{0}, totalBytes_{0};
    std::atomic<long> doneFiles_{0}, totalFiles_{0};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mu_;
    std::string err_, active_;
    std::thread th_;
};

}  // namespace packs
}  // namespace dect2
