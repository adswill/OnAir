// The plugin store client (see core/include/dect2/pstore/store.h and docs/PLUGIN-MARKETPLACE-3.9.md).
#include "dect2/pstore/store.h"
#include "dect2/automation/json_mini.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/plugin/zip.h"
#include "dect2/tv/iptv.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

namespace dect2::pstore {
namespace fs = std::filesystem;
namespace aj = automation::json;

// All zeros: the project's registry key has not been made yet. The owner makes it with tools/marketplace/onair_registry.py keygen.
const char* const kBuiltInRegistryKeyHex = "0000000000000000000000000000000000000000000000000000000000000000";
const char* const kDefaultRegistryUrl = "https://adswill.github.io/onair-plugins/";

namespace {
constexpr uint32_t kUnknownPermission = 0x80000000u;

std::string readText(const fs::path& p, size_t maxBytes) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.size() > maxBytes ? std::string() : s;
}
bool writeAtomic(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path t = p.string() + ".tmp";
    { std::ofstream f(t, std::ios::binary | std::ios::trunc); if (!f) return false; f << data; if (!f) return false; }
    fs::rename(t, p, ec);
    return !ec;
}
bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
bool isHex(const std::string& s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}
std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }
// a relative path inside the registry: no "..", no absolute path, no backslash, no scheme
bool safeRel(const std::string& r) {
    if (r.empty() || r.size() > 300 || r[0] == '/' || r.find('\\') != std::string::npos || r.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= r.size()) {
        size_t j = r.find('/', i);
        if (j == std::string::npos) j = r.size();
        const std::string part = r.substr(i, j - i);
        if (part.empty() || part == "." || part == "..") return false;
        i = j + 1;
    }
    return true;
}
std::string joinUrl(const std::string& base, const std::string& rel) {
    std::string b = base;
    if (!b.empty() && b.back() != '/' && b.back() != '\\') b += '/';
    return b + rel;
}
std::string nowIso() {
    const time_t t = time(nullptr); struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[32]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}
std::vector<std::string> strList(const aj::Value* v, size_t maxItems, size_t maxLen) {
    std::vector<std::string> o;
    if (v && v->type == aj::Value::Arr) for (const auto& x : v->a) if (x.type == aj::Value::Str && o.size() < maxItems) o.push_back(x.s.substr(0, maxLen));
    return o;
}
uint32_t permMask(const aj::Value* v) {
    uint32_t m = 0;
    if (!v || v->type != aj::Value::Arr) return 0;
    for (const auto& x : v->a) {
        uint32_t bit = 0;
        for (uint32_t b = 1; b <= plugin::PermSpawn; b <<= 1) if (x.type == aj::Value::Str && x.s == plugin::permissionName(b)) bit = b;
        m |= bit ? bit : kUnknownPermission;
    }
    return m;
}
bool validReview(const std::string& s) { return s == "listed" || s == "reviewed" || s == "verified"; }

// a local file or folder registry: path or file:// URL
std::string localPath(const std::string& u) {
    if (startsWith(u, "file://")) {
        std::string p = u.substr(7);
#ifdef _WIN32
        if (p.size() > 2 && p[0] == '/' && p[2] == ':') p = p.substr(1);
#endif
        return p;
    }
    return u;
}
} // namespace

const char* reviewName(Review r) { return r == Review::Verified ? "verified" : r == Review::Reviewed ? "reviewed" : "listed"; }
bool revocationMatches(const std::string& spec, const std::string& version) { return plugin::versionSpecMatches(spec, version); }

std::string toHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
bool fromHex(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    if (s.size() % 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i += 2) { const int a = v(s[i]), b = v(s[i + 1]); if (a < 0 || b < 0) return false; out.push_back((uint8_t)(a * 16 + b)); }
    return true;
}
std::string sha256Hex(const std::vector<uint8_t>& d) {
    uint8_t h[32];
    meshcrypto::sha256(d.data(), d.size(), h);
    return toHex(h, 32);
}
bool verifyIndexSignature(const std::string& indexBytes, const std::string& sigHex, const std::string& pubKeyHex) {
    std::string sh = sigHex;
    while (!sh.empty() && (sh.back() == '\n' || sh.back() == '\r' || sh.back() == ' ')) sh.pop_back();
    std::vector<uint8_t> sig, pub;
    if (!isHex(sh, 128) || !isHex(pubKeyHex, 64) || !fromHex(sh, sig) || !fromHex(pubKeyHex, pub)) return false;
    return meshcrypto::ed25519Verify(sig.data(), (const uint8_t*)indexBytes.data(), indexBytes.size(), pub.data());
}

