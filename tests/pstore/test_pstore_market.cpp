// The community marketplace client (core/src/pstore/market.cpp) against an in-process fake of the Worker API (tools/marketplace/server):
// signed answers and every way they can be wrong (tampered, wrong key, another request, replayed, from the future, another kind), browse,
// likes with the install id, install with the unreviewed confirmation, a wrong sha256 or size, a download that does not match its signed
// page, a non-https address, a package that differs from its page, wrong platform, too-new minimum version, a withdrawn plugin, downgrades,
// a failed update that is rolled back, withdrawn plugins switched off through the Store's revocations; the GitHub device-flow sign-in and its
// stored session; the local upload check, the multipart body and the upload and report requests. No network host is used.
#include "dect2/pstore/market.h"
#include "dect2/pstore/store.h"
#include "dect2/automation/json_mini.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/plugin/zip.h"
#include "../common/test_util.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>

using namespace dect2;
namespace fs = std::filesystem;
namespace aj = automation::json;
namespace ps = dect2::pstore;

namespace {
const uint8_t kSeed[32] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
const uint8_t kOtherSeed[32] = {42};
const int64_t kNow = 1800000000;
const char* kBase = "https://market.example";

std::string hexKey(const uint8_t* seed) { uint8_t pub[32]; meshcrypto::ed25519PublicKey(seed, pub); return ps::toHex(pub, 32); }
std::string sign(const uint8_t* seed, const std::string& d) { uint8_t s[64]; meshcrypto::ed25519Sign(seed, (const uint8_t*)d.data(), d.size(), s); return ps::toHex(s, 64); }
aj::Value strArr(const std::vector<std::string>& v) { aj::Value a = aj::Value::array(); for (const auto& s : v) a.push(aj::Value::string(s)); return a; }
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

std::vector<uint8_t> makePackage(const std::string& id, const std::string& ver, const std::vector<std::string>& perms, const std::string& lib = "demo.so", bool withLib = true,
                                 const std::string& kind = "action") {
    aj::Value m = aj::Value::object();
    m.set("id", aj::Value::string(id)); m.set("name", aj::Value::string("Demo")); m.set("version", aj::Value::string(ver)); m.set("author", aj::Value::string("t"));
    m.set("license", aj::Value::string("MIT")); m.set("kind", aj::Value::string(kind)); m.set("abi_version", aj::Value::number(1));
    m.set("library", aj::Value::string(lib)); m.set("permissions", strArr(perms));
    const std::string mt = aj::dump(m);
    const std::string body = "LIBRARY-BYTES-LIBRARY-BYTES-" + ver;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files{{"manifest.json", std::vector<uint8_t>(mt.begin(), mt.end())}};
    if (withLib) files.push_back({lib, std::vector<uint8_t>(body.begin(), body.end())});
    return plugin::zipStore(files);
}

struct Plug {
    std::string id = "demo", title = "Demo plugin", version = "1.0.0", kind = "action", minOnAir, status = "live", reason;
    std::vector<std::string> perms{"record"}, platforms{"macos", "linux", "windows"};
    std::vector<uint8_t> pkg;
    int64_t likes = 0, downloads = 0;
};

// the Worker API, as far as the client uses it
struct Fake {
    std::map<std::string, Plug> plugins;
    std::map<std::string, bool> liked;              // install id -> liked (one plugin is enough here)
    const uint8_t* seed = kSeed;
    std::string requestOverride, kindOverride, badSha, badUrl;
    int64_t timeShift = 0;
    bool tamper = false, downloadSizeLie = false, unsigned_ = false, expireSession = false;
    int calls = 0, pollsPending = 1;
    std::vector<ps::HttpCall> seen;

