// The community plugin marketplace (client side, docs/PLUGIN-MARKETPLACE-4.0.md): anyone signed in with GitHub can publish a plugin; it is
// live at once and marked "unreviewed". Browsing, liking and downloading need no account. The server is tools/marketplace/server (a
// Cloudflare Worker). Every answer this class trusts for what to install (the list, a plugin page, the download address and its sha256) is
// signed with the marketplace's Ed25519 key and names the request it answers and the time; an answer that fails is refused. The package is
// checked against the signed sha256 and size, and its manifest against the signed page, before the plugin manager installs it.
// Nothing here runs by itself: every call is a user action. The HTTP requests go through Config::http, which the program supplies.
#pragma once
#include "dect2/plugin/host.h"
#include "dect2/pstore/store.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2::pstore {

// Public key of the project's marketplace (64 hex digits). All zeros until the owner has made the key (tools/marketplace/server README):
// such a build says the marketplace is not set up and contacts nothing.
extern const char* const kBuiltInMarketKeyHex;
extern const char* const kDefaultMarketUrl;           // https://plugins.on-air.dev
constexpr int64_t kMarketMaxAgeSec = 7 * 86400;        // a signed answer older than this is refused (replay)

struct HttpCall {
    std::string method = "GET", url, body, contentType;
    std::vector<std::string> headers;                  // "Name: value"
    int timeoutSec = 20;
    size_t maxReply = 1u << 20;
};
struct HttpReply { int status = 0; std::string body, error; };   // status 0: no answer (error says why)
using HttpFn = std::function<HttpReply(const HttpCall&)>;

struct MarketVersion { std::string version, sha256, minOnAir; uint64_t size = 0; std::vector<std::string> platforms; uint32_t permissions = 0; };
struct MarketItem {
    std::string id, title, summary, owner, version, kind, review = "unreviewed";
    std::vector<std::string> tags, platforms;          // platforms: macos, linux, windows
    int64_t likes = 0, downloads = 0, updated = 0;
    // the plugin page (empty until page() ran)
    bool haveDetail = false, liked = false;
    std::string description, repo, license, author, status = "live", pulledReason;
    uint32_t permissions = 0;                          // plugin::Permission bits; kUnknown when a name is not known here
    int64_t created = 0;
    std::vector<MarketVersion> versions;               // newest first
};
struct MarketQuery {
    std::string q, sort = "relevance", tag, kind, platform;   // sort: relevance, likes, downloads, newest; platform: "" = any
    int page = 1, limit = 30;
};
struct DeviceStart { std::string userCode, verificationUri, deviceCode; int interval = 5, expiresIn = 900; };
enum class SignInState { Pending, SlowDown, Ok, Denied, Expired, Error };
struct UploadForm {
    std::vector<uint8_t> package;                      // the .onairplugin file
    std::string title, description, repo, tags;        // title and description are required for a new plugin
    std::string newVersionOf;                          // "" = a new plugin; else the id of your own plugin
};
// What the app shows before publishing: the manifest as OnAir's installer reads it, the platforms that have a library, or why not.
struct UploadPreview { bool ok = false; std::string error; plugin::Manifest manifest; std::vector<std::string> platforms; };

struct MarketConfig {
    std::string baseUrl;                               // https://plugins.on-air.dev (no trailing slash needed)
    std::string publicKeyHex;
    std::string stateDir;                              // plugins/.market: install id, sign-in, what was installed from here
    std::string platform;                              // "" = this computer (macos, linux or windows)
    std::string hostVersion;
    uint32_t hostAbi = plugin::kAbiVersion;
    HttpFn http;                                       // required
    std::function<int64_t()> now;                      // "" = the system clock (tests set it)
};

class Market {
public:
    Market(MarketConfig c, plugin::Manager* mgr);
    ~Market();

    bool keyConfigured() const;                        // false: the key is all zeros
    std::string lastError() const;
    std::string platform() const;                      // macos, linux or windows
    // A random id made on first use and kept in stateDir; the server stores only a salted hash of it (one like and one download count each).
    std::string installId();

    // Browse. False and lastError() on any failure (network, signature, bad answer).
    bool search(const MarketQuery& q, std::vector<MarketItem>& out, int& total);
    bool tags(std::vector<std::pair<std::string, int>>& out);
    bool page(const std::string& id, MarketItem& out);
    // Like or unlike (once per install). likes: the new count.
    bool setLike(const std::string& id, bool on, int64_t& likes);
    // Why the listing cannot be installed here ("" = it can): platform, ABI, minimum version, unknown permission, withdrawn.
    std::string incompatibility(const MarketItem& it) const;
    // Installs or updates the current version. confirmedUnreviewed must be true: the caller showed that the plugin is unreviewed and runs
    // with the program's full rights, and the user agreed. The download is checked against the signed sha256 and size; the package's
    // manifest must match the signed page (id, version, kind, permissions not above it). A failed update puts the old version back.
    Result install(const std::string& id, bool confirmedUnreviewed, const std::function<void(uint64_t, uint64_t)>& progress = {});
    bool installedFromHere(const std::string& id, std::string* version = nullptr) const;
    void forgetInstalled(const std::string& id);       // after the user removed it
    // Plugins the moderators withdrew (from the last list), and those of them that were installed from here: the app turns them off.
    std::vector<Revocation> pulled() const;
    std::vector<Revocation> pulledInstalled() const;

    // GitHub sign-in (device flow, through the marketplace server; OnAir never sees a password and never stores the GitHub token).
    bool signInStart(DeviceStart& out);
    SignInState signInPoll(const std::string& deviceCode, int* newInterval = nullptr);
    bool signedIn() const;
    std::string login() const;
    void signOut();
    // Your own plugins (id, title, version, status); needs a sign-in.
    bool mine(std::vector<MarketItem>& out);

    // Publish. Checks locally first (preview()), then uploads. On success id and version are set.
    static UploadPreview preview(const std::vector<uint8_t>& package, const std::string& hostVersion = {});
    bool upload(const UploadForm& f, std::string& id, std::string& version);
    bool report(const std::string& id, const std::string& reason, const std::string& text);   // reason: malware, broken, licence, spam, other

private:
    struct Impl;
    Impl* p_;
};

// For the tests and the server contract: opens a signed answer {"signed": text, "sig": hex}; checks the signature, v, kind, request and age.
bool openSignedAnswer(const std::string& body, const std::string& pubKeyHex, const std::string& kind, const std::string& request, int64_t now,
                      std::string& inner, std::string& err);
std::string urlEncode(const std::string& s);
std::string multipartBody(const std::vector<std::pair<std::string, std::string>>& fields, const std::string& fileField, const std::string& fileName,
                          const std::vector<uint8_t>& file, const std::string& boundary);

} // namespace dect2::pstore
