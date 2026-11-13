// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The community plugin marketplace client (see core/include/dect2/pstore/market.h and docs/PLUGIN-MARKETPLACE-4.0.md).
#include "dect2/pstore/market.h"
#include "dect2/automation/json_mini.h"
#include "dect2/plugin/zip.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>

namespace dect2::pstore {
namespace fs = std::filesystem;
namespace aj = automation::json;

// The public key of plugins.on-air.dev (made 2026-11-12, tools/marketplace/server/README.md step 5). Changing it needs a new release.
const char* const kBuiltInMarketKeyHex = "2fff25bf79f6bfe317f5fa2e8de268ee4da8509f69724063898174bc1cb59906";
const char* const kDefaultMarketUrl = "https://plugins.on-air.dev";

namespace {
constexpr uint32_t kUnknownPerm = 0x80000000u;
constexpr size_t kMaxPackage = 128u << 20;

std::string readFile(const fs::path& p, size_t maxBytes) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.size() > maxBytes ? std::string() : s;
}
bool writeFile(const fs::path& p, const std::string& data, bool priv) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path t = p.string() + ".tmp";
    { std::ofstream f(t, std::ios::binary | std::ios::trunc); if (!f) return false; f << data; if (!f) return false; }
    if (priv) fs::permissions(t, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    fs::rename(t, p, ec);
    return !ec;
}
bool isHexN(const std::string& s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
std::string pctDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) { o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16); i += 2; }
        else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}
uint32_t permBits(const aj::Value* v) {
    uint32_t m = 0;
    if (!v || v->type != aj::Value::Arr) return 0;
    for (const auto& x : v->a) {
        uint32_t bit = 0;
        for (uint32_t b = 1; b <= plugin::PermSpawn; b <<= 1) if (x.type == aj::Value::Str && x.s == plugin::permissionName(b)) bit = b;
        m |= bit ? bit : kUnknownPerm;
    }
    return m;
}
std::vector<std::string> strs(const aj::Value* v, size_t maxItems, size_t maxLen) {
    std::vector<std::string> o;
    if (v && v->type == aj::Value::Arr) for (const auto& x : v->a) if (x.type == aj::Value::Str && o.size() < maxItems) o.push_back(x.s.substr(0, maxLen));
    return o;
}
int64_t num64(const aj::Value& v, const char* k) { const double d = v.num(k); return d > 0 && d < 9e15 ? (int64_t)d : 0; }
void readSummary(const aj::Value& x, MarketItem& it) {
    it.id = x.str("id").substr(0, 64); it.title = x.str("title").substr(0, 120); it.summary = x.str("summary").substr(0, 300);
    it.owner = x.str("owner").substr(0, 40); it.version = x.str("version").substr(0, 32); it.kind = x.str("kind").substr(0, 16);
    it.tags = strs(x.get("tags"), 8, 24); it.platforms = strs(x.get("platforms"), 3, 10);
    it.likes = num64(x, "likes"); it.downloads = num64(x, "downloads"); it.updated = num64(x, "updated");
    it.review = x.str("review", "unreviewed").substr(0, 20);
}
// finds manifest.json at the top or inside one folder (as plugin::Manager::installBytes does)
bool packageManifest(const plugin::ZipReader& z, const plugin::ZipEntry*& mf, std::string& prefix) {
    mf = nullptr; prefix.clear();
    for (const auto& e : z.entries()) if (e.name == "manifest.json") { mf = &e; return true; }
    for (const auto& e : z.entries()) {
        const size_t s = e.name.find('/');
        if (s != std::string::npos && e.name.substr(s) == "/manifest.json") { mf = &e; prefix = e.name.substr(0, s + 1); return true; }
    }
    return false;
}
bool copyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive, ec);
    return !ec;
}
} // namespace

std::string urlEncode(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') o += (char)c;
        else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
    }
    return o;
}