std::string currentPlatform() {
#if defined(_WIN32)
    return "windows-x64";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#else
    return "macos-x86_64";
#endif
#elif defined(__aarch64__)
    return "linux-aarch64";
#else
    return "linux-x86_64";
#endif
}

Fetcher defaultFetcher(const std::string& userAgent) {
    return [userAgent](const std::string& url, size_t maxBytes, std::vector<uint8_t>& out, std::string& err, const std::function<void(uint64_t, uint64_t)>& progress) {
        out.clear();
        if (startsWith(lower(url), "https://")) {
            iptv::HttpOptions o;
            o.userAgent = userAgent;
            iptv::HttpResult r;
            auto b = iptv::httpOpen(url, o, r);
            if (!b) { err = r.error.empty() ? "no response" : r.error; return false; }
            if (r.status != 0 && (r.status < 200 || r.status > 299)) { err = "HTTP " + std::to_string(r.status); return false; }
            if (r.contentLength > 0 && (uint64_t)r.contentLength > maxBytes) { err = "the file is larger than allowed"; return false; }
            uint8_t buf[16384];
            for (;;) {
                const long k = b->read(buf, sizeof buf);
                if (k == 0) break;
                if (k < 0) { err = "the download stopped (timeout or connection lost)"; return false; }
                out.insert(out.end(), buf, buf + k);
                if (out.size() > maxBytes) { err = "the file is larger than allowed"; return false; }
                if (progress) progress(out.size(), r.contentLength > 0 ? (uint64_t)r.contentLength : 0);
            }
            return true;
        }
        if (startsWith(lower(url), "http://")) { err = "plain http:// is not used: the registry must be https:// (or a local folder)"; return false; }
        std::ifstream f(localPath(url), std::ios::binary);
        if (!f) { err = "cannot open " + url; return false; }
        std::error_code ec;
        const auto sz = fs::file_size(localPath(url), ec);
        if (!ec && sz > maxBytes) { err = "the file is larger than allowed"; return false; }
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (out.size() > maxBytes) { err = "the file is larger than allowed"; return false; }
        if (progress) progress(out.size(), out.size());
        return true;
    };
}

// ---------------------------------------------------------------- Store
struct Store::Impl {
    Config cfg;
    plugin::Manager* mgr = nullptr;
    mutable std::mutex mu;
    std::vector<Listing> items;
    std::vector<Revocation> revoked;
    Source source = Source::None;
    int serial = 0, maxSerial = 0;
    std::string generated, notice, error;
    std::map<std::string, Installed> installed;

    fs::path cachePath(const char* f) const { return fs::path(cfg.cacheDir) / f; }
    std::string base() const { return cfg.registryUrl; }

    void loadState() {
        aj::Value v; std::string e;
        const std::string t = readText(cachePath("store-state.json"), 1 << 20);
        if (t.empty() || !aj::parse(t, v, e) || v.type != aj::Value::Obj) return;
        maxSerial = (int)v.num("max_serial");
        if (const aj::Value* in = v.get("installed")) if (in->type == aj::Value::Obj)
            for (const auto& kv : in->o) installed[kv.first] = Installed{kv.second.str("version"), kv.second.str("review"), kv.second.str("installed_at")};
    }
    void saveState() {
        aj::Value root = aj::Value::object(), in = aj::Value::object();
        root.set("max_serial", aj::Value::number(maxSerial));
        for (const auto& kv : installed) {
            aj::Value o = aj::Value::object();
            o.set("version", aj::Value::string(kv.second.version)); o.set("review", aj::Value::string(kv.second.review)); o.set("installed_at", aj::Value::string(kv.second.installedAt));
            in.set(kv.first, o);
        }
        root.set("installed", in);
        writeAtomic(cachePath("store-state.json"), aj::dump(root, true));
    }

