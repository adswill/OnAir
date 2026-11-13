// Map badges, the app side (docs/MAP-BADGES-4.0.md). One OnAir GitHub sign-in serves the plugin marketplace and the live map: the
// marketplace's device flow (core/src/pstore/market.cpp) writes plugins/.market/session.json, and the map share reads the same file and
// sends the token as "Authorization: Bearer ..." with its posts, only while the user has "Earn badges" on. Signed out, or with badges off,
// the post is exactly what it was before. The server keeps the stats; this file only reads the session, builds the extra request bits and
// reads the server's answers for the toast and the Badges panel. Pure (files and strings only), covered by tests/badges/test_badges_client.cpp.
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace dect2::badges {

struct Session {
    std::string token, login;
    int64_t expires = 0;                 // seconds since 1970
    bool valid(int64_t nowSec) const { return !token.empty() && !login.empty() && expires > nowSec; }
};

// The marketplace sign-in from <stateDir>/session.json (stateDir = <data>/plugins/.market). Empty when missing, expired or malformed, or when
// the token has characters a session token cannot have (so nothing odd ever reaches an HTTP header).
Session readSession(const std::string& marketStateDir, int64_t nowSec);
// "Authorization: Bearer <token>" for a valid session, "" otherwise.
std::string authHeader(const Session& s, int64_t nowSec);
// The 6-character Maidenhead locator of a position ("" when it is not a real position). Sent as "home" for range badges only when the user
// ticked that option; the server uses it for that post and stores no position.
std::string locator6(double lat, double lon);

struct Award { std::string id, name, icon; int tier = 0; };
// The account part of an /ingest answer ({"account":{...},"awards":[...],"account_error":"..."}).
struct IngestAccount {
    bool present = false;                // the server knew the sign-in
    std::string login, error, note;      // error: the token was refused (sign in again); note: why this post earned nothing ("duplicate", ...)
    bool badgesOn = false, isPublic = false;
    std::vector<Award> awards;           // badges earned with this post
};
IngestAccount parseIngestReply(const std::string& body);

// One line per new badge for the toast: "Badge earned: Plane spotter (Gold)".
std::string toastText(const Award& a);

// Awards waiting for the toast. Thread-safe (the post thread pushes, the UI pops). An award (id and tier) is shown once, also across restarts
// when a file is given: the ids shown are kept there.
class ToastQueue {
public:
    explicit ToastQueue(std::string file = {});
    void push(const std::vector<Award>& awards);
    bool pop(Award& out);
    size_t pending() const;
private:
    void load();
    void save() const;
    mutable std::mutex mu_;
    std::string file_;
    std::deque<Award> q_;
    std::set<std::string> shown_;
    bool loaded_ = false;
};

// The Badges panel's model, from GET /api/me.
struct BadgeRow {
    std::string id, name, desc, icon, unit;
    int tier = 0, maxTier = 0;
    int64_t earnedAt = 0;                // ms of the newest tier, 0 = not earned
    double value = 0, next = 0, frac = 0;   // next: the next tier's threshold (0 = top tier reached); frac: progress to it, 0..1
    std::string progress;                // "150 of 1,000" or "Top tier"
};
struct Profile {
    bool ok = false, signedIn = false, configured = true, badgesOn = false, isPublic = false;
    std::string login, by, error;
    int apps = 0;
    double hours = 0, rangeKm = 0;
    int64_t days = 0, streak = 0, bestStreak = 0, objects = 0, countries = 0;
    int64_t aircraft = 0, ships = 0, sondes = 0, aprs = 0, sats = 0, ham = 0, fixes = 0, acars = 0;
    std::vector<BadgeRow> badges;        // earned first (highest tier, newest), then the rest by progress
    int earnedCount() const;
};
Profile parseMe(const std::string& body);
// Body for POST /api/me/settings; -1 leaves a flag as it is, by == nullptr leaves the name.
std::string settingsBody(int badges, int isPublic, const std::string* by);
// https://map.on-air.dev/u/<login> (origin without a trailing slash).
std::string profileUrl(const std::string& origin, const std::string& login);
// "1,234" / "3.5" / "321 km": the panel's numbers.
std::string fmtValue(double v, const std::string& unit);
// "Bronze", "Silver", "Gold", "Platinum" ("" for 0).
const char* tierName(int tier);

// What the Badges panel asks the user to do next (one step at a time, the one-button way).
enum class PanelStep { NotConfigured, SignIn, TurnOnBadges, TurnOnSharing, Earning };
// session: the marketplace sign-in is valid; me: the last /api/me answer (nullptr: none yet); earn: the app's "Earn badges" choice;
// sharing: Server > Send data to map server is on.
PanelStep panelStep(bool session, const Profile* me, bool earn, bool sharing);
const char* panelStepText(PanelStep s);
// The map post carries the token only when the user chose to earn badges and the sign-in is valid.
bool sendToken(bool earn, const Session& s, int64_t nowSec);

} // namespace dect2::badges
