// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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
    std::string password;             // only while it is being typed (kept in memory, never written to disk); empty: unchanged
    std::string passwordHash;         // what is saved and given to the server (pbkdf2$...): the plain password is on no disk
    bool accessPassword = false;      // the access mode "Password" (needs passwordHash)
    std::string link = "auto";        // auto (default): the OnAir relay if it answers, else a Cloudflare tunnel, so one button works everywhere; relay: a public address through the OnAir relay service (an outbound connection, no helper program, no port forwarding); tunnel: the free Cloudflare quick tunnel; own: an https address you set up (a reverse proxy on this computer); lan: this network only
    std::string relayUrl = "wss://relay.on-air.dev/relay/v1/host";   // link == "relay": the relay service (a different one can be set under Advanced)
    // access and safety
    bool inviteOnly = false;          // listeners need an invite link made in this window (overrides password)
    std::string control = "everyone"; // who may tune or change mode: everyone, members (password / invite), slot (one person at a time), nobody
    int controlSlotMin = 5;           // control == "slot": how long one person holds it
    int audioRate = 0;                // audio quality cap in Hz (0: the listener's choice, up to the bit-rate limit)
    std::string allowRanges;          // extra frequency limits for listeners: comma separated, e.g. "88-108 MHz, 118-137 MHz"
    std::string blockRanges;          // ranges listeners may never tune
    bool allowTx = false;             // listeners may send transmit requests (off by default; see the warning in Advanced)
    bool bindAll = false;             // link == "own": listen on every interface (direct port forwarding), not only this computer
    bool autostart = false;           // share when OnAir starts, with the same choices
    bool autoList = false;            // autostart also lists it again (the owner has confirmed the listing once)
    int maxPerIp = 2;                 // listeners from one address
    int cpuCapPct = 75;               // new listeners are refused above this share of the computer's processor
    int maxKbpsTotal = 0;             // all listeners together, kbit/s (0: the server's default, 8000)
    int idleMin = 0;                  // minutes without action before a listener is dropped (0: the server's default, 30)
    int sessionMin = 0;               // longest session of one listener in minutes (0: no limit)
    // the server
    int port = 0;                     // the web server's port on this computer (0: the first free from 8073)
    std::string tlsCert, tlsKey;      // link == "own" or "lan": the server's own certificate (files); empty: none
    int maxBins = 0;                  // the biggest spectrum row a listener may ask for (0: the server's default, 1024)
    int maxFps = 0;                   // the most spectrum rows per second (0: the server's default)
    bool autoApply = true;            // changes made while sharing are applied by themselves after a moment
    std::string antenna;              // shown to listeners (the page header)
    std::string ownUrl;               // link == "own"
    // step 2
    bool listPublic = false;
    std::string name = "OnAir Web", desc, loc, contact, radioLabel, dirUrl = "https://web.on-air.dev";
    int locDecimals = 1;              // 0: about 100 km, 1: about 10 km (default: coarse), 2: about 1 km ...
    bool reach = false;               // the directory may check once an hour that the server answers
};
bool settingsValid(const ShareSettings& s, std::string& why);   // lengths, characters, numbers
// The app's own location as a listing location, already coarse ("54.7,25.3"); empty when the app has none (lat and lon both 0)
std::string locationText(double lat, double lon, int decimals);
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

// ---------------------------------------------------------------- listeners (admin API) and the relay's status
struct UserRow { int id = 0; std::string name, cls, address, mode; double freqHz = 0, kbps = 0, cpuPct = 0; long long seconds = 0; };
std::vector<UserRow> parseUsers(const std::string& json);        // GET /web/v1/admin/users (not limited to 4 KB like jsonParse; at most 200 rows)
struct BanRow { std::string address; int minutesLeft = 0; };
std::vector<BanRow> parseBans(const std::string& json);          // GET /web/v1/admin/bans
std::string kickBody(int id, const std::string& reason);         // POST admin/kick
std::string banBody(int id, int minutes, const std::string& reason);   // POST admin/ban
std::string unbanBody(const std::string& address);               // POST admin/unban
std::string inviteBody(const std::string& label, int maxMinutes);    // POST admin/invite
std::string inviteLink(const std::string& url, const std::string& token);   // the link a listener opens: <url>/#invite=TOKEN (the token never reaches the server's logs)
bool parseInviteToken(const std::string& json, std::string& token);  // the answer of admin/invite
// What the "Share my radio" button will do, in plain words ("HackRF Pro, 88.0-108.0 MHz, up to 8 listeners, through the OnAir relay, open to anyone with the link")
std::string easySummary(const ShareSettings& s, const RigCaps& rig, const Plan& plan);
// The relay's status as one plain sentence for the window (state, retry, problem); empty when there is no relay.
struct RelayView { std::string state, url, code, message, advice; int conns = 0; long long retryIn = 0, bytesIn = 0, bytesOut = 0; };
bool parseRelayStatus(const std::string& json, RelayView& out);
std::string relayLine(const RelayView& r);

// ---------------------------------------------------------------- small helpers shared with the window
std::string linkQrText(const std::string& url);                  // the text the QR code encodes (the link as it is)
struct ShareStats { int users = 0, maxUsers = 0; double kbps = 0, maxKbps = 0, cpuPct = 0, centreHz = 0, rateHz = 0; long long iqDropped = 0; bool ok = false; };
bool parseStats(const std::string& json, ShareStats& out);       // GET /web/v1/admin/stats
std::string rangeSpecMHz(double loHz, double hiHz);

} // namespace webshare