    bool parseIndex(const std::string& text, std::vector<Listing>& items_, std::vector<Revocation>& rev, int& serial_, std::string& gen, std::string& err) const {
        aj::Value root; std::string pe;
        if (!aj::parse(text, root, pe) || root.type != aj::Value::Obj) { err = "the plugin list is not valid JSON (" + pe + ")"; return false; }
        if ((int)root.num("schema") != kSchema) { err = "the plugin list has a schema this OnAir does not know (" + std::to_string((int)root.num("schema")) + ")"; return false; }
        serial_ = (int)root.num("serial");
        if (serial_ < 1) { err = "the plugin list has no serial number"; return false; }
        gen = root.str("generated").substr(0, 40);
        const aj::Value* pl = root.get("plugins");
        if (!pl || pl->type != aj::Value::Arr) { err = "the plugin list has no plugins array"; return false; }
        std::set<std::string> seen;
        for (const auto& x : pl->a) {
            if (x.type != aj::Value::Obj || items_.size() >= 5000) continue;
            Listing l;
            l.id = x.str("id"); l.name = x.str("name").substr(0, 120); l.summary = x.str("summary").substr(0, 300);
            l.version = x.str("version").substr(0, 40); l.minOnAir = x.str("min_onair").substr(0, 40); l.kind = x.str("kind").substr(0, 20);
            l.license = x.str("license").substr(0, 80); l.author = x.str("author").substr(0, 120);
            l.review = validReview(x.str("review")) ? x.str("review") : "listed";
            l.abi = (uint32_t)x.num("abi_version");
            l.permissions = permMask(x.get("permissions"));
            l.categories = strList(x.get("categories"), 8, 40); l.tags = strList(x.get("tags"), 16, 40); l.platforms = strList(x.get("platforms"), 8, 24);
            if (const aj::Value* m = x.get("manifest")) { l.manifestPath = m->str("path"); l.manifestSha256 = lower(m->str("sha256")); }
            if (!plugin::validId(l.id) || l.name.empty() || l.version.empty() || l.license.empty() || !safeRel(l.manifestPath) || !isHex(l.manifestSha256, 64)) continue;   // a bad entry is skipped, the rest stays
            if (!seen.insert(l.id).second) continue;
            items_.push_back(std::move(l));
        }
        if (const aj::Value* rv = root.get("revoked")) if (rv->type == aj::Value::Arr)
            for (const auto& x : rv->a) if (x.type == aj::Value::Obj && plugin::validId(x.str("id")) && rev.size() < 1000) rev.push_back({x.str("id"), x.str("version", "*").substr(0, 40), x.str("reason").substr(0, 500)});
        return true;
    }

    // verifies and takes over an index; false with err when it is bad
    bool adopt(const std::string& text, const std::string& sig, Source src, std::string& err) {
        if (!verifyIndexSignature(text, sig, cfg.publicKeyHex)) { err = "The plugin list failed its signature check and was ignored."; return false; }
        std::vector<Listing> it; std::vector<Revocation> rev; int ser = 0; std::string gen;
        if (!parseIndex(text, it, rev, ser, gen, err)) return false;
        if (src == Source::Network && ser < maxSerial) { err = "The plugin list is older than the one already seen (serial " + std::to_string(ser) + " < " + std::to_string(maxSerial) + "): ignored."; return false; }
        std::lock_guard<std::mutex> lk(mu);
        items = std::move(it); revoked = std::move(rev); serial = ser; generated = gen; source = src;
        if (ser > maxSerial) { maxSerial = ser; }
        return true;
    }
    std::string incompat(const Listing& l) const {
        if (!l.platforms.empty() && std::find(l.platforms.begin(), l.platforms.end(), platform()) == l.platforms.end()) return "not available for " + platform();
        if (l.abi != cfg.hostAbi) return "needs plugin interface version " + std::to_string(l.abi) + " (this OnAir speaks " + std::to_string(cfg.hostAbi) + ")";
        if (l.permissions & kUnknownPermission) return "asks for a permission this OnAir does not know";
        if (!l.minOnAir.empty() && !cfg.hostVersion.empty() && plugin::compareVersions(cfg.hostVersion, l.minOnAir) < 0) return "needs OnAir " + l.minOnAir + " or newer";
        for (const auto& r : revoked) if (r.id == l.id && plugin::versionSpecMatches(r.version, l.version)) return "withdrawn by the registry" + (r.reason.empty() ? std::string() : ": " + r.reason);
        return {};
    }
    std::string platform() const { return cfg.platform.empty() ? currentPlatform() : cfg.platform; }
};

Store::Store(Config c, plugin::Manager* mgr) : p_(new Impl) {
    if (!c.fetch) c.fetch = defaultFetcher("OnAir-Store/" + c.hostVersion);
    p_->cfg = std::move(c);
    p_->mgr = mgr;
    p_->loadState();
}
Store::~Store() { delete p_; }

