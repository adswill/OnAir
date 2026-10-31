// OnAir Web setup (Server > Share on web): the part with no window and no processes, so it is tested alone (tests/webshare/test_webshare_core.cpp).
// What it holds: the choices the owner makes (radio, centre and span, decoders, who may listen, the listing), how they are saved, how they become
// the settings file of onair-server, which decoders are possible in the chosen range, and the small state machine of step 2 (the directory listing)
// that turns the preview / status / test lines of `onair-server --directory-*` into what the window shows. Docs: docs/ONAIR-WEB-SHARING.md.
#pragma once
#include "dect2/source/source.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace webshare {

// ---------------------------------------------------------------- radios
struct RigCaps {
    std::string name, board;
    double minHz = 0, maxHz = 0;                 // the tuning range the radio is specified for (0: not known)
    double minRate = 0, maxRate = 0, steadyRate = 0;   // complex sample rates (0: not known)
    bool network = false, test = false, isRadio = true;
};
RigCaps capsFromDevice(const dect2::DeviceInfo& d);
std::string capsLine(const RigCaps& c);          // "24 MHz - 1.77 GHz, up to 3.2 Msps"
// The sources the dropdown offers: real radios first, then network radios, then the test signal; files and sound cards are left out.
std::vector<RigCaps> shareableRigs(const std::vector<dect2::DeviceInfo>& devices);

// ---------------------------------------------------------------- range presets
struct RangePreset { const char* id; const char* label; double centreHz, spanHz; };
const std::vector<RangePreset>& rangePresets();

// ---------------------------------------------------------------- what the owner chooses (saved)
struct ShareSettings {
    std::string rig;                  // the radio's name; empty: the first one
    bool wholeSpan = true;            // the radio's whole instantaneous bandwidth around the centre
    double centreMHz = 98.0;
    double spanMHz = 20.0;            // used when wholeSpan is off
    bool rateAuto = true;             // off: rateMsps is used as it is (no limit of OnAir's own)
    double rateMsps = 0;
    bool gainManual = false;
    double gainDb = 30;
    std::set<std::string> dataOff;        // data decoders the owner switched off (all are on by default)
    std::set<std::string> restrictedOn;   // voice / pager / other decoders switched on (needs legalAck)
    bool legalAck = false;
    std::string demods = "am,nfm,wfm,usb,lsb,cw";   // modes listeners may use
    int maxUsers = 8;
    int userKbps = 0;                 // 0: the server's default (512)
    bool audioOnly = false;
    bool limitToRange = false;        // listeners may tune only inside the shared range (web_allow_range)
    std::string password;             // empty: open to everyone who has the link
    std::string link = "tunnel";      // tunnel: a public address through the free Cloudflare quick tunnel; own: an https address you set up (a reverse proxy on this computer); lan: this network only
    std::string ownUrl;               // link == "own"
    // step 2
    bool listPublic = false;
    std::string name = "OnAir Web", desc, loc, contact, radioLabel, dirUrl = "https://web.on-air.dev";
    int locDecimals = 2;              // 0: about 100 km, 1: about 10 km, 2: about 1 km
    bool reach = false;               // the directory may check once an hour that the server answers
};
bool settingsValid(const ShareSettings& s, std::string& why);   // lengths, characters, numbers
std::string settingsToText(const ShareSettings& s);
ShareSettings settingsFromText(const std::string& text);        // unknown / bad lines keep the defaults

// What will actually be captured
struct Plan {
    std::string device;               // the radio's name as onair-server wants it
    double centreHz = 0, rateHz = 0;  // the capture
    double loHz = 0, hiHz = 0;        // what listeners can see and tune (96 % of the rate)
    std::string warning;              // empty, or a plain sentence (a centre outside the radio's range, a rate above what it reports)
};
double defaultCentreMHz(const RigCaps& c);                     // FM broadcast when the radio covers it, else the middle of its range
Plan resolvePlan(const ShareSettings& s, const RigCaps& rig);
void applyPreset(ShareSettings& s, const RangePreset& p);

// ---------------------------------------------------------------- decoders
struct DecoderRow {
    std::string kind, label, group;   // group: "Listening modes", "Data decoders", "Needs your decision (legal notice)"
    int cpu = 1;                      // 1 (light) to 5 (heavy): rough cost of one running decoder
    bool needsCodec = false;          // voice codec the user installs (docs/CODECS.md)
    bool restricted = false;          // voice / pager / other: off unless the owner accepts the legal notice
    bool isDemod = false;
    bool possible = true;             // the chosen range contains the band this decoder works in
    std::string why;                  // when not possible: the band it needs
    bool on = false;
};
std::vector<DecoderRow> decoderRows(const ShareSettings& s, const Plan& plan);
// A short cost summary: "about 2 cores for 8 listeners, all decoders" etc. from the rows that are on.
int cpuScore(const std::vector<DecoderRow>& rows);
// group toggles
void setGroup(ShareSettings& s, const std::string& group, bool on);

// ---------------------------------------------------------------- onair-server's settings
// The sharing server's settings file. publicUrl empty while there is no address yet.
std::string buildServerConf(const ShareSettings& s, const Plan& plan, int port, int apiPort, const std::string& publicUrl, const std::string& adminToken);
// The listing program's settings file (preview and heartbeat): the same name / address / radio / coverage as the server, the directory, and the confirmation code.
std::string buildDirectoryConf(const ShareSettings& s, const Plan& plan, int port, const std::string& publicUrl, const std::string& ack, const std::string& keyFile);
std::string coverageText(const Plan& p);                       // "88-108" in MHz, what the listing says the server covers

// ---------------------------------------------------------------- step 2 (the listing) as a state machine
enum class Step2 { Off, Previewing, PreviewReady, PreviewFailed, Listing, Listed, ListFailed };
struct Problem { std::string code, message, advice; bool temporary = false; };
struct Step2State {
    Step2 s = Step2::Off;
    std::string preview, ack;
    Problem problem;                  // the last problem (also while it is still being retried)
    long long fails = 0, lastOk = 0, lastTry = 0, nextTry = 0, startedAt = 0;
};
// the output of `onair-server --directory-preview`: "ack=CODE\n---\nTEXT" or "error=TEXT"
void step2PreviewResult(Step2State& st, const std::string& output, bool exitedOk);
void step2Confirm(Step2State& st, long long now);               // PreviewReady -> Listing
// the contents of web_directory_status.json (empty: not written yet); `alive` = the listing program still runs
void step2Status(Step2State& st, const std::string& statusJson, bool alive, long long now);
void step2Cancel(Step2State& st);                               // back to Off
const char* step2Name(Step2 s);

// one line of `onair-server --directory-test`
struct TestStep { std::string step, code, message, advice; bool ok = false; long ms = 0; bool done = false; };
bool parseTestLine(const std::string& line, TestStep& out);
const char* testStepTitle(const std::string& step);             // "Directory", "Web server on this computer", "Your public address"

// ---------------------------------------------------------------- small helpers shared with the window
std::string linkQrText(const std::string& url);                  // the text the QR code encodes (the link as it is)
struct ShareStats { int users = 0, maxUsers = 0; double kbps = 0, maxKbps = 0, cpuPct = 0, centreHz = 0, rateHz = 0; long long iqDropped = 0; bool ok = false; };
bool parseStats(const std::string& json, ShareStats& out);       // GET /web/v1/admin/stats
std::string rangeSpecMHz(double loHz, double hiHz);

} // namespace webshare