std::string multipartBody(const std::vector<std::pair<std::string, std::string>>& fields, const std::string& fileField, const std::string& fileName,
                          const std::vector<uint8_t>& file, const std::string& boundary) {
    std::string b;
    for (const auto& f : fields) b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + f.first + "\"\r\n\r\n" + f.second + "\r\n";
    b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + fileField + "\"; filename=\"" + fileName + "\"\r\nContent-Type: application/zip\r\n\r\n";
    b.append((const char*)file.data(), file.size());
    b += "\r\n--" + boundary + "--\r\n";
    return b;
}

bool openSignedAnswer(const std::string& body, const std::string& pubKeyHex, const std::string& kind, const std::string& request, int64_t now,
                      std::string& inner, std::string& err) {
    aj::Value outer; std::string pe;
    if (!aj::parse(body, outer, pe) || outer.type != aj::Value::Obj) { err = "the marketplace sent an answer that is not JSON"; return false; }
    const aj::Value* s = outer.get("signed");
    const std::string sig = outer.str("sig");
    if (!s || s->type != aj::Value::Str) { err = outer.str("error").empty() ? "the marketplace answer is not signed: ignored" : outer.str("error").substr(0, 300); return false; }
    if (!verifyIndexSignature(s->s, sig, pubKeyHex)) { err = "The marketplace answer failed its signature check and was ignored."; return false; }
    aj::Value v;
    if (!aj::parse(s->s, v, pe) || v.type != aj::Value::Obj) { err = "the signed marketplace answer is not valid JSON"; return false; }
    if ((int)v.num("v") != 1 || v.str("kind") != kind) { err = "the signed marketplace answer is of another kind: ignored"; return false; }
    const std::string rq = v.str("request");
    if (rq != request && pctDecode(rq) != pctDecode(request)) { err = "the signed marketplace answer is for another request: ignored"; return false; }
    const int64_t t = num64(v, "time");
    if (t < now - kMarketMaxAgeSec || t > now + 86400) { err = "the marketplace answer is too old or from the future (check this computer's clock): ignored"; return false; }
    inner = s->s;
    return true;
}

// ---------------------------------------------------------------- Market
struct Market::Impl {
    MarketConfig cfg;
    plugin::Manager* mgr = nullptr;
    mutable std::mutex mu;
    std::string error, token, login, installId;
    std::map<std::string, std::string> installed;          // id -> version installed from here
    std::vector<Revocation> pulled;

    int64_t now() const { return cfg.now ? cfg.now() : (int64_t)time(nullptr); }
    fs::path path(const char* f) const { return fs::path(cfg.stateDir) / f; }
    std::string url(const std::string& p) const { std::string b = cfg.baseUrl; while (!b.empty() && b.back() == '/') b.pop_back(); return b + p; }
    void setError(const std::string& e) { std::lock_guard<std::mutex> lk(mu); error = e; }