    std::string signedAnswer(const std::string& kind, const std::string& request, aj::Value payload) {
        payload.set("v", aj::Value::number(1)); payload.set("kind", aj::Value::string(kindOverride.empty() ? kind : kindOverride));
        payload.set("request", aj::Value::string(requestOverride.empty() ? request : requestOverride)); payload.set("time", aj::Value::number((double)(kNow + timeShift)));
        std::string inner = aj::dump(payload);
        const std::string sig = sign(seed, inner);
        if (tamper) { const size_t p = inner.find("Demo"); if (p != std::string::npos) inner[p] = 'X'; }
        aj::Value o = aj::Value::object();
        o.set("signed", aj::Value::string(inner)); o.set("sig", aj::Value::string(sig));
        if (unsigned_) return aj::dump(payload);
        return aj::dump(o);
    }
    aj::Value summary(const Plug& p) {
        aj::Value x = aj::Value::object();
        x.set("id", aj::Value::string(p.id)); x.set("title", aj::Value::string(p.title)); x.set("summary", aj::Value::string("Demo summary"));
        x.set("owner", aj::Value::string("alice")); x.set("version", aj::Value::string(p.version)); x.set("kind", aj::Value::string(p.kind));
        x.set("tags", strArr({"demo", "test"})); x.set("platforms", strArr(p.platforms)); x.set("likes", aj::Value::number((double)p.likes));
        x.set("downloads", aj::Value::number((double)p.downloads)); x.set("updated", aj::Value::number((double)kNow)); x.set("review", aj::Value::string("unreviewed"));
        return x;
    }
    ps::HttpReply reply(int st, const std::string& body) { ps::HttpReply r; r.status = st; r.body = body; return r; }
    ps::HttpReply operator()(const ps::HttpCall& c) {
        calls++;
        seen.push_back(c);
        if (c.url.compare(0, strlen(kBase), kBase) != 0) {   // a package file
            for (auto& kv : plugins) if (c.url == "https://files.example/" + kv.first + ".onairplugin") return reply(200, std::string(kv.second.pkg.begin(), kv.second.pkg.end()));
            return reply(404, "{}");
        }
        const std::string rq = c.url.substr(strlen(kBase));
        const std::string path = rq.substr(0, rq.find('?'));
        auto q = [&](const std::string& k) { const size_t p = rq.find(k + "="); if (p == std::string::npos) return std::string(); const size_t e = rq.find('&', p); return rq.substr(p + k.size() + 1, e == std::string::npos ? std::string::npos : e - p - k.size() - 1); };
        auto idOf = [&](const char* tail) { std::string s = path.substr(strlen("/api/v1/plugins/")); return tail ? s.substr(0, s.size() - strlen(tail)) : s; };
        if (c.method == "GET" && path == "/api/v1/plugins") {
            aj::Value pl = aj::Value::array(), pulled = aj::Value::array(), o = aj::Value::object();
            for (auto& kv : plugins) {
                if (kv.second.status == "live") pl.push(summary(kv.second));
                else { aj::Value r = aj::Value::object(); r.set("id", aj::Value::string(kv.first)); r.set("reason", aj::Value::string(kv.second.reason)); pulled.push(r); }
            }
            o.set("total", aj::Value::number((double)pl.a.size())); o.set("plugins", pl); o.set("pulled", pulled);
            return reply(200, signedAnswer("list", rq, o));
        }
        if (c.method == "GET" && path == "/api/v1/tags") {
            aj::Value t = aj::Value::array(), x = aj::Value::object(), o = aj::Value::object();
            x.set("tag", aj::Value::string("demo")); x.set("count", aj::Value::number(3)); t.push(x); o.set("tags", t);
            return reply(200, signedAnswer("tags", rq, o));
        }
        if (c.method == "GET" && path.compare(0, 16, "/api/v1/plugins/") == 0 && path.size() > 9 && path.substr(path.size() - 9) == "/download") {
            const std::string id = idOf("/download");
            auto it = plugins.find(id);
            if (it == plugins.end()) return reply(404, "{\"error\":\"No such plugin.\"}");
            if (it->second.status != "live") return reply(410, "{\"error\":\"This plugin was withdrawn.\"}");
            it->second.downloads++;
            aj::Value o = aj::Value::object();
            o.set("id", aj::Value::string(id)); o.set("version", aj::Value::string(q("version")));
            o.set("url", aj::Value::string(badUrl.empty() ? "https://files.example/" + id + ".onairplugin" : badUrl));
            o.set("size", aj::Value::number((double)it->second.pkg.size() + (downloadSizeLie ? 1 : 0)));
            o.set("sha256", aj::Value::string(ps::sha256Hex(it->second.pkg)));
            return reply(200, signedAnswer("download", rq, o));
        }
        if (c.method == "GET" && path.compare(0, 16, "/api/v1/plugins/") == 0) {
            const std::string id = idOf(nullptr);
            auto it = plugins.find(id);
            if (it == plugins.end()) return reply(404, "{\"error\":\"No such plugin.\"}");
            const Plug& p = it->second;
            aj::Value x = summary(p), vs = aj::Value::array(), v = aj::Value::object(), o = aj::Value::object();
            x.set("description", aj::Value::string("A longer description.")); x.set("repo", aj::Value::string("https://github.com/alice/demo"));
            x.set("license", aj::Value::string("MIT")); x.set("author", aj::Value::string("Alice")); x.set("permissions", strArr(p.perms));
            x.set("status", aj::Value::string(p.status)); x.set("pulled_reason", aj::Value::string(p.reason)); x.set("liked", aj::Value::boolean(liked[q("install")]));
            v.set("version", aj::Value::string(p.version)); v.set("size", aj::Value::number((double)p.pkg.size()));
            v.set("sha256", aj::Value::string(badSha.empty() ? ps::sha256Hex(p.pkg) : badSha)); v.set("min_onair", aj::Value::string(p.minOnAir));
            v.set("platforms", strArr(p.platforms)); v.set("permissions", strArr(p.perms));
            vs.push(v); x.set("versions", vs); o.set("plugin", x);
            return reply(200, signedAnswer("plugin", rq, o));
        }
        if (c.method == "POST" && (path.size() > 5 && (path.substr(path.size() - 5) == "/like" || path.substr(path.size() - 7) == "/unlike"))) {
            aj::Value b; std::string e;
            if (!aj::parse(c.body, b, e) || b.str("install").size() != 32) return reply(400, "{\"error\":\"A like needs the install id.\"}");
            const bool on = path.substr(path.size() - 5) == "/like";
            const std::string id = idOf(on ? "/like" : "/unlike");
            Plug& p = plugins[id];
            if (on != liked[b.str("install")]) p.likes += on ? 1 : -1;
            liked[b.str("install")] = on;
            return reply(200, "{\"id\":\"" + id + "\",\"likes\":" + std::to_string(p.likes) + ",\"liked\":" + (on ? "true" : "false") + "}");
        }
        if (c.method == "POST" && path == "/api/v1/auth/device")
            return reply(200, "{\"device_code\":\"dev-code-1234\",\"user_code\":\"WXYZ-9876\",\"verification_uri\":\"https://evil.example/login\",\"interval\":5,\"expires_in\":900}");
        if (c.method == "POST" && path == "/api/v1/auth/poll") {
            if (pollsPending > 0) { pollsPending--; return reply(200, "{\"status\":\"pending\"}"); }
            return reply(200, "{\"status\":\"ok\",\"token\":\"sess.token\",\"login\":\"alice\",\"expires\":" + std::to_string(kNow + 86400) + "}");
        }
        auto authed = [&]() { if (expireSession) return false; for (const auto& h : c.headers) if (h == "Authorization: Bearer sess.token") return true; return false; };
        if (c.method == "GET" && path == "/api/v1/me") {
            if (!authed()) return reply(401, "{\"error\":\"Sign in with GitHub first.\"}");
            return reply(200, "{\"login\":\"alice\",\"plugins\":[{\"id\":\"demo\",\"title\":\"Demo plugin\",\"version\":\"1.0.0\",\"status\":\"live\"}]}");
        }
        if (c.method == "POST" && (path == "/api/v1/plugins" || path == "/api/v1/plugins/demo/versions")) {
            if (!authed()) return reply(401, "{\"error\":\"Your sign-in has expired.\"}");
            return reply(path == "/api/v1/plugins" ? 201 : 200, "{\"ok\":true,\"id\":\"demo\",\"version\":\"2.0.0\"}");
        }
        if (c.method == "POST" && path == "/api/v1/plugins/demo/report") return authed() ? reply(201, "{\"ok\":true}") : reply(401, "{\"error\":\"Sign in\"}");
        return reply(404, "{\"error\":\"Not found.\"}");
    }
};

struct Env {
    fs::path root;
    Fake fake;
    std::unique_ptr<plugin::Manager> mgr;
    std::unique_ptr<ps::Market> m;
    std::string key = hexKey(kSeed), platform = "linux", host = "4.0.0";
    explicit Env(const std::string& name) {
        root = fs::temp_directory_path() / ("onair_market_" + name + "_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove_all(root);
        plugin::ManagerConfig mc; mc.pluginsDir = (root / "plugins").string(); mc.hostVersion = host;
        mgr.reset(new plugin::Manager(mc));
        mgr->scan();
        Plug p; p.pkg = makePackage("demo", "1.0.0", {"record"});
        fake.plugins["demo"] = p;
        open();
    }
    ~Env() { m.reset(); mgr.reset(); std::error_code ec; fs::remove_all(root, ec); }
    void open() {
        ps::MarketConfig c;
        c.baseUrl = std::string(kBase) + "/"; c.publicKeyHex = key; c.stateDir = (root / "plugins" / ".market").string(); c.platform = platform; c.hostVersion = host;
        c.http = [this](const ps::HttpCall& call) { return fake(call); };
        c.now = [] { return kNow; };
        m.reset(new ps::Market(c, mgr.get()));
    }
    bool installed(const std::string& id, std::string* ver = nullptr) { plugin::Entry e; const bool f = mgr->find(id, e) && e.valid; if (f && ver) *ver = e.m.version; return f; }
};
} // namespace

int main() {
    // helpers
    CHECK(ps::urlEncode("ft8 map/é&x") == "ft8%20map%2F%C3%A9%26x", "url encoding: %s", ps::urlEncode("ft8 map/é&x").c_str());
    {
        const std::vector<uint8_t> file{0, 1, 2, '\r', '\n', 255};
        const std::string b = ps::multipartBody({{"title", "T"}}, "package", "x.onairplugin", file, "BND");
        CHECK(b.rfind("--BND\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nT\r\n", 0) == 0, "multipart field");
        CHECK(has(b, "name=\"package\"; filename=\"x.onairplugin\"\r\nContent-Type: application/zip\r\n\r\n") && b.size() > 6 &&
              b.substr(b.size() - 9 - 6 - 2, 6) == std::string(file.begin(), file.end()) && b.substr(b.size() - 9) == "--BND--\r\n", "multipart file part is the exact bytes");
    }

    // 1. browse: signed list, tags, page, pulled ids
    {
        Env e("browse");
        std::vector<ps::MarketItem> items; int total = 0;
        ps::MarketQuery q; q.q = "demo plug"; q.sort = "likes"; q.tag = "demo";
        CHECK(e.m->search(q, items, total), "search: %s", e.m->lastError().c_str());
        CHECK(items.size() == 1 && total == 1 && items[0].id == "demo" && items[0].owner == "alice" && items[0].review == "unreviewed" && items[0].tags.size() == 2, "one unreviewed item");
        CHECK(has(e.fake.seen.back().url, "q=demo%20plug") && has(e.fake.seen.back().url, "sort=likes") && has(e.fake.seen.back().url, "tag=demo"), "query in the URL");
        std::vector<std::pair<std::string, int>> tags;
        CHECK(e.m->tags(tags) && tags.size() == 1 && tags[0].first == "demo" && tags[0].second == 3, "tags");
        ps::MarketItem it;
        CHECK(e.m->page("demo", it), "page: %s", e.m->lastError().c_str());
        CHECK(it.haveDetail && it.repo == "https://github.com/alice/demo" && it.versions.size() == 1 && it.permissions == plugin::PermRecord && !it.liked, "page fields");
        CHECK(e.m->incompatibility(it).empty(), "installable: %s", e.m->incompatibility(it).c_str());
        e.fake.plugins["gone"] = Plug{"gone", "Gone", "1.0.0", "action", "", "pulled", "malware"};
        CHECK(e.m->search(ps::MarketQuery{}, items, total) && e.m->pulled().size() == 1 && e.m->pulled()[0].id == "gone" && e.m->pulledInstalled().empty(), "pulled ids kept, none installed from here");
    }

    // 2. signed answers: every failure is refused
    {
        Env e("signed");
        std::vector<ps::MarketItem> items; int total = 0;
        auto refused = [&](const char* what, const char* msg) {
            const bool ok = e.m->search(ps::MarketQuery{}, items, total);
            CHECK(!ok && has(e.m->lastError(), msg), "%s refused (%s)", what, e.m->lastError().c_str());
        };
        e.fake.tamper = true; refused("tampered text", "signature"); e.fake.tamper = false;
        e.fake.seed = kOtherSeed; refused("other key", "signature"); e.fake.seed = kSeed;
        e.fake.requestOverride = "/api/v1/plugins?sort=relevance&page=1&limit=30&q=other"; refused("another request's answer", "another request"); e.fake.requestOverride.clear();
        e.fake.kindOverride = "download"; refused("another kind", "another kind"); e.fake.kindOverride.clear();
        e.fake.timeShift = -8 * 86400; refused("replayed old answer", "too old"); e.fake.timeShift = 2 * 86400; refused("answer from the future", "too old"); e.fake.timeShift = 0;
        e.fake.unsigned_ = true; refused("unsigned answer", "not signed"); e.fake.unsigned_ = false;
        CHECK(e.m->search(ps::MarketQuery{}, items, total), "good again");
        std::string inner, err;
        CHECK(!ps::openSignedAnswer("{\"error\":\"The marketplace has no signing key yet.\",\"code\":\"not_configured\"}", e.key, "list", "/x", kNow, inner, err) && has(err, "no signing key"), "server error text shown");
    }

    // 3. a build without the key contacts nothing
    {
        Env e("nokey");
        e.key = std::string(64, '0'); e.open();
        std::vector<ps::MarketItem> items; int total = 0;
        const int before = e.fake.calls;
        CHECK(!e.m->keyConfigured() && !e.m->search(ps::MarketQuery{}, items, total) && has(e.m->lastError(), "not set up"), "no key: not set up");
        int64_t likes = 0; ps::DeviceStart ds;
        CHECK(!e.m->setLike("demo", true, likes) && !e.m->signInStart(ds) && e.fake.calls == before, "no request at all without the key");
    }

    // 4. likes: the install id is random, 32 hex, kept, sent; once per install
    {
        Env e("likes");
        const std::string id1 = e.m->installId();
        CHECK(id1.size() == 32 && id1.find_first_not_of("0123456789abcdef") == std::string::npos, "install id is 32 hex");
        e.open();
        CHECK(e.m->installId() == id1, "install id kept across restarts");
        int64_t likes = 0;
        CHECK(e.m->setLike("demo", true, likes) && likes == 1, "liked");
        CHECK(has(e.fake.seen.back().body, id1.c_str()) && e.fake.seen.back().method == "POST", "install id in the like body");
        CHECK(e.m->setLike("demo", true, likes) && likes == 1, "liking twice counts once");
        ps::MarketItem it;
        CHECK(e.m->page("demo", it) && it.liked, "page says liked");
        CHECK(e.m->setLike("demo", false, likes) && likes == 0, "unliked");
    }

    // 5. install: the confirmation, then a good install
    {
        Env e("install");
        auto r = e.m->install("demo", false);
        CHECK(!r.ok && has(r.error, "not confirmed") && !e.installed("demo"), "refused without the unreviewed confirmation");
        r = e.m->install("demo", true);
        CHECK(r.ok && r.version == "1.0.0" && e.installed("demo"), "installed: %s", r.error.c_str());
        std::string v;
        CHECK(e.m->installedFromHere("demo", &v) && v == "1.0.0", "recorded as from the marketplace");
        CHECK(e.fake.plugins["demo"].downloads == 1, "download counted once");
        bool sawInstallId = false;
        for (const auto& c : e.fake.seen) if (has(c.url, "/download?format=json") && has(c.url, ("install=" + e.m->installId()).c_str())) sawInstallId = true;
        CHECK(sawInstallId, "the download request carries the install id");
        // update to 1.1.0, then a downgrade is refused
        e.fake.plugins["demo"].version = "1.1.0"; e.fake.plugins["demo"].pkg = makePackage("demo", "1.1.0", {"record"});
        r = e.m->install("demo", true);
        CHECK(r.ok && e.installed("demo", &v) && v == "1.1.0", "updated: %s", r.error.c_str());
        e.fake.plugins["demo"].version = "1.0.5"; e.fake.plugins["demo"].pkg = makePackage("demo", "1.0.5", {"record"});
        r = e.m->install("demo", true);
        CHECK(!r.ok && has(r.error, "downgrade") && e.installed("demo", &v) && v == "1.1.0", "downgrade refused");
        // a broken update with a valid hash: the old version stays
        e.fake.plugins["demo"].version = "1.2.0";
        auto bad = makePackage("demo", "1.2.0", {"record"}, "demo.so", false);   // names a library it does not contain
        e.fake.plugins["demo"].pkg = bad;
        r = e.m->install("demo", true);
        CHECK(!r.ok && e.installed("demo", &v) && v == "1.1.0", "broken update rolled back (%s)", r.error.c_str());
        // withdrawn: the Store's revocation list switches it off, only because it came from here
        e.fake.plugins["demo"].status = "pulled"; e.fake.plugins["demo"].reason = "malware";
        std::vector<ps::MarketItem> items; int total = 0;
        CHECK(e.m->search(ps::MarketQuery{}, items, total) && items.empty(), "withdrawn plugin not listed");
        const auto pi = e.m->pulledInstalled();
        CHECK(pi.size() == 1 && pi[0].id == "demo" && has(pi[0].reason, "malware"), "withdrawn and installed from here");
        ps::Config sc; sc.registryUrl = (e.root / "noreg").string(); sc.publicKeyHex = hexKey(kSeed); sc.cacheDir = (e.root / "plugins" / ".store").string(); sc.hostVersion = "4.0.0";
        ps::Store store(sc, e.mgr.get());
        store.setExtraRevocations(pi);
        const auto hit = store.applyRevocations();
        plugin::Entry en;
        CHECK(hit.size() == 1 && e.mgr->find("demo", en) && en.revoked && has(en.error, "malware"), "switched off by the kill switch");
        std::string err;
        CHECK(!e.mgr->setEnabled("demo", true, err), "cannot be turned on");
        r = e.m->install("demo", true);
        CHECK(!r.ok && has(r.error, "withdrawn"), "a withdrawn plugin cannot be installed");
        store.setExtraRevocations({});
        store.applyRevocations();
        CHECK(e.mgr->find("demo", en) && !en.revoked, "lifted when the marketplace restores it");
    }

    // 6. install refusals: nothing is written
    {
        struct Case { const char* what; std::function<void(Env&)> set; const char* msg; };
        const std::vector<Case> cases = {
            {"wrong sha256 in the page", [](Env& e) { e.fake.badSha = std::string(64, 'a'); }, "does not match"},
            {"download size lie", [](Env& e) { e.fake.downloadSizeLie = true; }, "does not match the plugin's signed page"},
            {"plain http address", [](Env& e) { e.fake.badUrl = "http://files.example/demo.onairplugin"; }, "not https"},
            {"package of another plugin", [](Env& e) { e.fake.plugins["demo"].pkg = makePackage("other", "1.0.0", {"record"}); }, "the package is other"},
            {"package asks for more", [](Env& e) { e.fake.plugins["demo"].pkg = makePackage("demo", "1.0.0", {"record", "spawn-process"}); }, "permissions"},
            {"package of another kind", [](Env& e) { e.fake.plugins["demo"].pkg = makePackage("demo", "1.0.0", {"record"}, "demo.so", true, "panel-lite"); }, "kind"},
            {"other platform", [](Env& e) { e.fake.plugins["demo"].platforms = {"windows"}; }, "not available for linux"},
            {"too-new minimum", [](Env& e) { e.fake.plugins["demo"].minOnAir = "9.0"; }, "needs OnAir 9.0"},
            {"unknown permission", [](Env& e) { e.fake.plugins["demo"].perms = {"record", "root"}; }, "does not know"},
            {"withdrawn", [](Env& e) { e.fake.plugins["demo"].status = "pulled"; }, "withdrawn"},
            {"tampered page", [](Env& e) { e.fake.tamper = true; }, "signature"},
            {"no such plugin", [](Env& e) { e.fake.plugins.clear(); }, "No such plugin"},
        };
        for (const auto& c : cases) {
            Env e(std::string("refuse"));
            c.set(e);
            const auto r = e.m->install("demo", true);
            CHECK(!r.ok && has(r.error, c.msg) && !e.installed("demo") && !e.m->installedFromHere("demo"), "%s: refused (%s)", c.what, r.error.c_str());
            CHECK(!fs::exists(e.root / "plugins" / "demo"), "%s: nothing written", c.what);
        }
    }

    // 7. GitHub sign-in through the server; the session is kept privately and dropped on 401
    {
        Env e("signin");
        CHECK(!e.m->signedIn(), "not signed in at first");
        std::vector<ps::MarketItem> mine;
        CHECK(!e.m->mine(mine) && has(e.m->lastError(), "Sign in"), "me needs a sign-in");
        ps::DeviceStart ds;
        CHECK(e.m->signInStart(ds) && ds.userCode == "WXYZ-9876" && ds.deviceCode == "dev-code-1234", "device code");
        CHECK(ds.verificationUri == "https://github.com/login/device", "the address shown is always GitHub's (%s)", ds.verificationUri.c_str());
        CHECK(e.m->signInPoll(ds.deviceCode) == ps::SignInState::Pending, "pending");
        CHECK(e.m->signInPoll(ds.deviceCode) == ps::SignInState::Ok && e.m->signedIn() && e.m->login() == "alice", "signed in");
        const fs::path sess = e.root / "plugins" / ".market" / "session.json";
        CHECK(fs::exists(sess), "session kept");
#ifndef _WIN32
        CHECK((fs::status(sess).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none, "session file is private");
#endif
        e.open();
        CHECK(e.m->signedIn() && e.m->login() == "alice", "session survives a restart");
        CHECK(e.m->mine(mine) && mine.size() == 1 && mine[0].id == "demo", "my plugins");
        e.m->signOut();
        CHECK(!e.m->signedIn() && !fs::exists(sess), "signed out, file gone");
    }

    // 8. upload: local check, multipart, auth, new version, report
    {
        Env e("upload");
        auto pv = ps::Market::preview(makePackage("demo", "2.0.0", {"network"}));
        CHECK(pv.ok && pv.manifest.id == "demo" && pv.platforms.size() == 3 && pv.manifest.permissions == plugin::PermNetwork, "preview: %s", pv.error.c_str());
        CHECK(!ps::Market::preview(makePackage("demo", "2.0.0", {}, "demo.so", false)).ok, "preview: missing library");
        CHECK(has(ps::Market::preview(std::vector<uint8_t>(100, 'x')).error, "not an .onairplugin"), "preview: not a zip");
        CHECK(has(ps::Market::preview(makePackage("Demo!", "2.0.0", {})).error, "\"id\""), "preview: bad manifest");
        auto m2 = ps::Market::preview(plugin::zipStore({{"x.txt", {1, 2}}}));
        CHECK(has(m2.error, "no manifest"), "preview: no manifest");
        ps::UploadForm f; f.package = makePackage("demo", "2.0.0", {"network"}); f.title = "Demo"; f.description = "A demo plugin for the tests."; f.tags = "demo, test";
        std::string id, ver;
        CHECK(!e.m->upload(f, id, ver) && has(e.m->lastError(), "Sign in"), "upload needs a sign-in");
        ps::DeviceStart ds; e.fake.pollsPending = 0;
        CHECK(e.m->signInStart(ds) && e.m->signInPoll(ds.deviceCode) == ps::SignInState::Ok, "signed in");
        ps::UploadForm nt = f; nt.title.clear();
        CHECK(!e.m->upload(nt, id, ver) && has(e.m->lastError(), "title"), "title required");
        CHECK(e.m->upload(f, id, ver) && id == "demo" && ver == "2.0.0", "uploaded: %s", e.m->lastError().c_str());
        const ps::HttpCall& c = e.fake.seen.back();
        const std::string bnd = c.contentType.substr(c.contentType.find("boundary=") + 9);
        CHECK(c.method == "POST" && c.url == std::string(kBase) + "/api/v1/plugins" && c.contentType.compare(0, 30, "multipart/form-data; boundary=") == 0 && bnd.size() > 16, "multipart POST");
        CHECK(std::find(c.headers.begin(), c.headers.end(), "Authorization: Bearer sess.token") != c.headers.end(), "bearer token sent");
        CHECK(has(c.body, "name=\"title\"\r\n\r\nDemo\r\n") && has(c.body, "name=\"tags\"\r\n\r\ndemo, test\r\n"), "fields in the body");
        CHECK(c.body.find(std::string(f.package.begin(), f.package.end())) != std::string::npos, "the exact package bytes");
        ps::UploadForm nv; nv.package = f.package; nv.newVersionOf = "demo";
        CHECK(e.m->upload(nv, id, ver) && e.fake.seen.back().url == std::string(kBase) + "/api/v1/plugins/demo/versions", "new version path");
        nv.newVersionOf = "other";
        CHECK(!e.m->upload(nv, id, ver) && has(e.m->lastError(), "not other"), "new version of the wrong id refused locally");
        CHECK(e.m->report("demo", "malware", "It \"opens\" a shell.") && has(e.fake.seen.back().body, "\\\"opens\\\"") && has(e.fake.seen.back().body, "\"reason\":\"malware\""), "report");
        // the server says the session is no longer valid: it is dropped
        e.fake.expireSession = true;
        std::vector<ps::MarketItem> mine;
        CHECK(!e.m->mine(mine) && !e.m->signedIn() && !fs::exists(e.root / "plugins" / ".market" / "session.json"), "a 401 drops the session");
        CHECK(!e.m->report("demo", "spam", "") && has(e.m->lastError(), "Sign in"), "report needs a sign-in");
    }
    return testSummary("pstore_market");
}