bool Store::signatureKeyConfigured() const { return p_->cfg.publicKeyHex.find_first_not_of('0') != std::string::npos; }

bool Store::loadCache() {
    const std::string text = readText(p_->cachePath("index.json"), kMaxIndexBytes), sig = readText(p_->cachePath("index.json.sig"), 1024);
    std::string err;
    if (text.empty() || !p_->adopt(text, sig, Source::Cache, err)) return false;
    if (p_->cfg.autoRevocations) applyRevocations();
    return true;
}

bool Store::refresh() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->error.clear(); p_->notice.clear();
    }
    auto fallBack = [&](const std::string& why) {
        bool had;
        { std::lock_guard<std::mutex> lk(p_->mu); p_->error = why; had = !p_->items.empty(); }
        if (!had) loadCache();
        std::lock_guard<std::mutex> lk(p_->mu);
        if (!p_->items.empty()) {
            p_->source = Source::Cache;
            p_->notice = "Showing the list saved on " + (p_->generated.empty() ? std::string("an earlier visit") : p_->generated.substr(0, 10)) + ". " + why;
        }
        return false;
    };
    if (!signatureKeyConfigured()) return fallBack("This registry has no signing key set, so its plugin list cannot be trusted (the project's key is not published yet).");
    if (p_->cfg.registryUrl.empty()) return fallBack("No registry address is set.");
    std::vector<uint8_t> idx, sig;
    std::string err;
    if (!p_->cfg.fetch(joinUrl(p_->base(), "index.json"), kMaxIndexBytes, idx, err, {})) return fallBack("Could not reach the registry (" + err + ").");
    if (!p_->cfg.fetch(joinUrl(p_->base(), "index.json.sig"), 1024, sig, err, {})) return fallBack("Could not read the list's signature (" + err + ").");
    const std::string text(idx.begin(), idx.end()), sigText(sig.begin(), sig.end());
    if (!p_->adopt(text, sigText, Source::Network, err)) return fallBack(err);
    writeAtomic(p_->cachePath("index.json"), text);
    writeAtomic(p_->cachePath("index.json.sig"), sigText);
    { std::lock_guard<std::mutex> lk(p_->mu); p_->saveState(); }
    if (p_->cfg.autoRevocations) applyRevocations();
    return true;
}

std::string Store::lastError() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->error; }
std::string Store::notice() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->notice; }
Source Store::source() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->source; }
int Store::serial() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->serial; }
std::string Store::generated() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->generated; }
std::vector<Revocation> Store::revocations() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->revoked; }
std::string Store::incompatibility(const Listing& l) const { std::lock_guard<std::mutex> lk(p_->mu); return p_->incompat(l); }

std::vector<Listing> Store::list(const std::string& query, const std::string& category) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Listing> out;
    const std::string q = lower(query), cat = lower(category);
    for (const auto& l : p_->items) {
        if (!p_->incompat(l).empty()) continue;
        if (!cat.empty() && std::none_of(l.categories.begin(), l.categories.end(), [&](const std::string& c) { return lower(c) == cat; })) continue;
        if (!q.empty()) {
            std::string hay = lower(l.id + " " + l.name + " " + l.summary + " " + l.author);
            for (const auto& t : l.tags) hay += " " + lower(t);
            if (hay.find(q) == std::string::npos) continue;
        }
        out.push_back(l);
    }
    std::sort(out.begin(), out.end(), [](const Listing& a, const Listing& b) { return lower(a.name) < lower(b.name); });
    return out;
}
std::vector<std::string> Store::categories() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::set<std::string> s;
    for (const auto& l : p_->items) if (p_->incompat(l).empty()) for (const auto& c : l.categories) s.insert(c);
    return std::vector<std::string>(s.begin(), s.end());
}
size_t Store::hiddenCount() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    size_t n = 0;
    for (const auto& l : p_->items) if (!p_->incompat(l).empty()) n++;
    return n;
}
bool Store::find(const std::string& id, Listing& out) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const auto& l : p_->items) if (l.id == id) { out = l; return true; }
    return false;
}

