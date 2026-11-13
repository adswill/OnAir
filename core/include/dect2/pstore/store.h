// The plugin store (client side): a static, signed registry of plugins (docs/PLUGIN-MARKETPLACE-3.9.md).
// Nothing here runs by itself: the program calls refresh() only when the user opens the Store or presses Refresh, and install() only after the
// user confirmed the native-code warning. The registry is plain files (index.json, index.json.sig, per-plugin manifests, packages) on any HTTPS
// host or in a local folder. The index is signed with Ed25519; every package is checked against the sha256 in the (signed) listing before anything
// is written to the plugins folder. The sandbox for untrusted plugins does not exist yet: "reviewed" means a person looked, not a guarantee.
#pragma once
#include "dect2/plugin/host.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2::pstore {

constexpr int kSchema = 1;                          // index.json "schema"
constexpr size_t kMaxIndexBytes = 1u << 20;         // the JSON reader takes 1 MiB at most
constexpr size_t kMaxDetailBytes = 256u << 10;
constexpr uint64_t kMaxPackageBytes = 128ull << 20;

// The platform names of the registry: windows-x64, macos-arm64, macos-x86_64, linux-x86_64, linux-aarch64 (anything else: the build's own guess).
std::string currentPlatform();
// Public key of the project's registry (Ed25519, 64 hex digits). All zeros until the owner has made the key: the official registry then says so.
extern const char* const kBuiltInRegistryKeyHex;
extern const char* const kDefaultRegistryUrl;

struct Download { std::string url; uint64_t size = 0; std::string sha256; };
enum class Review { Listed, Reviewed, Verified };
const char* reviewName(Review r);                   // "listed", "reviewed", "verified"

// One plugin as the registry describes it. The index holds the summary part; detail() fills in the rest from the per-plugin manifest.
struct Listing {
    std::string id, name, summary, version, minOnAir, kind, license, review = "listed";
    uint32_t abi = 0, permissions = 0;
    std::vector<std::string> categories, tags, platforms;
    std::string manifestPath, manifestSha256;      // the per-plugin manifest, vouched for by the signed index
    // detail (empty until detail() ran)
    bool haveDetail = false;
    std::string description, author, homepage, source;
    std::vector<std::string> screenshots;
    std::map<std::string, Download> downloads;     // by platform
    Review reviewLevel() const { return review == "verified" ? Review::Verified : review == "reviewed" ? Review::Reviewed : Review::Listed; }
};

struct Revocation { std::string id, version, reason; };   // version: "*", "1.2.0" (exactly) or "<=1.2.0"
// true when the spec covers this version
bool revocationMatches(const std::string& spec, const std::string& version);

enum class Source { None, Network, Cache };

// How a file is fetched: https:// URL, file:// URL or a plain folder path. Returns false with err. progress(done, total) may be called (total 0 = unknown).
using Fetcher = std::function<bool(const std::string& url, size_t maxBytes, std::vector<uint8_t>& out, std::string& err,
                                   const std::function<void(uint64_t, uint64_t)>& progress)>;
// The built-in fetcher: https through the program's HTTP client (certificates verified), local files and folders. Plain http:// is refused.
Fetcher defaultFetcher(const std::string& userAgent);

struct Config {
    std::string registryUrl;                       // https://host/path/ or a local folder; the files are index.json, index.json.sig, plugins/...
    std::string publicKeyHex;                      // the key the index must be signed with
    std::string cacheDir;                          // plugins/.store (index and signature of the last good fetch, state)
    std::string platform;                          // "" = currentPlatform()
    std::string hostVersion;                       // the running OnAir ("3.9.0")
    uint32_t hostAbi = plugin::kAbiVersion;
    Fetcher fetch;                                 // "" -> defaultFetcher
    bool autoRevocations = true;                   // refresh/install/loadCache apply the revocations to the manager by themselves; the app turns this off and calls applyRevocations() on its UI thread
};

struct Result { bool ok = false; std::string error; std::string version; };
struct UpdateInfo { std::string id, installed, available; };
struct Installed { std::string version, review, installedAt; };   // what the store recorded for a plugin it installed

class Store {
public:
    Store(Config c, plugin::Manager* mgr);
    ~Store();

    // Reads the cached index without any network (and applies its revocations). False when there is no valid cache.
    bool loadCache();
    // Fetches and verifies the index. On any failure the cached list stays in use (state().source == Cache) and the reason is in lastError().
    bool refresh();
    // A signature failure, a lower serial than the cache (rollback) or a bad document is a failure: the old list stays.
    std::string lastError() const;
    std::string notice() const;                    // e.g. "Offline: showing the list saved on 2026-11-11" ("" = none)
    Source source() const;
    int serial() const;
    std::string generated() const;
    bool signatureKeyConfigured() const;           // false: the key is all zeros (the official registry has no key yet)

    // The compatible plugins (this platform, this ABI, this OnAir), optionally filtered by text (name, summary, id, tags) and category.
    std::vector<Listing> list(const std::string& query = {}, const std::string& category = {}) const;
    std::vector<std::string> categories() const;
    size_t hiddenCount() const;                    // listings left out as not compatible
    bool find(const std::string& id, Listing& out) const;       // searches all listings, compatible or not
    // Fetches the per-plugin manifest (checked against the index) into the listing. False and err when it fails.
    bool detail(const std::string& id, Listing& out, std::string& err);
    // Why a listing cannot be installed here ("" = it can): wrong platform, ABI, too new minimum version, revoked
    std::string incompatibility(const Listing& l) const;
    std::vector<Revocation> revocations() const;

    // Installs or updates. confirmedNative must be true: the caller shows the permissions and the native-code warning first and only sets it
    // after the user said yes. progress(done, total) while downloading. Refused: revoked, wrong platform/ABI/version, a lower version than the
    // installed one (downgrade), a wrong size or sha256, a package that does not match its listing or asks for more permissions than listed.
    // A failed update leaves (or puts back) the previous version.
    Result install(const std::string& id, bool confirmedNative, const std::function<void(uint64_t, uint64_t)>& progress = {});
    Result uninstall(const std::string& id);
    // Installed (not bundled) plugins that have a newer compatible listing.
    std::vector<UpdateInfo> updates() const;
    // Applies the cached revocations to the plugin manager; returns the installed plugins that are switched off now.
    std::vector<Revocation> applyRevocations();
    // Revocations from another source that applyRevocations() merges in (4.0: plugins the community marketplace withdrew that were
    // installed from it, see market.h). The registry's own entries win for the same id.
    void setExtraRevocations(const std::vector<Revocation>& r);
    bool installedInfo(const std::string& id, Installed& out) const;

private:
    struct Impl;
    Impl* p_;
};

// For the tests and the tools: the exact bytes the signature covers are the bytes of index.json. sig: 128 hex digits.
bool verifyIndexSignature(const std::string& indexBytes, const std::string& sigHex, const std::string& pubKeyHex);
std::string toHex(const uint8_t* p, size_t n);
bool fromHex(const std::string& s, std::vector<uint8_t>& out);
std::string sha256Hex(const std::vector<uint8_t>& d);

} // namespace dect2::pstore
