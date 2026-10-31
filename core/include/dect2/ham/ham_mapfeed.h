// Ham contacts for the public map (opt-in, its own choice next to "Send data to map server"). Pure logic, no network and no clock of its own:
// the app offers decoded contacts, the feed rounds the locators, deduplicates, rate limits and queues them, and a Sink takes what is due
// (the app's sink adds them to the ingest post as "ham" tracks; tests use a mock). Sent per contact: the station's callsign, the mode, the
// band and frequency, the partner's callsign, both positions as the CENTRE of the locator square (4 characters by default, 6 only when the
// user chose more) and the time. Nothing else: no logbook notes, no signal reports, no QRZ data, no exact location.
#pragma once
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2::ham::mapfeed {

struct Contact {
    std::string mode;                 // FT8 FT4 WSPR JS8 or QSO
    double freqHz = 0;                // RF frequency (dial + audio offset), 0 unknown
    std::string from, fromGrid;       // the station the track is drawn at: its callsign and locator
    std::string to, toGrid;           // the partner
    double tUtc = 0;                  // UTC seconds when heard / logged
};

struct Track {                        // what one "ham" track of the ingest post carries
    std::string id, label, sub, extra;   // extra: JSON text {"m","f","b","to","tlat","tlon"} (the site's hints)
    double lat = 0, lon = 0, ageSec = 0;
};

struct Limits {
    int gridChars = 4;                // 4 (default) or 6; anything else becomes 4
    double dedupeSec = 600;           // the same from/to/band/mode again inside this is dropped
    double maxPerMinute = 20;         // tracks released per minute (token bucket, burst = maxPerMinute)
    size_t maxPerPost = 10;           // tracks released per flush
    size_t maxQueued = 100;           // oldest dropped beyond this
    double maxQueuedAgeSec = 1800;    // queued longer than this: dropped, not sent late
};

// A contact is accepted only when ALL are true: the share-positions master switch and its consent, and the ham contacts choice and its consent.
struct Switches { bool master = false, masterConsent = false, ham = false, hamConsent = false; };
inline bool allowed(const Switches& s) { return s.master && s.masterConsent && s.ham && s.hamConsent; }

// Centre of the locator square cut to 4 or 6 characters; false when it is not a valid 4 or 6 character locator.
bool roundedCentre(const std::string& grid, int chars, double& lat, double& lon);
// One track out of one contact, or false (invalid callsign, no usable station locator, unknown mode, no partner).
bool buildTrack(const Contact& c, int gridChars, double nowUtc, Track& out);
// The posted JSON text of the extra field.
std::string extraJson(const std::string& mode, double freqHz, const std::string& to, bool hasPos, double tlat, double tlon);
std::string dedupeKey(const Contact& c);

// How many of the other tracks of a post may stay when `hamCount` contact tracks must go in, under the hub's limit of `cap` tracks per post.
inline size_t roomForOthers(size_t cap, size_t hamCount) { return hamCount >= cap ? 0 : cap - hamCount; }

struct Sink { virtual ~Sink() = default; virtual bool post(const std::vector<Track>& tracks) = 0; };   // false: not delivered (offline); kept queued

class Feed {
public:
    explicit Feed(const Limits& l = Limits()) : l_(l) {}
    Limits& limits() { return l_; }
    // Queues a contact; false when switched off, invalid or a duplicate. Nothing is kept while switched off.
    bool offer(const Switches& sw, const Contact& c, double nowUtc);
    // Releases what is due (rate limit, per post cap, age) to the sink; kept queued when the sink says it is offline. Returns the number delivered.
    size_t flush(const Switches& sw, Sink& sink, double nowUtc);
    size_t queued() const { return q_.size(); }
    void clear() { q_.clear(); seen_.clear(); }
private:
    struct Item { Track t; double tUtc = 0; };
    Limits l_;
    std::deque<Item> q_;
    std::map<std::string, double> seen_;   // dedupe key -> time
    double tokens_ = -1, lastTok_ = 0;
};

}  // namespace dect2::ham::mapfeed