    void load() {
        aj::Value v; std::string e;
        const std::string t = readFile(path("market-state.json"), 1 << 20);
        if (!t.empty() && aj::parse(t, v, e) && v.type == aj::Value::Obj) {
            if (const aj::Value* in = v.get("installed")) if (in->type == aj::Value::Obj) for (const auto& kv : in->o) if (kv.second.type == aj::Value::Str) installed[kv.first] = kv.second.s;
            if (const aj::Value* pl = v.get("pulled")) if (pl->type == aj::Value::Arr)
                for (const auto& x : pl->a) if (plugin::validId(x.str("id")) && pulled.size() < 1000) pulled.push_back({x.str("id"), "*", x.str("reason").substr(0, 300)});
        }
        aj::Value s;
        const std::string st = readFile(path("session.json"), 8192);
        if (!st.empty() && aj::parse(st, s, e) && s.type == aj::Value::Obj && s.num("expires") > (double)now()) { token = s.str("token"); login = s.str("login"); }
        const std::string id = readFile(path("install-id"), 200);
        if (isHexN(id.substr(0, 32), 32)) installId = id.substr(0, 32);
    }
    void saveState() {   // with mu held
        aj::Value root = aj::Value::object(), in = aj::Value::object(), pl = aj::Value::array();
        for (const auto& kv : installed) in.set(kv.first, aj::Value::string(kv.second));
        for (const auto& r : pulled) { aj::Value o = aj::Value::object(); o.set("id", aj::Value::string(r.id)); o.set("reason", aj::Value::string(r.reason)); pl.push(o); }
        root.set("installed", in); root.set("pulled", pl);
        writeFile(path("market-state.json"), aj::dump(root, true), false);
    }
    HttpReply call(HttpCall c) const {
        if (!cfg.http) { HttpReply r; r.error = "no HTTP client"; return r; }
        return cfg.http(c);
    }
    // a GET whose answer must be signed; inner is the signed JSON text
    bool getSigned(const std::string& p, const std::string& kind, aj::Value& out) {
        if (cfg.publicKeyHex.find_first_not_of('0') == std::string::npos) { setError("The community marketplace is not set up in this build (no signing key yet)."); return false; }
        HttpCall c; c.url = url(p);
        const HttpReply r = call(c);
        if (r.status == 0) { setError("Could not reach the marketplace (" + (r.error.empty() ? std::string("no answer") : r.error) + ")."); return false; }
        std::string inner, err;
        if (r.status != 200) {
            aj::Value e; std::string pe;
            setError(aj::parse(r.body, e, pe) && !e.str("error").empty() ? e.str("error").substr(0, 300) : "The marketplace answered HTTP " + std::to_string(r.status) + ".");
            return false;
        }
        if (!openSignedAnswer(r.body, cfg.publicKeyHex, kind, p, now(), inner, err)) { setError(err); return false; }
        std::string pe;
        if (!aj::parse(inner, out, pe)) { setError("bad marketplace answer"); return false; }
        return true;
    }
    // an unsigned JSON request (likes, sign-in, upload, report); false with the server's sentence in error
    bool request(HttpCall c, aj::Value& out, int* status = nullptr) {
        if (cfg.publicKeyHex.find_first_not_of('0') == std::string::npos) { setError("The community marketplace is not set up in this build (no signing key yet)."); return false; }
        c.url = url(c.url);
        const HttpReply r = call(c);
        if (status) *status = r.status;
        if (r.status == 0) { setError("Could not reach the marketplace (" + (r.error.empty() ? std::string("no answer") : r.error) + ")."); return false; }
        std::string pe;
        const bool parsed = aj::parse(r.body, out, pe) && out.type == aj::Value::Obj;
        if (r.status < 200 || r.status > 299) {
            setError(parsed && !out.str("error").empty() ? out.str("error").substr(0, 400) : "The marketplace answered HTTP " + std::to_string(r.status) + ".");
            if (r.status == 401) { std::lock_guard<std::mutex> lk(mu); token.clear(); login.clear(); std::error_code ec; fs::remove(path("session.json"), ec); }
            return false;
        }
        if (!parsed) { setError("the marketplace sent an answer that is not JSON"); return false; }
        return true;
    }
    std::string authHeader() const { std::lock_guard<std::mutex> lk(mu); return token.empty() ? std::string() : "Authorization: Bearer " + token; }
    void readPage(const aj::Value& x, MarketItem& it) const {
        readSummary(x, it);
        it.status = x.str("status", "live").substr(0, 10); it.pulledReason = x.str("pulled_reason").substr(0, 300);
        it.description = x.str("description").substr(0, 4000); it.repo = x.str("repo").substr(0, 200);
        if (it.repo.compare(0, 8, "https://") != 0) it.repo.clear();
        it.license = x.str("license").substr(0, 80); it.author = x.str("author").substr(0, 120);
        it.permissions = permBits(x.get("permissions")); it.created = num64(x, "created"); it.liked = x.flag("liked");
        it.versions.clear();
        if (const aj::Value* vs = x.get("versions")) if (vs->type == aj::Value::Arr)
            for (const auto& v : vs->a) {
                if (it.versions.size() >= 20) break;
                MarketVersion mv;
                mv.version = v.str("version").substr(0, 32); mv.sha256 = v.str("sha256"); mv.minOnAir = v.str("min_onair").substr(0, 32);
                mv.size = (uint64_t)std::max(0.0, v.num("size")); mv.platforms = strs(v.get("platforms"), 3, 10); mv.permissions = permBits(v.get("permissions"));
                if (mv.version.empty() || !isHexN(mv.sha256, 64) || mv.size == 0 || mv.size > kMaxPackage) continue;
                it.versions.push_back(mv);
            }
        it.haveDetail = true;
    }
    const MarketVersion* current(const MarketItem& it) const {
        for (const auto& v : it.versions) if (v.version == it.version) return &v;
        return nullptr;
    }
};

