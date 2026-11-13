// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Offline packs: catalog, tile pack plan, download through a local-copy fetcher, resume, checksum failure, cancel, disk guard, verify, delete.
#include "dect2/packs/packs.h"
#include "dect2/engine/updater.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace dect2::packs;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }

int main() {
    const fs::path T = fs::temp_directory_path() / "onair_test_packs";
    fs::remove_all(T);
    const fs::path src = T / "src", data = T / "data", cache = T / "cache";
    put(src / "a.txt", "alpha alpha alpha");
    put(src / "b.txt", "bravo bravo");
    put(src / "t.png", "PNGDATA");
    const std::string ha = dect2::sha256File((src / "a.txt").string());

    // catalog
    auto cat = builtinCatalog();
    CHECK(cat.size() > 15, "catalog has %zu packs", cat.size());
    bool allOk = true;
    for (const Pack& p : cat) {
        allOk &= !p.id.empty() && !p.licence.empty() && !p.source.empty() && !p.attribution.empty() && (p.tiles ? p.tileCount > 0 && p.tileCount <= 4000 : !p.files.empty());
        for (const PackFile& f : p.files) allOk &= f.url.compare(0, 8, "https://") == 0 && f.rel.find("..") == std::string::npos;
    }
    CHECK(allOk, "every builtin pack has source, licence, credit, an https address and at most 4000 tiles");

    // the TSV catalog of the user: files of one id join; a path that leaves the folder is refused
    std::string err;
    auto u = parseCatalogTsv("# c\nmine\tMine\tData\tme\tCC0\tme\thttps://x/a\tdir/a.txt\t17\t" + ha + "\nmine\tMine\tData\tme\tCC0\tme\thttps://x/b\tdir/b.txt\nbad\tB\tD\ts\tl\ta\thttps://x/c\t../c.txt\n", &err);
    CHECK(u.size() == 1 && u[0].files.size() == 2 && u[0].estBytes == 17 && !err.empty(), "user catalog: %zu packs, %zu files, err '%s'", u.size(), u.empty() ? 0 : u[0].files.size(), err.c_str());

    // tile pack
    Pack tp;
    tp.id = "t"; tp.title = "t"; tp.category = "Map tiles"; tp.source = "s"; tp.licence = "l"; tp.attribution = "a";
    tp.south = 54.6; tp.west = 25.2; tp.north = 54.8; tp.east = 25.4; tp.minZoom = 0; tp.maxZoom = 19;
    tp.tileUrl = "file://" + src.string() + "/t.png";
    finishTilePack(tp);
    CHECK(tp.maxZoom >= 12 && tp.maxZoom < 19 && tp.tileCount > 0 && tp.tileCount <= 4000, "zoom capped to %d, %ld tiles", tp.maxZoom, tp.tileCount);
    CHECK(tilePackFiles(tp).size() == (size_t)tp.tileCount, "tile list matches count");
    Pack big = tp; big.south = -80; big.north = 80; big.west = -170; big.east = 170; big.minZoom = 8; big.maxZoom = 8;
    CHECK(tilePackFiles(big).empty(), "too many tiles gives no list");

    // download with a copy fetcher that counts calls
    PackManager m(data.string(), cache.string());
    Pack f1;
    f1.id = "f1"; f1.title = "f1"; f1.category = "x"; f1.source = "s"; f1.licence = "l"; f1.attribution = "a"; f1.estBytes = 100;
    PackFile a, b; a.url = "file://" + (src / "a.txt").string(); a.rel = "sub/a.txt"; a.bytes = 17; a.sha256 = ha;
    b.url = "file://" + (src / "b.txt").string(); b.rel = "b.txt";
    f1.files = {a, b};
    Pack f2 = f1; f2.id = "f2"; f2.files[0].sha256 = std::string(64, '0');   // wrong checksum
    Pack f3 = f1; f3.id = "f3"; f3.root = Root::Cache; f3.post = "step";
    Pack f4 = f1; f4.id = "f4"; f4.estBytes = 1ull << 60;                      // never fits
    std::vector<Pack> all = {f1, f2, f3, f4, tp};
    m.setCatalog(all);
    int calls = 0;
    std::atomic<bool> failNext{false};
    m.setFetcher([&](const std::string& url, const std::string& out, uint64_t, const std::atomic<bool>& cancel, const std::function<void()>& tick) {
        calls++;
        if (failNext) return 7;
        if (cancel) return -1;
        std::error_code ec;
        fs::copy_file(url.substr(7), out, fs::copy_options::overwrite_existing, ec);
        tick();
        return ec ? 1 : 0;
    });
    m.setTileGapMs(0);

    CHECK(!m.installed("f1").ok, "not installed at first");
    CHECK(m.start("f1"), "start");
    m.join();
    CHECK(m.state() == PackManager::State::Done && calls == 2, "done, %d fetches (%s)", calls, m.error().c_str());
    CHECK(fs::exists(data / "sub/a.txt") && fs::exists(data / "b.txt") && !fs::exists(data / "sub/a.txt.part"), "files in place, no part left");
    Installed in = m.installed("f1");
    CHECK(in.ok && in.files == 2 && in.bytes == 28 && in.when > 0, "record: %ld files %llu bytes", in.files, (unsigned long long)in.bytes);
    CHECK(m.verify("f1").bad == 0 && m.verify("f1").checked == 2, "verify clean");

    // resume: one file lost, one kept -> only one fetch
    fs::remove(data / "b.txt");
    calls = 0;
    CHECK(m.start("f1"), "restart");
    m.join();
    CHECK(m.state() == PackManager::State::Done && calls == 1, "resume fetched only the missing file (%d)", calls);

    // verify finds damage and loss
    put(data / "sub/a.txt", "tampered tamper!");
    VerifyResult v = m.verify("f1");
    CHECK(v.bad == 1, "verify finds the changed file (%ld bad, %s)", v.bad, v.detail.c_str());
    fs::remove(data / "b.txt");
    CHECK(m.verify("f1").missing == 1, "verify finds the missing file");

    // wrong checksum: failed, nothing left, no record
    CHECK(m.start("f2"), "start f2");
    m.join();
    CHECK(m.state() == PackManager::State::Failed && !fs::exists(data / "sub/a.txt.part") && !m.installed("f2").ok, "bad checksum refused: %s", m.error().c_str());

    // network failure
    failNext = true;
    CHECK(m.start("f3"), "start f3");
    m.join();
    CHECK(m.state() == PackManager::State::Failed && !m.installed("f3").ok, "fetch failure reported");
    failNext = false;

    // post step on the cache root
    int postCalls = 0;
    m.setPostStep([&](const Pack& p, std::string&) { postCalls++; return p.post == "step"; });
    CHECK(m.start("f3"), "start f3 again");
    m.join();
    CHECK(m.state() == PackManager::State::Done && postCalls == 1 && fs::exists(cache / "b.txt") && m.installed("f3").ok, "cache root + post step");

    // disk guard
    std::string why;
    CHECK(!m.start("f4", &why) && why.find("free disk space") != std::string::npos, "disk guard: %s", why.c_str());
    CHECK(m.state() == PackManager::State::Done, "state unchanged by the refused start");

    // cancel
    m.setFetcher([&](const std::string&, const std::string&, uint64_t, const std::atomic<bool>&, const std::function<void()>&) { return -1; });
    CHECK(m.start("f1"), "start for cancel");
    m.join();
    CHECK(m.state() == PackManager::State::Cancelled, "cancelled");
    m.setFetcher([&](const std::string& url, const std::string& out, uint64_t, const std::atomic<bool>&, const std::function<void()>&) {
        std::error_code ec; fs::copy_file(url.substr(7), out, fs::copy_options::overwrite_existing, ec); return ec ? 1 : 0; });

    // tiles
    CHECK(m.start("t"), "start tiles");
    m.join();
    const auto files = tilePackFiles(tp);
    size_t have = 0;
    for (const auto& f : files) have += fs::exists(data / f.rel);
    CHECK(m.state() == PackManager::State::Done && have == files.size() && m.installed("t").files == (long)files.size(), "tiles: %zu of %zu present (%s)", have, files.size(), m.error().c_str());
    CHECK(m.verify("t").bad == 0, "tiles verify");

    // delete
    CHECK(m.remove("t") && !fs::exists(data / "maps"), "tiles deleted with their folders");
    CHECK(m.remove("f1") && !fs::exists(data / "sub/a.txt") && !m.installed("f1").ok, "pack deleted");
    CHECK(!m.remove("f1"), "second delete says nothing installed");

    fs::remove_all(T);
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
