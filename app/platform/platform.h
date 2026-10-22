// Operating-system services for the UI: file dialogs, saved settings, fonts.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace plat {

// A message the user must see before the program exits (a message box on Windows, standard error elsewhere).
void showFatalError(const char* title, const char* text);

std::string openFileDialog();
std::string saveFileDialog(const char* defaultName);
std::string openFolderDialog();   // an existing folder; empty when cancelled or no dialog is available (type the path then)

// Saved settings. Keys are short names; values are stored per platform (NSUserDefaults on macOS, a text file elsewhere).
struct Channel { double freqMhz = 0, bwMhz = 8; std::string name, mode; float snrDb = 0; int nServices = 0; bool favourite = false; };

struct Prefs {
    virtual ~Prefs() {}
    virtual bool has(const char* key) = 0;
    virtual double getD(const char* key, double def) = 0;
    virtual long getI(const char* key, long def) = 0;
    virtual bool getB(const char* key, bool def) = 0;
    virtual std::string getS(const char* key, const std::string& def) = 0;
    virtual void setD(const char* key, double v) = 0;
    virtual void setI(const char* key, long v) = 0;
    virtual void setB(const char* key, bool v) = 0;
    virtual void setS(const char* key, const std::string& v) = 0;
    virtual std::vector<Channel> getChannels() = 0;
    virtual void setChannels(const std::vector<Channel>& c) = 0;
    virtual void flush() {}
    // Start over with no saved settings (after starts that kept crashing): the old ones are kept aside, not deleted.
    // Returns where they went (UTF-8), empty when they could not be kept.
    virtual std::string setAside() = 0;
};
Prefs& prefs();       // what the app reads and writes: the keys of one receiver get its prefix (rx_prefs.cpp)
Prefs& basePrefs();   // the platform's store itself
extern int gPrefRx;   // the receiver whose settings prefs() reads and writes (1 = the first: no prefix)

// Pictures and position (the ADS-B map).
// Decode a picture file into RGBA8 (0xAABBGGRR, row by row): PNG everywhere, on macOS also what the system reads. False when it cannot be read.
bool decodeImage(const std::string& path, int& w, int& h, std::vector<uint32_t>& rgba);
// A folder for downloaded files that may be thrown away (map tiles of this session only), created on demand. UTF-8.
std::string cacheDir();
// A folder for downloaded files the user keeps (map tiles kept until deleted): system cleaners leave it alone. Created on demand. UTF-8.
std::string dataDir();
// Downloads url into file (UTF-8 path) with the curl program, which Windows 10 and later, macOS and nearly every Linux have (no TLS library
// is linked). On Windows it runs without a console window. Gives up after about 15 s. False when the download did not work.
bool fetchUrl(const std::string& url, const std::string& file, const std::string& userAgent);
// POST of UTF-8 JSON (Content-Type: application/json), same curl program. False when it did not work (including HTTP 4xx/5xx).
bool postJson(const std::string& url, const std::string& json, const std::string& userAgent);
// POST with a Content-Type and extra headers ("Name: value", e.g. an access token). The headers are passed to curl in a private temporary file,
// so a token never appears in a process list. False when it did not work (including HTTP 4xx/5xx).
bool postRequest(const std::string& url, const std::string& body, const std::string& contentType, const std::vector<std::string>& headers, const std::string& userAgent);
// The same, and the response body (up to 64 KB) in reply. The Live rooms feature needs the room id and token the server answers with.
bool postJsonReply(const std::string& url, const std::string& json, const std::string& userAgent, std::string& reply);
// One HTTP request that tells why it failed. method "GET", "HEAD" or "POST" (body sent as JSON). status is the HTTP status (0 when there was no
// answer); net says why there was none: 0 an answer came, 1 the host name was not found, 2 no connection (offline, refused, unreachable), 3 timed out,
// 4 anything else (TLS ...). detail is a short technical text for the log. body is the answer (up to 64 KB).
struct HttpResult { int status = 0; int net = 0; std::string body; std::string detail; };
HttpResult httpRequest(const std::string& method, const std::string& url, const std::string& body, const std::string& userAgent, int timeoutSec);
// Starts a program with arguments and does not wait for it. No shell is ever involved: every argument stays one argument. False and err when it did not start.
bool spawnProgram(const std::string& program, const std::vector<std::string>& args, std::string& err);
// A desktop notification (Notification Center on macOS, a toast on Windows, notify-send on Linux when installed). False when the system could not show one.
bool notify(const std::string& title, const std::string& body);
// The position from the system's location service (on macOS this is Wi-Fi positioning and needs the user's permission).
// locateStart() begins a request; locateState() says how it went: 0 nothing asked, 1 working, 2 done (lat, lon, accuracy in metres), 3 failed (msg says why).
void locateStart();
int locateState(double& lat, double& lon, double& accuracyM, std::string& msg);

// Paths of fonts to try, best first (proportional UI font, monospaced font)
std::vector<std::string> uiFontCandidates();
std::vector<std::string> cjkFontCandidates();   // system CJK font merged into the UI fonts for Chinese/Japanese/Korean service names
std::vector<std::string> monoFontCandidates();

// The language the user reads the system in, as a code such as "de", "zh-Hans" or "lt_LT.UTF-8" (i18n::set() takes the first letters, or a listed region such as pt-PT)
std::string uiLanguage();

}