bool Store::detail(const std::string& id, Listing& out, std::string& err) {
    Listing l;
    if (!find(id, l)) { err = "no such plugin in the list"; return false; }
    if (l.haveDetail) { out = l; return true; }
    std::vector<uint8_t> b;
    if (!p_->cfg.fetch(joinUrl(p_->base(), l.manifestPath), kMaxDetailBytes, b, err, {})) { err = "could not fetch the plugin's page (" + err + ")"; return false; }
    if (sha256Hex(b) != l.manifestSha256) { err = "the plugin's page does not match the signed list (sha256): refused"; return false; }
    aj::Value v; std::string pe;
    if (!aj::parse(std::string(b.begin(), b.end()), v, pe) || v.type != aj::Value::Obj) { err = "the plugin's page is not valid JSON"; return false; }
    if (v.str("id") != l.id || v.str("version") != l.version) { err = "the plugin's page names a different plugin or version than the list: refused"; return false; }
    l.description = v.str("description").substr(0, 4000);
    if (!v.str("author").empty()) l.author = v.str("author").substr(0, 120);
    l.homepage = v.str("homepage").substr(0, 300); l.source = v.str("source").substr(0, 300);
    l.screenshots = strList(v.get("screenshots"), 6, 300);
    l.downloads.clear();
    if (const aj::Value* d = v.get("downloads")) if (d->type == aj::Value::Obj)
        for (const auto& kv : d->o) {
            Download dl;
            dl.url = kv.second.str("url").substr(0, 400); dl.size = (uint64_t)std::max(0.0, kv.second.num("size")); dl.sha256 = lower(kv.second.str("sha256"));
            if (dl.url.empty() || !isHex(dl.sha256, 64) || dl.size == 0 || dl.size > kMaxPackageBytes) continue;
            l.downloads[kv.first] = dl;
        }
    l.haveDetail = true;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& x : p_->items) if (x.id == id) x = l;
    }
    out = l;
    return true;
}

namespace {
// finds manifest.json in the package (top level or inside one folder) and parses it
bool inspectPackage(const std::vector<uint8_t>& zip, const std::string& hostVersion, plugin::Manifest& m, std::string& err) {
    plugin::ZipReader z;
    if (!z.open(zip, err)) return false;
    const plugin::ZipEntry* mf = nullptr;
    for (const auto& e : z.entries()) {
        if (e.name == "manifest.json") { mf = &e; break; }
        const size_t s = e.name.find('/');
        if (s != std::string::npos && e.name.substr(s) == "/manifest.json") mf = &e;
    }
    if (!mf) { err = "the package has no manifest.json"; return false; }
    std::vector<uint8_t> mb;
    if (!z.read(*mf, mb, err)) return false;
    return plugin::parseManifest(std::string(mb.begin(), mb.end()), m, err, hostVersion);
}
bool copyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive, ec);
    return !ec;
}
} // namespace

