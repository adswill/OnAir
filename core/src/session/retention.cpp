#include "dect2/session/retention.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

namespace dect2::sess {

namespace fs = std::filesystem;

namespace {

fs::path pathOf(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::string strOf(const fs::path& p) { const auto u = p.u8string(); return std::string(reinterpret_cast<const char*>(u.c_str()), u.size()); }

// files that belong with `p` and go with it
std::vector<fs::path> sidecars(const fs::path& p) {
    std::vector<fs::path> v;
    fs::path j = p; j.replace_extension(".json");
    if (j != p) v.push_back(j);
    const std::string ext = strOf(p.extension());
    if (ext == ".sigmf-data") { fs::path m = p; m.replace_extension(".sigmf-meta"); v.push_back(m); }
    return v;
}

int64_t fileMs(const fs::file_time_type& t) {
    const auto sys = std::chrono::time_point_cast<std::chrono::milliseconds>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return sys.time_since_epoch().count();
}

}  // namespace

bool insideRoots(const std::string& path, const std::vector<std::string>& roots) {
    std::error_code ec;
    const fs::path f = fs::weakly_canonical(pathOf(path), ec);
    if (ec) return false;
    for (const std::string& r : roots) {
        if (r.empty()) continue;
        const fs::path root = fs::weakly_canonical(pathOf(r), ec);
        if (ec) continue;
        // f must be below root: the relative path may not start with ".."
        const fs::path rel = f.lexically_relative(root);
        if (rel.empty() || rel == ".") continue;
        if (*rel.begin() == "..") continue;
        return true;
    }
    return false;
}

LibraryResult cleanLibrary(SessionDb& db, LinkType linkType, const LibraryRule& rule, int64_t nowMs) {
    LibraryResult res;
    struct Item { std::string ref; int64_t wall = 0; int64_t bytes = 0; bool protectedFile = false; bool gone = false; };
    std::map<std::string, Item> items;   // one per file, however many sessions link it
    for (const LibFile& f : db.libraryFiles((uint8_t)linkType)) {
        Item& it = items[f.ref];
        it.ref = f.ref;
        it.wall = it.wall ? std::min(it.wall, f.wall) : f.wall;
        it.protectedFile = it.protectedFile || f.starred;
    }
    std::vector<Item*> live;
    for (auto& kv : items) {
        Item& it = kv.second;
        std::error_code ec;
        const fs::path p = pathOf(it.ref);
        if (!fs::is_regular_file(p, ec)) {
            // the file is gone (the user deleted it): the link is stale and goes
            db.removeLinksByRef(it.ref);
            res.staleLinks++;
            continue;
        }
        if (!insideRoots(it.ref, rule.roots)) continue;
        it.bytes = (int64_t)fs::file_size(p, ec);
        live.push_back(&it);
    }
    auto drop = [&](Item& it) {
        std::error_code ec;
        const fs::path p = pathOf(it.ref);
        if (fs::remove(p, ec)) {
            res.filesDeleted++;
            res.bytesFreed += it.bytes;
            res.deleted.push_back(it.ref);
        }
        for (const fs::path& s : sidecars(p)) { std::error_code e2; fs::remove(s, e2); }
        db.removeLinksByRef(it.ref);
        it.gone = true;
        // an emptied day folder goes too (never the root itself)
        std::error_code e3;
        const fs::path dir = p.parent_path();
        if (fs::is_empty(dir, e3) && !e3) {
            bool isRoot = false;
            for (const std::string& r : rule.roots) { std::error_code e4; if (fs::equivalent(dir, pathOf(r), e4)) isRoot = true; }
            if (!isRoot) fs::remove(dir, e3);
        }
    };
    std::sort(live.begin(), live.end(), [](const Item* a, const Item* b) { return a->wall < b->wall; });
    if (rule.maxAgeDays > 0) {
        const int64_t cutoff = nowMs - (int64_t)rule.maxAgeDays * 86400000ll;
        for (Item* it : live) if (!it->protectedFile && it->wall < cutoff) drop(*it);
    }
    if (rule.maxBytes > 0) {
        int64_t total = 0;
        for (Item* it : live) if (!it->gone) total += it->bytes;
        for (Item* it : live) {
            if (total <= rule.maxBytes) break;
            if (it->gone || it->protectedFile) continue;
            total -= it->bytes;
            drop(*it);
        }
    }
    return res;
}

bool moveToTrash(const std::string& path, const std::string& trashDir, int64_t nowMs) {
    std::error_code ec;
    const fs::path p = pathOf(path);
    if (!fs::is_regular_file(p, ec) || trashDir.empty()) return false;
    const time_t t = (time_t)(nowMs / 1000);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char day[16];
    snprintf(day, sizeof day, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    const fs::path dir = pathOf(trashDir) / day;
    fs::create_directories(dir, ec);
    if (ec) return false;
    auto moveOne = [&](const fs::path& src) -> bool {
        fs::path dst = dir / src.filename();
        for (int k = 2; fs::exists(dst, ec) && k < 1000; k++) dst = dir / (src.stem().string() + "_" + std::to_string(k) + src.extension().string());
        fs::rename(src, dst, ec);
        if (ec) { fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec); if (!ec) fs::remove(src, ec); }
        return !ec;
    };
    const bool ok = moveOne(p);
    for (const fs::path& s : sidecars(p)) if (fs::is_regular_file(s, ec)) moveOne(s);
    return ok;
}

int purgeTrash(const std::string& trashDir, int days, int64_t nowMs) {
    std::error_code ec;
    const fs::path root = pathOf(trashDir);
    if (trashDir.empty() || !fs::is_directory(root, ec) || days < 0) return 0;
    const int64_t cutoff = nowMs - (int64_t)days * 86400000ll;
    int n = 0;
    std::vector<fs::path> dirs;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) { dirs.push_back(it->path()); continue; }
        if (!it->is_regular_file(e2)) continue;
        if (fileMs(it->last_write_time(e2)) < cutoff) { if (fs::remove(it->path(), e2)) n++; }
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.string().size() > b.string().size(); });
    for (const fs::path& d : dirs) { std::error_code e3; if (fs::is_empty(d, e3) && !e3) fs::remove(d, e3); }
    return n;
}

}  // namespace dect2::sess