Market::Market(MarketConfig c, plugin::Manager* mgr) : p_(new Impl) {
    p_->cfg = std::move(c);
    p_->mgr = mgr;
    p_->load();
}
Market::~Market() { delete p_; }

bool Market::keyConfigured() const { return p_->cfg.publicKeyHex.size() == 64 && p_->cfg.publicKeyHex.find_first_not_of('0') != std::string::npos; }
std::string Market::lastError() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->error; }
std::string Market::platform() const {
    if (!p_->cfg.platform.empty()) return p_->cfg.platform;
    const std::string p = currentPlatform();
    return p.substr(0, p.find('-'));
}

std::string Market::installId() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->installId.empty()) {
        std::random_device rd;
        uint8_t b[16];
        for (auto& x : b) x = (uint8_t)(rd() & 0xFF);
        p_->installId = toHex(b, 16);
        writeFile(p_->path("install-id"), p_->installId + "\n", true);
    }
    return p_->installId;
}

bool Market::search(const MarketQuery& q, std::vector<MarketItem>& out, int& total) {
    out.clear(); total = 0;
    std::string p = "/api/v1/plugins?sort=" + urlEncode(q.sort.empty() ? std::string("relevance") : q.sort) + "&page=" + std::to_string(std::max(1, q.page)) +
                    "&limit=" + std::to_string(std::min(50, std::max(1, q.limit)));
    if (!q.q.empty()) p += "&q=" + urlEncode(q.q.substr(0, 100));
    if (!q.tag.empty()) p += "&tag=" + urlEncode(q.tag);
    if (!q.kind.empty()) p += "&kind=" + urlEncode(q.kind);
    if (!q.platform.empty()) p += "&platform=" + urlEncode(q.platform);
    aj::Value v;
    if (!p_->getSigned(p, "list", v)) return false;
    total = (int)v.num("total");
    if (const aj::Value* a = v.get("plugins")) if (a->type == aj::Value::Arr)
        for (const auto& x : a->a) {
            if (out.size() >= 50 || x.type != aj::Value::Obj) continue;
            MarketItem it;
            readSummary(x, it);
            if (plugin::validId(it.id) && !it.title.empty()) out.push_back(std::move(it));
        }
    std::vector<Revocation> pl;
    if (const aj::Value* a = v.get("pulled")) if (a->type == aj::Value::Arr)
        for (const auto& x : a->a) if (plugin::validId(x.str("id")) && pl.size() < 1000) pl.push_back({x.str("id"), "*", x.str("reason").substr(0, 300)});
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pulled = std::move(pl);
    p_->error.clear();
    p_->saveState();
    return true;
}

bool Market::tags(std::vector<std::pair<std::string, int>>& out) {
    out.clear();
    aj::Value v;
    if (!p_->getSigned("/api/v1/tags", "tags", v)) return false;
    if (const aj::Value* a = v.get("tags")) if (a->type == aj::Value::Arr)
        for (const auto& x : a->a) if (!x.str("tag").empty() && out.size() < 40) out.push_back({x.str("tag").substr(0, 24), (int)x.num("count")});
    return true;
}

