// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The plugin store client against local mock registries (never a real network host): a good install, a tampered index, a wrong sha256, a wrong
// ABI, a too-new minimum version, downgrades, revocation, a failed update that is rolled back, the offline cache, the platform filter, updates
// and uninstall; plus the checked-in sample registry, which was signed by the Python tool (so this also proves Python signs what C++ verifies).
#include "dect2/pstore/store.h"
#include "dect2/automation/json_mini.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/plugin/zip.h"
#include "../common/test_util.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>

using namespace dect2;
namespace fs = std::filesystem;
namespace aj = automation::json;
namespace ps = dect2::pstore;

namespace {
const uint8_t kSeed[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
const char* kPlatform = "linux-x86_64";
const char* kHost = "3.9.0";

void writeFile(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
std::string readFile(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
std::string pubHex() { uint8_t pub[32]; meshcrypto::ed25519PublicKey(kSeed, pub); return ps::toHex(pub, 32); }
std::string signHex(const std::string& data) { uint8_t sig[64]; meshcrypto::ed25519Sign(kSeed, (const uint8_t*)data.data(), data.size(), sig); return ps::toHex(sig, 64); }

struct Spec {
    std::string id = "demo", ver = "1.0.0", minOnAir, review = "reviewed", pkgVer, badUrl;
    int abi = 1;
    std::vector<std::string> perms, pkgPerms;
    bool pkgPermsSet = false, badCrc = false, badSha = false;
    std::vector<std::string> platforms{kPlatform};
};
aj::Value strArr(const std::vector<std::string>& v) { aj::Value a = aj::Value::array(); for (const auto& s : v) a.push(aj::Value::string(s)); return a; }

struct Reg {
    fs::path dir;
    int serial = 1;
    std::vector<Spec> plugins;
    std::vector<ps::Revocation> revoked;
    void write() {
        fs::remove_all(dir);
        aj::Value idx = aj::Value::object(), list = aj::Value::array(), rev = aj::Value::array();
        idx.set("schema", aj::Value::number(1)); idx.set("serial", aj::Value::number(serial)); idx.set("generated", aj::Value::string("2026-11-11T00:00:00Z"));
        for (const auto& s : plugins) {
            // package
            const std::string lib = s.id + ".so";
            aj::Value man = aj::Value::object();
            man.set("id", aj::Value::string(s.id)); man.set("name", aj::Value::string("Demo")); man.set("version", aj::Value::string(s.pkgVer.empty() ? s.ver : s.pkgVer));
            man.set("author", aj::Value::string("t")); man.set("license", aj::Value::string("MIT")); man.set("kind", aj::Value::string("action")); man.set("abi_version", aj::Value::number(s.abi));
            man.set("library", aj::Value::string(lib)); man.set("permissions", strArr(s.pkgPermsSet ? s.pkgPerms : s.perms));
            if (!s.minOnAir.empty()) man.set("minimum_onair", aj::Value::string(s.minOnAir));
            std::string body = "LIBRARY-BYTES-LIBRARY-BYTES-LIBRARY-BYTES";
            const std::string manText = aj::dump(man);
            auto pkg = plugin::zipStore({{"manifest.json", std::vector<uint8_t>(manText.begin(), manText.end())}, {lib, std::vector<uint8_t>(body.begin(), body.end())}});
            std::string sha = ps::sha256Hex(pkg);
            if (s.badCrc) { auto it = std::search(pkg.begin(), pkg.end(), body.begin(), body.end()); if (it != pkg.end()) *(it + 3) ^= 0x55; sha = ps::sha256Hex(pkg); }   // valid hash, broken zip
            if (s.badSha) sha = std::string(64, 'a');
            const std::string rel = "packages/" + s.id + "/" + s.id + "-" + s.ver + ".onairplugin";
            writeFile(dir / rel, std::string(pkg.begin(), pkg.end()));
            // page
            aj::Value page = aj::Value::object(), dls = aj::Value::object();
            page.set("schema", aj::Value::number(1)); page.set("id", aj::Value::string(s.id)); page.set("version", aj::Value::string(s.ver));
            page.set("description", aj::Value::string("A demo plugin.")); page.set("author", aj::Value::string("t")); page.set("source", aj::Value::string("https://example.invalid/src"));
            for (const auto& p : s.platforms) {
                aj::Value d = aj::Value::object();
                d.set("url", aj::Value::string(s.badUrl.empty() ? rel : s.badUrl)); d.set("size", aj::Value::number((double)pkg.size())); d.set("sha256", aj::Value::string(sha));
                dls.set(p, d);
            }
            page.set("downloads", dls);
            const std::string pageText = aj::dump(page, true);
            writeFile(dir / "plugins" / s.id / "plugin.json", pageText);
            aj::Value e = aj::Value::object(), mf = aj::Value::object();
            e.set("id", aj::Value::string(s.id)); e.set("name", aj::Value::string("Demo")); e.set("summary", aj::Value::string("A demo plugin")); e.set("version", aj::Value::string(s.ver));
            e.set("author", aj::Value::string("t")); e.set("license", aj::Value::string("MIT")); e.set("kind", aj::Value::string("action")); e.set("abi_version", aj::Value::number(s.abi));
            e.set("min_onair", aj::Value::string(s.minOnAir)); e.set("review", aj::Value::string(s.review)); e.set("permissions", strArr(s.perms));
            e.set("categories", strArr({"testing"})); e.set("tags", strArr({"demo", "example"})); e.set("platforms", strArr(s.platforms));
            mf.set("path", aj::Value::string("plugins/" + s.id + "/plugin.json"));
            mf.set("sha256", aj::Value::string(ps::sha256Hex(std::vector<uint8_t>(pageText.begin(), pageText.end()))));
            e.set("manifest", mf);
            list.push(e);
        }
        for (const auto& r : revoked) { aj::Value o = aj::Value::object(); o.set("id", aj::Value::string(r.id)); o.set("version", aj::Value::string(r.version)); o.set("reason", aj::Value::string(r.reason)); rev.push(o); }
        idx.set("plugins", list); idx.set("revoked", rev);
        const std::string text = aj::dump(idx, true) + "\n";
        writeFile(dir / "index.json", text);
        writeFile(dir / "index.json.sig", signHex(text) + "\n");
    }
};

struct Env {
    fs::path root, plugins;
    Reg reg;
    std::unique_ptr<plugin::Manager> mgr;
    std::unique_ptr<ps::Store> store;
    explicit Env(const std::string& name) {
        root = fs::temp_directory_path() / ("onair_pstore_" + name + "_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove_all(root);
        plugins = root / "plugins";
        reg.dir = root / "registry";
        plugin::ManagerConfig mc; mc.pluginsDir = plugins.string(); mc.hostVersion = kHost;
        mgr.reset(new plugin::Manager(mc));
        mgr->scan();
    }
    ~Env() { store.reset(); mgr.reset(); std::error_code ec; fs::remove_all(root, ec); }
    ps::Config cfg(const std::string& url = {}, const std::string& key = {}) const {
        ps::Config c; c.registryUrl = url.empty() ? reg.dir.string() : url; c.publicKeyHex = key.empty() ? pubHex() : key; c.cacheDir = (plugins / ".store").string();
        c.platform = kPlatform; c.hostVersion = kHost; return c;
    }
    void open(const std::string& url = {}) { store.reset(new ps::Store(cfg(url), mgr.get())); }
    bool installed(const std::string& id, std::string* ver = nullptr) { plugin::Entry e; const bool f = mgr->find(id, e) && e.valid; if (f && ver) *ver = e.m.version; return f; }
};
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
}

int main() {
    // revocation specs
    CHECK(ps::revocationMatches("*", "1.0"), "star matches");
    CHECK(ps::revocationMatches("1.2.0", "1.2.0") && !ps::revocationMatches("1.2.0", "1.2.1"), "exact");
    CHECK(ps::revocationMatches("<=1.2.0", "1.1.9") && ps::revocationMatches("<=1.2.0", "1.2.0") && !ps::revocationMatches("<=1.2.0", "1.10.0"), "at most");

    // 1. good install, and the confirmation is required
    {
        Env e("good");
        e.reg.plugins = {Spec{}}; e.reg.write(); e.open();
        CHECK(e.store->refresh(), "refresh: %s", e.store->lastError().c_str());
        CHECK(e.store->source() == ps::Source::Network && e.store->serial() == 1, "network source, serial 1");
        auto l = e.store->list();
        CHECK(l.size() == 1 && l[0].id == "demo" && l[0].review == "reviewed", "one reviewed listing");
        CHECK(e.store->list("zzz").empty() && e.store->list("EXAMPLE").size() == 1 && e.store->list("", "testing").size() == 1 && e.store->list("", "nope").empty(), "search and category");
        auto r = e.store->install("demo", false);
        CHECK(!r.ok && has(r.error, "confirm") && !e.installed("demo"), "no install without the native-code confirmation: %s", r.error.c_str());
        r = e.store->install("demo", true);
        std::string v;
        CHECK(r.ok && e.installed("demo", &v) && v == "1.0.0", "installed: %s", r.error.c_str());
        ps::Installed info;
        CHECK(e.store->installedInfo("demo", info) && info.version == "1.0.0" && info.review == "reviewed" && !info.installedAt.empty(), "store recorded the install");
        CHECK(fs::exists(e.plugins / "demo" / "manifest.json") && fs::exists(e.plugins / "demo" / "demo.so"), "files are in the plugins folder");
        CHECK(!fs::exists(e.plugins / ".install-demo"), "no temp folder left");
        // 12. uninstall
        r = e.store->uninstall("demo");
        CHECK(r.ok && !e.installed("demo") && !fs::exists(e.plugins / "demo") && !e.store->installedInfo("demo", info), "uninstalled: %s", r.error.c_str());
    }
    // 2. tampered index signature
    {
        Env e("tamper");
        e.reg.plugins = {Spec{}}; e.reg.write();
        std::string t = readFile(e.reg.dir / "index.json");
        t.replace(t.find("Demo"), 4, "Demx");
        writeFile(e.reg.dir / "index.json", t);
        e.open();
        CHECK(!e.store->refresh() && has(e.store->lastError(), "signature"), "tampered index refused: %s", e.store->lastError().c_str());
        CHECK(e.store->list().empty() && !e.store->install("demo", true).ok, "nothing from a tampered index is offered or installable");
        // a flipped signature, and a signature of another key
        e.reg.write();
        std::string s = readFile(e.reg.dir / "index.json.sig"); s[0] = s[0] == '0' ? '1' : '0';
        writeFile(e.reg.dir / "index.json.sig", s);
        CHECK(!e.store->refresh() && has(e.store->lastError(), "signature"), "flipped signature refused");
        e.reg.write();
        e.store.reset(new ps::Store(e.cfg("", std::string(64, '1')), e.mgr.get()));
        CHECK(!e.store->refresh() && has(e.store->lastError(), "signature"), "another key refused");
        // a key of all zeros: no network use at all, clear message
        e.store.reset(new ps::Store(e.cfg("", std::string(64, '0')), e.mgr.get()));
        CHECK(!e.store->signatureKeyConfigured() && !e.store->refresh() && has(e.store->lastError(), "key"), "no key configured: %s", e.store->lastError().c_str());
    }
    // 3. wrong sha256; 17. a path that leaves the registry; extra permissions in the package
    {
        Env e("sha");
        Spec s; s.badSha = true;
        Spec t; t.id = "evil"; t.badUrl = "../outside.zip";
        Spec u; u.id = "greedy"; u.pkgPerms = {"filesystem", "network"}; u.pkgPermsSet = true;
        e.reg.plugins = {s, t, u}; e.reg.write(); e.open();
        CHECK(e.store->refresh(), "refresh");
        auto r = e.store->install("demo", true);
        CHECK(!r.ok && has(r.error, "sha256") && !e.installed("demo") && !fs::exists(e.plugins / "demo"), "wrong sha256 refused, nothing written: %s", r.error.c_str());
        r = e.store->install("evil", true);
        CHECK(!r.ok && !e.installed("evil"), "a download path with .. refused: %s", r.error.c_str());
        r = e.store->install("greedy", true);
        CHECK(!r.ok && has(r.error, "permissions") && !e.installed("greedy"), "a package that asks for more permissions than listed is refused: %s", r.error.c_str());
    }
    // 5 and 6. wrong ABI and too-new minimum version; 11. platform filter
    {
        Env e("compat");
        Spec a; a.id = "abi2"; a.abi = 2;
        Spec b; b.id = "future"; b.minOnAir = "9.0.0";
        Spec c; c.id = "winonly"; c.platforms = {"windows-x64"};
        Spec d; d.id = "okay";
        e.reg.plugins = {a, b, c, d}; e.reg.write(); e.open();
        CHECK(e.store->refresh(), "refresh");
        auto l = e.store->list();
        CHECK(l.size() == 1 && l[0].id == "okay" && e.store->hiddenCount() == 3, "only the compatible plugin is listed (%zu listed, %zu hidden)", l.size(), e.store->hiddenCount());
        auto r = e.store->install("abi2", true);
        CHECK(!r.ok && has(r.error, "interface version"), "wrong ABI refused: %s", r.error.c_str());
        r = e.store->install("future", true);
        CHECK(!r.ok && has(r.error, "OnAir 9.0.0"), "too-new minimum version refused: %s", r.error.c_str());
        r = e.store->install("winonly", true);
        CHECK(!r.ok && has(r.error, "not available for"), "other platform refused: %s", r.error.c_str());
        // the same registry seen from a Windows computer
        auto c2 = e.cfg(); c2.platform = "windows-x64";
        ps::Store w(c2, e.mgr.get());
        CHECK(w.refresh() && w.list().size() == 1 && w.list()[0].id == "winonly", "platform filter: windows sees its own plugin");
    }
    // 7 and 13. update, downgrade, rollback of the index serial
    {
        Env e("update");
        Spec s; s.ver = "1.0.0";
        e.reg.plugins = {s}; e.reg.write(); e.open();
        CHECK(e.store->refresh() && e.store->install("demo", true).ok, "v1 installed");
        CHECK(e.store->updates().empty(), "no update yet");
        e.reg.serial = 2; e.reg.plugins[0].ver = "1.1.0"; e.reg.write();
        CHECK(e.store->refresh(), "refresh 2: %s", e.store->lastError().c_str());
        auto up = e.store->updates();
        CHECK(up.size() == 1 && up[0].id == "demo" && up[0].installed == "1.0.0" && up[0].available == "1.1.0", "update found");
        std::string v;
        auto r = e.store->install("demo", true);
        CHECK(r.ok && e.installed("demo", &v) && v == "1.1.0" && e.store->updates().empty(), "updated to 1.1.0: %s", r.error.c_str());
        // the registry now offers an older version: refused
        e.reg.serial = 3; e.reg.plugins[0].ver = "1.0.5"; e.reg.write();
        CHECK(e.store->refresh(), "refresh 3");
        r = e.store->install("demo", true);
        CHECK(!r.ok && has(r.error, "downgrade") && e.installed("demo", &v) && v == "1.1.0", "downgrade refused: %s", r.error.c_str());
        // an old index replayed (lower serial) is ignored
        e.reg.serial = 2; e.reg.plugins[0].ver = "9.9.9"; e.reg.write();
        CHECK(!e.store->refresh() && has(e.store->lastError(), "older"), "a replayed older index is refused: %s", e.store->lastError().c_str());
        ps::Listing keep;
        CHECK(e.store->serial() == 3 && e.store->find("demo", keep) && keep.version == "1.0.5", "the newer list stays in use");
    }
    // 9. a failed update is rolled back
    {
        Env e("rollback");
        e.reg.plugins = {Spec{}}; e.reg.write(); e.open();
        CHECK(e.store->refresh() && e.store->install("demo", true).ok, "v1 installed");
        std::string v;
        // (a) the package is not what the list says (other version inside)
        e.reg.serial = 2; e.reg.plugins[0].ver = "2.0.0"; e.reg.plugins[0].pkgVer = "1.9.9"; e.reg.write();
        CHECK(e.store->refresh(), "refresh");
        auto r = e.store->install("demo", true);
        CHECK(!r.ok && e.installed("demo", &v) && v == "1.0.0", "mismatching package refused, 1.0.0 stays: %s", r.error.c_str());
        // (b) a package with a valid hash whose archive is damaged
        e.reg.serial = 3; e.reg.plugins[0].pkgVer = ""; e.reg.plugins[0].badCrc = true; e.reg.write();
        CHECK(e.store->refresh(), "refresh");
        r = e.store->install("demo", true);
        CHECK(!r.ok && e.installed("demo", &v) && v == "1.0.0" && fs::exists(e.plugins / "demo" / "demo.so"), "damaged archive: update fails, 1.0.0 stays installed: %s", r.error.c_str());
        CHECK(!fs::exists(e.plugins / ".install-demo"), "no half-installed folder");
        // and then a good 2.0.0 works
        e.reg.serial = 4; e.reg.plugins[0].badCrc = false; e.reg.write();
        CHECK(e.store->refresh() && e.store->install("demo", true).ok && e.installed("demo", &v) && v == "2.0.0", "the next good update installs");
    }
    // 8. revocation: the kill switch
    {
        Env e("revoke");
        e.reg.plugins = {Spec{}}; e.reg.write(); e.open();
        CHECK(e.store->refresh() && e.store->install("demo", true).ok, "installed");
        std::string err;
        CHECK(e.mgr->setEnabled("demo", true, err), "enabled");
        e.reg.serial = 2; e.reg.revoked = {{"demo", "<=1.0.0", "it records audio without asking"}}; e.reg.write();
        CHECK(e.store->refresh(), "refresh");
        plugin::Entry en;
        CHECK(e.mgr->find("demo", en) && en.revoked && !en.valid && !en.enabled && has(en.error, "records audio"), "revoked plugin is off with the reason: %s", en.error.c_str());
        CHECK(!e.mgr->setEnabled("demo", true, err) && has(err, "Revoked"), "it cannot be turned on again: %s", err.c_str());
        bool listedAvail = false; for (const auto& m : e.mgr->modeRows()) if (m.pluginId == "demo" && m.available) listedAvail = true;
        CHECK(!listedAvail, "no available mode rows");
        CHECK(e.store->list().empty() && !e.store->install("demo", true).ok, "a revoked plugin is neither listed nor installable");
        // the block survives a restart without any network
        e.store.reset(); e.mgr.reset();
        plugin::ManagerConfig mc; mc.pluginsDir = e.plugins.string(); mc.hostVersion = kHost;
        e.mgr.reset(new plugin::Manager(mc)); e.mgr->scan();
        CHECK(e.mgr->find("demo", en) && en.revoked && !en.valid, "still revoked after a restart");
        // a later list without the entry lifts it
        e.open("");
        e.reg.serial = 3; e.reg.revoked.clear(); e.reg.write();
        CHECK(e.store->refresh() && e.mgr->find("demo", en) && !en.revoked && en.valid, "lifted when the registry lifts it");
    }
    // 10. offline cache
    {
        Env e("offline");
        e.reg.plugins = {Spec{}}; e.reg.write(); e.open();
        CHECK(e.store->refresh(), "online refresh");
        e.store.reset();
        e.store.reset(new ps::Store(e.cfg((e.root / "gone").string()), e.mgr.get()));
        CHECK(e.store->list().empty(), "a new Store starts empty");
        CHECK(!e.store->refresh(), "refresh of an unreachable registry fails");
        CHECK(e.store->source() == ps::Source::Cache && e.store->list().size() == 1 && has(e.store->notice(), "Showing the list saved on 2026-11-11"), "cache is used with a clear notice: %s", e.store->notice().c_str());
        // loadCache alone (start-up, no network)
        e.store.reset(new ps::Store(e.cfg((e.root / "gone").string()), e.mgr.get()));
        CHECK(e.store->loadCache() && e.store->list().size() == 1, "loadCache works without any network");
        // a damaged cache is not trusted
        writeFile(e.plugins / ".store" / "index.json", readFile(e.plugins / ".store" / "index.json") + " ");
        e.store.reset(new ps::Store(e.cfg((e.root / "gone").string()), e.mgr.get()));
        CHECK(!e.store->loadCache(), "a cache that fails the signature check is ignored");
    }
    // 15. the checked-in sample registry (signed by tools/marketplace/onair_registry.py with the published test key)
    {
        const fs::path sample = fs::path(__FILE__).parent_path().parent_path().parent_path() / "tools" / "marketplace" / "sample-registry";
        if (!fs::exists(sample / "index.json")) { printf("sample registry not found at %s: skipped\n", sample.string().c_str()); }
        else {
            std::string key = readFile(sample / "TEST-PUBLIC-KEY.txt"); while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) key.pop_back();
            CHECK(ps::verifyIndexSignature(readFile(sample / "index.json"), readFile(sample / "index.json.sig"), key), "the Python-made signature verifies in C++");
            Env e("sample");
            e.store.reset(new ps::Store(e.cfg(sample.string(), key), e.mgr.get()));
            CHECK(e.store->refresh(), "sample refresh: %s", e.store->lastError().c_str());
            CHECK(e.store->list().size() == 3 && e.store->categories().size() >= 2, "three sample plugins");
            ps::Listing l;
            std::string err;
            CHECK(e.store->detail("log-to-file", l, err) && l.permissions == plugin::PermFilesystem && l.downloads.size() == 5 && !l.description.empty(), "detail of log-to-file: %s", err.c_str());
            auto r = e.store->install("tone-detector", true);
            std::string v;
#ifndef __linux__
            // the sample archives carry a stub library for Linux only: elsewhere the install must refuse with that reason
            CHECK(!r.ok && r.error.find("no library for this platform") != std::string::npos, "the sample plugin without a library for this platform is refused: %s", r.error.c_str());
            (void)v;
#else
            CHECK(r.ok && e.installed("tone-detector", &v) && v == "1.0.0", "a stub sample plugin installs: %s", r.error.c_str());
#endif
            // the same registry under a wrong key
            e.store.reset(new ps::Store(e.cfg(sample.string(), std::string(64, '2')), e.mgr.get()));
            CHECK(!e.store->refresh(), "the sample registry is refused under any other key");
        }
    }
    return testSummary("pstore_store");
}
