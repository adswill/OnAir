// OnAir Web directory: an optional heartbeat that tells a public directory of OnAir Web servers that this one exists (name, location, coverage,
// user counts). It is OFF by default and nothing is sent anywhere unless the host turns it on with web_directory=1, gives a directory URL
// (https only) and confirms what will be sent with web_directory_ack (the server prints the exact JSON and the code first). It never sends a
// listener's address or what anyone listens to, only the number of users. The messages are signed with an Ed25519 key made on first start
// (docs/ONAIR-DIRECTORY.md).
#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace onair_web {

struct DirectoryConfig {
    bool on = false;            // web_directory
    std::string url;            // web_directory_url (https only)
    std::string publicUrl;      // web_public_url: how listeners reach this server (https://...)
    int intervalSec = 300;      // web_directory_interval (60..3600)
    std::string keyFile;        // web_directory_key: where the Ed25519 seed is kept (set by the server when empty)
    std::string loc;            // web_directory_loc: "lat,lon" or a Maidenhead locator; empty: no location is sent
    int locDecimals = 2;        // web_directory_loc_decimals: 0..5 (2 is about a kilometre)
    std::string freq;           // web_directory_freq: "88-108,1.0-30" MHz; empty: the capture window
    std::string radio;          // web_directory_radio: "HackRF Pro", "RTL-SDR v4", ...
    std::string contact;        // web_directory_contact (optional, shown publicly)
    std::string desc;           // web_directory_desc
    bool reach = false;         // web_directory_reach: let the directory check that /web/v1/info answers (opt in)
    std::string ack;            // web_directory_ack: the code printed with the preview
    std::string version;        // software version (set by the server)
    std::string name;           // not a setting: filled from the live info when empty
    int maxUsers = 8;           // not a setting: web_max_users, used for the stand-in info (the preview and the confirmation code are made without a running web server)
};

class DirectoryHeartbeat {
public:
    virtual ~DirectoryHeartbeat() = default;
    // Prepares the key and checks the settings. Returns false (err says why and what to do) when nothing will be sent: no HTTPS in the build, no
    // confirmation yet, a bad address. Otherwise starts its own thread; `info` returns the public info JSON at each beat.
    virtual bool start(const DirectoryConfig& c, std::function<std::string()> info, std::string& err) = 0;
    virtual void stop() = 0;
    // Asks the directory to remove this server now (signed delete); called by `onair-server --directory-remove` (needs the directory URL and the key file, not the confirmation). Blocking.
    virtual bool removeNow(const DirectoryConfig& c, std::string& err) = 0;
};

// Where the heartbeat keeps its status file (web_directory_status.json beside the key file); the app and `onair-server --directory-test` read it.
std::string directoryStatusPath(const DirectoryConfig& c);
// The info JSON used when no web server is running yet (the preview, the confirmation check of the listing program): name, 0 users, the capacity.
std::string directoryStandInInfo(const DirectoryConfig& c);
// The preview and the confirmation code for these settings WITHOUT sending anything (and without needing a running web server): `info` is the
// live info JSON (or a stand-in with name / max_users / capture). Makes the key file when it does not exist yet. False with err when a setting is not valid.
bool directoryPreview(const DirectoryConfig& c, const std::string& infoJson, std::string& preview, std::string& ack, std::string& err);
// "Test connection": checks the directory, the local web server (localPort > 0) and this server's public address one after the other, one JSON line
// per step through emit (step, ok, ms, code, message, advice, skew_sec). Every request has a time limit; `cancel` ends it at once. Blocking: run it off the UI thread.
void directoryCheck(const DirectoryConfig& c, int localPort, const std::function<void(const std::string&)>& emit, const std::atomic<bool>& cancel);
// One plain GET of the local web server's /web/v1/info (127.0.0.1:port) with a short time limit: "" when it does not answer.
std::string fetchLocalInfo(int port, const std::atomic<bool>& cancel);

// The module's factory (always built in; start() says when the build has no TLS client)
DirectoryHeartbeat* makeDirectoryHeartbeat();

} // namespace onair_web