bool Market::page(const std::string& id, MarketItem& out) {
    if (!plugin::validId(id)) { p_->setError("not a plugin id"); return false; }
    aj::Value v;
    if (!p_->getSigned("/api/v1/plugins/" + id + "?install=" + installId(), "plugin", v)) return false;
    const aj::Value* pl = v.get("plugin");
    if (!pl || pl->type != aj::Value::Obj || pl->str("id") != id) { p_->setError("the marketplace sent a page for another plugin: ignored"); return false; }
    MarketItem it;
    p_->readPage(*pl, it);
    out = std::move(it);
    return true;
}

bool Market::setLike(const std::string& id, bool on, int64_t& likes) {
    if (!plugin::validId(id)) return false;
    HttpCall c;
    c.method = "POST"; c.url = "/api/v1/plugins/" + id + (on ? "/like" : "/unlike"); c.contentType = "application/json";
    c.body = "{\"install\":\"" + installId() + "\"}";
    aj::Value v;
    if (!p_->request(c, v)) return false;
    likes = num64(v, "likes");
    return true;
}

std::string Market::incompatibility(const MarketItem& it) const {
    if (it.status != "live") return "withdrawn by the moderators" + (it.pulledReason.empty() ? std::string() : ": " + it.pulledReason);
    if (!it.platforms.empty() && std::find(it.platforms.begin(), it.platforms.end(), platform()) == it.platforms.end()) return "not available for " + platform();
    if (it.permissions & kUnknownPerm) return "asks for a permission this OnAir does not know";
    if (it.haveDetail) {
        const MarketVersion* v = p_->current(it);
        if (!v) return "the marketplace lists no file for this version";
        if (!v->minOnAir.empty() && !p_->cfg.hostVersion.empty() && plugin::compareVersions(p_->cfg.hostVersion, v->minOnAir) < 0) return "needs OnAir " + v->minOnAir + " or newer";
    }
    return {};
}

bool Market::installedFromHere(const std::string& id, std::string* version) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->installed.find(id);
    if (it == p_->installed.end()) return false;
    if (version) *version = it->second;
    return true;
}
void Market::forgetInstalled(const std::string& id) { std::lock_guard<std::mutex> lk(p_->mu); p_->installed.erase(id); p_->saveState(); }
std::vector<Revocation> Market::pulled() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->pulled; }
std::vector<Revocation> Market::pulledInstalled() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Revocation> o;
    for (const auto& r : p_->pulled) if (p_->installed.count(r.id)) o.push_back({r.id, "*", "withdrawn by the OnAir marketplace moderators" + (r.reason.empty() ? std::string() : ": " + r.reason)});
    return o;
}