Result Store::install(const std::string& id, bool confirmedNative, const std::function<void(uint64_t, uint64_t)>& progress) {
    Result r;
    if (!p_->mgr) { r.error = "no plugin manager"; return r; }
    Listing l;
    if (!find(id, l)) { r.error = "no such plugin in the list"; return r; }
    const std::string why = incompatibility(l);
    if (!why.empty()) { r.error = "Not installed: " + why + "."; return r; }
    if (!confirmedNative) { r.error = "Not installed: the native-code warning was not confirmed."; return r; }
    plugin::Entry cur;
    const bool have = p_->mgr->find(id, cur);
    if (have && cur.bundled) { r.error = "Not installed: a bundled example with this id exists."; return r; }
    if (have && !cur.m.version.empty() && plugin::compareVersions(l.version, cur.m.version) < 0) {
        r.error = "Not installed: version " + l.version + " is older than the installed " + cur.m.version + " (downgrades are refused)."; return r;
    }
    std::string err;
    if (!detail(id, l, err)) { r.error = "Not installed: " + err; return r; }
    auto dit = l.downloads.find(p_->platform());
    if (dit == l.downloads.end()) { r.error = "Not installed: no download for " + p_->platform() + "."; return r; }
    const Download dl = dit->second;
    std::string url = dl.url;
    if (lower(url).compare(0, 8, "https://") != 0) {
        if (!safeRel(url)) { r.error = "Not installed: the download address is not allowed."; return r; }
        url = joinUrl(p_->base(), url);
    }
    std::vector<uint8_t> pkg;
    if (!p_->cfg.fetch(url, (size_t)std::min<uint64_t>(dl.size, kMaxPackageBytes) + 1, pkg, err, progress)) { r.error = "Not installed: download failed (" + err + ")."; return r; }
    if (pkg.size() != dl.size) { r.error = "Not installed: the download has the wrong size (" + std::to_string(pkg.size()) + " instead of " + std::to_string(dl.size) + " bytes)."; return r; }
    if (sha256Hex(pkg) != dl.sha256) { r.error = "Not installed: the download does not match its sha256 in the list. Nothing was written."; return r; }
    plugin::Manifest m;
    if (!inspectPackage(pkg, p_->cfg.hostVersion, m, err)) { r.error = "Not installed: " + err + "."; return r; }
    if (m.id != l.id || m.version != l.version) { r.error = "Not installed: the package is " + m.id + " " + m.version + ", not " + l.id + " " + l.version + "."; return r; }
    if (m.abi != p_->cfg.hostAbi) { r.error = "Not installed: the package speaks a different plugin interface version."; return r; }
    if (m.kind != l.kind && !l.kind.empty()) { r.error = "Not installed: the package's kind differs from the listing."; return r; }
    if (m.permissions & ~l.permissions) { r.error = "Not installed: the package asks for permissions that the listing does not show."; return r; }
    // keep the old version until the new one is in place and checked
    const fs::path live = fs::path(p_->mgr->pluginsDir()) / id, backup = p_->cachePath("rollback") / id;
    const bool backedUp = have && fs::exists(live) && copyTree(live, backup);
    std::string installedId;
    auto rollBack = [&](const std::string& reason) {
        std::error_code ec;
        if (backedUp) { fs::remove_all(live, ec); copyTree(backup, live); p_->mgr->scan(); }
        r.error = "Not installed: " + reason + (backedUp ? " The previous version was kept." : "");
        return r;
    };
    if (!p_->mgr->installBytes(pkg, installedId, err)) return rollBack(err + ".");
    plugin::Entry now;
    if (!p_->mgr->find(id, now) || !now.valid || now.m.version != l.version) return rollBack("the installed plugin did not check out.");
    { std::error_code ec; fs::remove_all(backup, ec); }
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->installed[id] = Installed{l.version, l.review, nowIso()};
        p_->saveState();
    }
    if (p_->cfg.autoRevocations) applyRevocations();
    r.ok = true; r.version = l.version;
    return r;
}

Result Store::uninstall(const std::string& id) {
    Result r;
    if (!p_->mgr) { r.error = "no plugin manager"; return r; }
    std::string err;
    if (!p_->mgr->remove(id, err)) { r.error = err; return r; }
    { std::lock_guard<std::mutex> lk(p_->mu); p_->installed.erase(id); p_->saveState(); }
    r.ok = true;
    return r;
}

std::vector<UpdateInfo> Store::updates() const {
    std::vector<UpdateInfo> u;
    if (!p_->mgr) return u;
    for (const auto& e : p_->mgr->entries()) {
        if (e.bundled || e.m.version.empty()) continue;
        Listing l;
        if (!find(e.m.id, l) || !incompatibility(l).empty()) continue;
        if (plugin::compareVersions(l.version, e.m.version) > 0) u.push_back({e.m.id, e.m.version, l.version});
    }
    return u;
}

std::vector<Revocation> Store::applyRevocations() {
    std::vector<Revocation> hit;
    if (!p_->mgr) return hit;
    std::vector<Revocation> rev = revocations();
    std::map<std::string, std::pair<std::string, std::string>> want;
    const auto entries = p_->mgr->entries();
    for (const auto& r : rev) {
        auto it = want.find(r.id);
        bool matchesInstalled = false;
        for (const auto& e : entries) if (e.m.id == r.id && !e.m.version.empty() && plugin::versionSpecMatches(r.version, e.m.version)) matchesInstalled = true;
        if (it == want.end() || matchesInstalled) want[r.id] = {r.version, r.reason};
    }
    if (want != p_->mgr->revoked()) p_->mgr->setRevoked(want);
    for (const auto& e : p_->mgr->entries()) if (e.revoked) {
        auto it = want.find(e.m.id);
        hit.push_back({e.m.id, e.m.version, it != want.end() ? it->second.second : std::string()});
    }
    return hit;
}

bool Store::installedInfo(const std::string& id, Installed& out) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->installed.find(id);
    if (it == p_->installed.end()) return false;
    out = it->second;
    return true;
}

} // namespace dect2::pstore
