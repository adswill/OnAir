// OnAir Web directory: the pure part of the heartbeat. What is sent, in which exact form, and what is signed. No network, no threads, so it is
// tested on its own (tests/server/test_web_directory.cpp). The Worker side is web/onair-directory/src/logic.js; both build the same signing
// text and a shared test vector keeps them identical (docs/ONAIR-DIRECTORY.md describes the protocol).
#pragma once
#include "web_directory.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace onair_web {

struct DirRecord {
    std::string action = "heartbeat";                   // "heartbeat" or "delete"
    std::string id, pub;                                // id: "onair-" + 16 hex of SHA-256(pub); pub: Ed25519 public key (hex)
    long long ts = 0;                                   // unix seconds
    std::string name, url, loc;                         // url: https://host[:port]/path/ ; loc: "lat,lon" or a Maidenhead locator, or empty
    std::vector<std::pair<long long, long long>> freq;  // coverage in Hz
    std::string radio;
    long long cap = 0, users = 0, interval = 300;       // listener capacity, current users (a count only), seconds between beats
    std::string ver, contact, desc;
    int reach = 0;                                      // 1: the directory may check /web/v1/info once an hour
};

// The text that is signed: "OnAir-Directory-v1\n" then key=value lines in a fixed order (the delete action has only action, id, pub, ts).
std::string dirCanonical(const DirRecord& r);
std::string dirFreqText(const std::vector<std::pair<long long, long long>>& f);

// The JSON body of the request (sigHex goes in as the "sig" field), keys in the same order as the signing text.
std::string dirBodyJson(const DirRecord& r, const std::string& sigHex);

// Ed25519 over dirCanonical(r): the signature as 128 hex characters; verify returns false for any wrong key, text or signature.
std::string dirSign(const uint8_t seed[32], const DirRecord& r);
bool dirVerify(const std::string& pubHex, const std::string& sigHex, const DirRecord& r);
std::string dirPubHex(const uint8_t seed[32]);
std::string dirIdFromPub(const std::string& pubHex);   // "onair-" + first 16 hex characters of SHA-256 of the 32 key bytes

// Field cleaning: control characters removed, trimmed, cut to `maxBytes` at a UTF-8 boundary.
std::string dirClean(const std::string& s, size_t maxBytes);
// https://host[:port][/path] to the normal form the Worker wants; false (err set) for http, user names, queries, spaces, a long text.
bool dirNormalizeUrl(const std::string& in, std::string& out, std::string& err);
// "54.6931,25.2797" to "54.69,25.28" (decimals 0..5, the host's choice); a 4/6/8 character locator is validated and upper/lower-cased; "" stays "".
bool dirNormalizeLoc(const std::string& in, int decimals, std::string& out, std::string& err);
// "88-108,1.0-30" (MHz ranges) to Hz ranges; false on a bad range
bool dirParseFreqMHz(const std::string& in, std::vector<std::pair<long long, long long>>& out, std::string& err);

// The record built from the configuration and the live /web/v1/info JSON. The key is not needed here (id and pub come from the caller).
// Coverage: web_directory_freq when set, else the capture window (centre +- rate/2). Returns false with err when something is not valid.
bool dirBuildRecord(const DirectoryConfig& c, const std::string& infoJson, long long ts, const std::string& id, const std::string& pubHex, DirRecord& out, std::string& err);

// The confirmation code of the fields the host chose. Covers everything that is sent except the things that change by themselves (time,
// current users, coverage taken from the capture, software version). Changing name, address, location, radio, contact, description or the
// reachability switch changes the code, so the host has to look at the new preview before anything is sent again.
std::string dirAck(const DirRecord& r, const std::string& directoryUrl);

// What the server prints before it will send anything: the exact JSON body (signature shown as a placeholder), where it goes, and the code.
std::string dirPreview(const DirRecord& r, const std::string& directoryUrl);

// The directory's own address (web_directory_url): https://host[:port]/path/ ; plain http is accepted only for this computer (127.0.0.1, localhost, [::1]),
// which is how the tests talk to a fake directory. Anything else must be https.
bool dirNormalizeEndpoint(const std::string& in, std::string& out, std::string& err);

// ---- what happened, in words a person can act on (no network here, tested in tests/server/test_web_directory.cpp) ------------------------------
struct DirOutcome {
    bool ok = false;
    int http = 0;               // the HTTP status when there was an answer
    std::string code;           // short stable name: ok, dns, refused, timeout, tls_cert, tls, no_tls, no_curl, clock, invalid, key, blocked, not_found, conflict, resolving, rate, full, server, other
    std::string message;        // what failed and why
    std::string advice;         // what to do about it
    bool retrySoon = false;     // the failure is probably temporary (a fresh address that is not in DNS yet): try again in seconds, not minutes
};
// An answer from the directory. skewSec = the directory's clock minus ours (from its Date header); haveSkew false when it sent none.
DirOutcome dirClassifyHttp(int status, const std::string& body, long long skewSec, bool haveSkew);
// No answer. errClass: dns, refused, timeout, tls_cert, tls, no_tls, no_curl, proto, other; detail: the raw text, kept short.
DirOutcome dirClassifyNet(const std::string& errClass, const std::string& detail, const std::string& host);
// The check of this server's own public address (what a listener's browser would do): status 0 = no answer (errClass as above).
DirOutcome dirClassifyPublic(int status, bool bodyLooksRight, const std::string& errClass, const std::string& url);

// Seconds to wait after the `fails`-th failure in a row (1 = the first): a failure that is probably temporary (a new address not in DNS yet) is retried in
// seconds; others back off up to 5 minutes; never more than the beat interval, never under 5 s.
int dirRetryDelay(int fails, bool soon, int intervalSec);
// "Sat, 31 Oct 2026 10:00:00 GMT" to unix seconds; false when it is not that form.
bool dirParseHttpDate(const std::string& s, long long& unixSec);

// The status file the heartbeat keeps up to date (<key file folder>/web_directory_status.json); the app reads it, nothing blocks on it.
struct DirStatus {
    std::string state = "idle";   // idle, sending, ok, error
    DirOutcome last;              // the last attempt
    long long attempts = 0, fails = 0, lastTry = 0, lastOk = 0, nextTry = 0, skewSec = 0;
    bool haveSkew = false;
    std::string id, url, directory;
};
std::string dirStatusJson(const DirStatus& s);
bool dirStatusParse(const std::string& json, DirStatus& out);

} // namespace onair_web