Result Market::install(const std::string& id, bool confirmedUnreviewed, const std::function<void(uint64_t, uint64_t)>& progress) {
    Result r;
    if (!p_->mgr) { r.error = "no plugin manager"; return r; }
    MarketItem it;
    if (!page(id, it)) { r.error = "Not installed: " + lastError(); return r; }
    const std::string why = incompatibility(it);
    if (!why.empty()) { r.error = "Not installed: " + why + "."; return r; }
    if (!confirmedUnreviewed) { r.error = "Not installed: the warning about unreviewed plugins was not confirmed."; return r; }
    plugin::Entry cur;
    const bool have = p_->mgr->find(id, cur);
    if (have && cur.bundled) { r.error = "Not installed: a bundled example with this id exists."; return r; }
    if (have && !cur.m.version.empty() && plugin::compareVersions(it.version, cur.m.version) < 0) {
        r.error = "Not installed: version " + it.version + " is older than the installed " + cur.m.version + " (downgrades are refused)."; return r;
    }
    const MarketVersion ver = *p_->current(it);   // incompatibility() made sure it exists
    aj::Value d;
    if (!p_->getSigned("/api/v1/plugins/" + id + "/download?format=json&version=" + urlEncode(ver.version) + "&install=" + installId(), "download", d)) { r.error = "Not installed: " + lastError(); return r; }
    if (d.str("id") != id || d.str("version") != ver.version || d.str("sha256") != ver.sha256 || (uint64_t)d.num("size") != ver.size) {
        r.error = "Not installed: the download does not match the plugin's signed page."; return r;
    }
    const std::string url = d.str("url");
    if (url.compare(0, 8, "https://") != 0) { r.error = "Not installed: the download address is not https://."; return r; }
    HttpCall c; c.url = url; c.timeoutSec = 300; c.maxReply = (size_t)ver.size + 1;
    if (progress) progress(0, ver.size);
    const HttpReply got = p_->call(c);
    if (got.status != 200) { r.error = "Not installed: the download failed (" + (got.status ? "HTTP " + std::to_string(got.status) : got.error) + ")."; return r; }
    const std::vector<uint8_t> pkg(got.body.begin(), got.body.end());
    if (progress) progress(pkg.size(), ver.size);
    if (pkg.size() != ver.size) { r.error = "Not installed: the download has the wrong size (" + std::to_string(pkg.size()) + " instead of " + std::to_string(ver.size) + " bytes)."; return r; }
    if (sha256Hex(pkg) != ver.sha256) { r.error = "Not installed: the download does not match its signed sha256. Nothing was written."; return r; }
    plugin::ZipReader z;
    std::string err, prefix;
    const plugin::ZipEntry* mf = nullptr;
    if (!z.open(pkg, err)) { r.error = "Not installed: " + err + "."; return r; }
    if (!packageManifest(z, mf, prefix)) { r.error = "Not installed: the package has no manifest.json."; return r; }
    std::vector<uint8_t> mb;
    plugin::Manifest m;
    if (!z.read(*mf, mb, err) || !plugin::parseManifest(std::string(mb.begin(), mb.end()), m, err, p_->cfg.hostVersion)) { r.error = "Not installed: " + err + "."; return r; }
    if (m.id != id || m.version != ver.version) { r.error = "Not installed: the package is " + m.id + " " + m.version + ", not " + id + " " + ver.version + "."; return r; }
    if (m.abi != p_->cfg.hostAbi) { r.error = "Not installed: the package speaks a different plugin interface version."; return r; }
    if (!it.kind.empty() && m.kind != it.kind) { r.error = "Not installed: the package's kind differs from its page."; return r; }
    if (m.permissions & ~ver.permissions) { r.error = "Not installed: the package asks for permissions that its page does not show."; return r; }
    const fs::path live = fs::path(p_->mgr->pluginsDir()) / id, backup = fs::path(p_->cfg.stateDir) / "rollback" / id;
    const bool backedUp = have && fs::exists(live) && copyTree(live, backup);
    auto rollBack = [&](const std::string& reason) {
        std::error_code ec;
        if (backedUp) { fs::remove_all(live, ec); copyTree(backup, live); p_->mgr->scan(); }
        r.error = "Not installed: " + reason + (backedUp ? " The previous version was kept." : "");
        return r;
    };
    std::string installedId;
    if (!p_->mgr->installBytes(pkg, installedId, err)) return rollBack(err + ".");
    plugin::Entry now;
    if (!p_->mgr->find(id, now) || !now.valid || now.m.version != ver.version) return rollBack("the installed plugin did not check out.");
    { std::error_code ec; fs::remove_all(backup, ec); }
    { std::lock_guard<std::mutex> lk(p_->mu); p_->installed[id] = ver.version; p_->saveState(); }
    r.ok = true; r.version = ver.version;
    return r;
}

// ---------------------------------------------------------------- sign-in
bool Market::signInStart(DeviceStart& out) {
    HttpCall c; c.method = "POST"; c.url = "/api/v1/auth/device"; c.contentType = "application/json"; c.body = "{}";
    aj::Value v;
    if (!p_->request(c, v)) return false;
    out.userCode = v.str("user_code").substr(0, 20); out.deviceCode = v.str("device_code").substr(0, 100);
    out.verificationUri = v.str("verification_uri", "https://github.com/login/device").substr(0, 200);
    if (out.verificationUri.compare(0, 19, "https://github.com/") != 0) out.verificationUri = "https://github.com/login/device";   // only ever send people to GitHub
    out.interval = std::max(1, std::min(60, (int)v.num("interval", 5)));
    out.expiresIn = std::max(60, std::min(3600, (int)v.num("expires_in", 900)));
    if (out.userCode.empty() || out.deviceCode.empty()) { p_->setError("the marketplace sent no sign-in code"); return false; }
    return true;
}
SignInState Market::signInPoll(const std::string& deviceCode, int* newInterval) {
    HttpCall c; c.method = "POST"; c.url = "/api/v1/auth/poll"; c.contentType = "application/json";
    c.body = "{\"device_code\":\"" + aj::escape(deviceCode) + "\"}";
    aj::Value v;
    if (!p_->request(c, v)) return SignInState::Error;
    const std::string s = v.str("status");
    if (s == "pending") return SignInState::Pending;
    if (s == "slow_down") { if (newInterval) *newInterval = std::max(1, std::min(60, (int)v.num("interval", 10))); return SignInState::SlowDown; }
    if (s == "denied") return SignInState::Denied;
    if (s == "expired") return SignInState::Expired;
    if (s != "ok" || v.str("token").empty() || v.str("login").empty()) { p_->setError("unexpected sign-in answer"); return SignInState::Error; }
    aj::Value sess = aj::Value::object();
    sess.set("token", aj::Value::string(v.str("token"))); sess.set("login", aj::Value::string(v.str("login").substr(0, 40)));
    sess.set("expires", aj::Value::number(v.num("expires", (double)(p_->now() + 30 * 86400))));
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->token = v.str("token"); p_->login = v.str("login").substr(0, 40);
    writeFile(p_->path("session.json"), aj::dump(sess, true), true);
    return SignInState::Ok;
}
bool Market::signedIn() const { std::lock_guard<std::mutex> lk(p_->mu); return !p_->token.empty(); }
std::string Market::login() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->login; }
void Market::signOut() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->token.clear(); p_->login.clear();
    std::error_code ec;
    fs::remove(p_->path("session.json"), ec);
}
bool Market::mine(std::vector<MarketItem>& out) {
    out.clear();
    HttpCall c; c.url = "/api/v1/me";
    const std::string h = p_->authHeader();
    if (h.empty()) { p_->setError("Sign in with GitHub first."); return false; }
    c.headers.push_back(h);
    aj::Value v;
    if (!p_->request(c, v)) return false;
    if (const aj::Value* a = v.get("plugins")) if (a->type == aj::Value::Arr)
        for (const auto& x : a->a) { MarketItem it; readSummary(x, it); it.status = x.str("status", "live"); it.pulledReason = x.str("pulled_reason").substr(0, 300); out.push_back(std::move(it)); }
    return true;
}

// ---------------------------------------------------------------- publish
UploadPreview Market::preview(const std::vector<uint8_t>& package, const std::string& hostVersion) {
    UploadPreview pv;
    plugin::ZipReader z;
    std::string err, prefix;
    if (package.size() < 22 || !z.open(package, err)) { pv.error = "This is not an .onairplugin package (a zip file)" + (err.empty() ? std::string() : ": " + err) + "."; return pv; }
    const plugin::ZipEntry* mf = nullptr;
    if (!packageManifest(z, mf, prefix)) { pv.error = "The package has no manifest.json."; return pv; }
    std::vector<uint8_t> mb;
    if (!z.read(*mf, mb, err) || !plugin::parseManifest(std::string(mb.begin(), mb.end()), pv.manifest, err, hostVersion)) { pv.error = "manifest.json: " + err + "."; return pv; }
    auto present = [&](const std::string& f) { for (const auto& e : z.entries()) if (!e.dir && e.name == prefix + f) return true; return false; };
    const std::pair<const char*, const std::string*> libs[] = {{"macos", &pv.manifest.libMac}, {"linux", &pv.manifest.libLinux}, {"windows", &pv.manifest.libWindows}};
    for (const auto& l : libs) {
        if (l.second->empty()) continue;
        if (!present(*l.second)) { pv.error = "The manifest names " + *l.second + " as the " + l.first + " library, but it is not in the package."; return pv; }
        pv.platforms.push_back(l.first);
    }
    if (pv.platforms.empty()) {
        if (pv.manifest.kind != "panel-lite") { pv.error = "The manifest names no library (\"library\": {\"macos\": ..., \"linux\": ..., \"windows\": ...})."; return pv; }
        pv.platforms = {"macos", "linux", "windows"};
    }
    pv.ok = true;
    return pv;
}

bool Market::upload(const UploadForm& f, std::string& id, std::string& version) {
    const std::string h = p_->authHeader();
    if (h.empty()) { p_->setError("Sign in with GitHub first."); return false; }
    const UploadPreview pv = preview(f.package);
    if (!pv.ok) { p_->setError(pv.error); return false; }
    if (f.newVersionOf.empty() && (f.title.empty() || f.description.empty())) { p_->setError("A title and a description are required."); return false; }
    if (!f.newVersionOf.empty() && f.newVersionOf != pv.manifest.id) { p_->setError("This package is " + pv.manifest.id + ", not " + f.newVersionOf + "."); return false; }
    std::random_device rd;
    uint8_t rb[12];
    for (auto& x : rb) x = (uint8_t)(rd() & 0xFF);
    const std::string boundary = "----OnAirBoundary" + toHex(rb, 12);
    std::vector<std::pair<std::string, std::string>> fields;
    if (!f.title.empty()) fields.push_back({"title", f.title});
    if (!f.description.empty()) fields.push_back({"description", f.description});
    if (!f.repo.empty()) fields.push_back({"repo", f.repo});
    if (!f.tags.empty() || f.newVersionOf.empty()) fields.push_back({"tags", f.tags});
    for (const auto& kv : fields) if (kv.second.find(boundary) != std::string::npos) { p_->setError("bad text"); return false; }
    HttpCall c;
    c.method = "POST"; c.url = f.newVersionOf.empty() ? "/api/v1/plugins" : "/api/v1/plugins/" + f.newVersionOf + "/versions";
    c.contentType = "multipart/form-data; boundary=" + boundary;
    c.headers.push_back(h);
    c.body = multipartBody(fields, "package", pv.manifest.id + "-" + pv.manifest.version + ".onairplugin", f.package, boundary);
    c.timeoutSec = 300;
    aj::Value v;
    if (!p_->request(c, v)) return false;
    id = v.str("id"); version = v.str("version");
    return !id.empty();
}

bool Market::report(const std::string& id, const std::string& reason, const std::string& text) {
    const std::string h = p_->authHeader();
    if (h.empty()) { p_->setError("Sign in with GitHub first."); return false; }
    if (!plugin::validId(id)) return false;
    HttpCall c;
    c.method = "POST"; c.url = "/api/v1/plugins/" + id + "/report"; c.contentType = "application/json"; c.headers.push_back(h);
    c.body = "{\"reason\":\"" + aj::escape(reason) + "\",\"text\":\"" + aj::escape(text.substr(0, 2000)) + "\"}";
    aj::Value v;
    return p_->request(c, v);
}

} // namespace dect2::pstore
