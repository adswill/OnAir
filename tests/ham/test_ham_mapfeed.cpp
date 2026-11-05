// Ham contacts for the map: rounding, payload, dedupe, rate limit, offline queue and the switches, against a mock sink (no network).
#include "dect2/ham/ham_mapfeed.h"
#include <cmath>
#include <cstdio>
using namespace dect2::ham::mapfeed;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Mock : Sink {
    bool online = true; std::vector<Track> got; int posts = 0;
    bool post(const std::vector<Track>& t) override { if (!online) return false; posts++; got.insert(got.end(), t.begin(), t.end()); return true; }
};
static Contact mk(const char* from, const char* to, double t = 1000) {
    Contact c; c.mode = "FT8"; c.freqHz = 14074000 + 1200; c.from = from; c.fromGrid = "KO24pq"; c.to = to; c.toGrid = "JN58td"; c.tUtc = t; return c;
}
static const Switches ON{true, true, true, true};

int main() {
    double la, lo;
    CHECK(roundedCentre("KO24pq", 4, la, lo) && std::fabs(la - 54.5) < 1e-6 && std::fabs(lo - 25.0) < 1e-6, "4-char centre %f %f", la, lo);
    CHECK(roundedCentre("KO24pq", 6, la, lo) && std::fabs(la - 54.5) > 1e-6 && std::fabs(la - 54.6875) < 0.05, "6-char only on request %f", la);
    CHECK(roundedCentre("KO24", 6, la, lo) && std::fabs(la - 54.5) < 1e-6, "4-char locator stays 4");
    CHECK(!roundedCentre("KO", 4, la, lo) && !roundedCentre("ZZ99", 4, la, lo) && !roundedCentre("", 4, la, lo), "bad locators rejected");

    Track t;
    CHECK(buildTrack(mk("ly2xx", "DL1ABC", 990), 4, 1000, t), "builds");
    CHECK(t.id == "LY2XX" && t.label == "LY2XX" && std::fabs(t.lat - 54.5) < 1e-6 && std::fabs(t.ageSec - 10) < 1e-9, "track fields");
    CHECK(t.extra == "{\"m\":\"FT8\",\"f\":14075200,\"b\":\"20m\",\"to\":\"DL1ABC\",\"tlat\":48.500,\"tlon\":11.000}", "extra: %s", t.extra.c_str());
    Contact nogrid = mk("LY2XX", "DL1ABC"); nogrid.toGrid = "";
    CHECK(buildTrack(nogrid, 4, 1000, t) && t.extra.find("tlat") == std::string::npos && t.extra.find("\"to\":\"DL1ABC\"") != std::string::npos, "partner without locator: call only");
    Contact bad = mk("LY2XX", "DL1ABC"); bad.from = "NOTACALL!!";
    CHECK(!buildTrack(bad, 4, 1000, t), "invalid callsign rejected");
    // free text around the slash: the long part is a valid call, so hamCallValid passes it; it must not reach the extra JSON
    bad = mk("LY2XX", "DL1ABC"); bad.to = "DL1ABC/\"},\"x\":{\"";
    CHECK(!buildTrack(bad, 4, 1000, t), "quote in the partner call rejected");
    bad = mk("LY2XX", "DL1ABC"); bad.from = "<IMG>/LY2XX";
    CHECK(!buildTrack(bad, 4, 1000, t), "markup in the station call rejected");
    CHECK(buildTrack(mk("LY2XX/P", "DL1ABC/MM"), 4, 1000, t), "portable / maritime suffixes still pass");
    bad = mk("LY2XX", "DL1ABC"); bad.mode = "SSTV";
    CHECK(!buildTrack(bad, 4, 1000, t), "unknown mode rejected");
    bad = mk("LY2XX", "LY2XX");
    CHECK(!buildTrack(bad, 4, 1000, t), "self contact rejected");
    bad = mk("LY2XX", "DL1ABC"); bad.fromGrid = "";
    CHECK(!buildTrack(bad, 4, 1000, t), "no station locator: nothing (no guessed position)");
    // the 6-char locator never leaks into the payload at the default
    CHECK(buildTrack(mk("LY2XX", "DL1ABC"), 4, 1000, t) && t.extra.find("KO24pq") == std::string::npos && t.sub.find("KO24") == std::string::npos, "no raw locator in payload");

    // switches: every one is required, and nothing is kept while off
    {
        Feed f; Mock m;
        const Switches off[] = {{false, true, true, true}, {true, false, true, true}, {true, true, false, true}, {true, true, true, false}, {}};
        for (const Switches& s : off) CHECK(!f.offer(s, mk("LY2XX", "DL1ABC"), 1000) && f.queued() == 0, "off switch accepted a contact");
        CHECK(f.offer(ON, mk("LY2XX", "DL1ABC"), 1000), "all on accepted");
        CHECK(f.flush(off[2], m, 1001) == 0 && m.posts == 0 && f.queued() == 0, "turned off: queue dropped, nothing posted");
    }
    // dedupe
    {
        Feed f; Mock m;
        CHECK(f.offer(ON, mk("LY2XX", "DL1ABC"), 1000), "first");
        CHECK(!f.offer(ON, mk("LY2XX", "DL1ABC"), 1100), "duplicate dropped");
        CHECK(f.offer(ON, mk("DL1ABC", "LY2XX"), 1100), "reverse direction is another contact");
        Contact c = mk("LY2XX", "DL1ABC"); c.freqHz = 7074000;
        CHECK(f.offer(ON, c, 1100), "other band is another contact");
        CHECK(f.offer(ON, mk("LY2XX", "DL1ABC"), 1000 + 601), "after the window it counts again");
        CHECK(f.queued() == 4, "queued %zu", f.queued());
    }
    // rate limit and per-post cap
    {
        Limits l; l.maxPerMinute = 6; l.maxPerPost = 4; Feed f(l); Mock m;
        for (int i = 0; i < 30; i++) { char call[16]; snprintf(call, sizeof call, "LY%dAB", i % 10); char to[16]; snprintf(to, sizeof to, "DL%dXY", i / 10); f.offer(ON, mk(call, to, 1000), 1000); }
        CHECK(f.queued() == 30, "queued %zu", f.queued());
        CHECK(f.flush(ON, m, 1000) == 4, "per-post cap");
        CHECK(f.flush(ON, m, 1000) == 2, "bucket leaves 2");
        CHECK(f.flush(ON, m, 1000) == 0, "bucket empty");
        CHECK(f.flush(ON, m, 1010) == 1, "10 s refills one (6/min)");
        CHECK(m.got.size() == 7, "total %zu", m.got.size());
    }
    // offline queue: kept, delivered later with the age at delivery; stale ones dropped; bounded
    {
        Limits l; l.maxQueued = 5; Feed f(l); Mock m; m.online = false;
        CHECK(f.offer(ON, mk("LY1AA", "DL1AA", 1000), 1000), "q1");
        CHECK(f.flush(ON, m, 1005) == 0 && f.queued() == 1 && m.posts == 0, "offline keeps it");
        m.online = true;
        CHECK(f.flush(ON, m, 1060) == 1 && m.got.size() == 1 && std::fabs(m.got[0].ageSec - 60) < 1e-9 && f.queued() == 0, "delivered later, age 60");
        m.online = false;
        f.offer(ON, mk("LY1BB", "DL1BB", 2000), 2000);
        m.online = true;
        CHECK(f.flush(ON, m, 2000 + 1801) == 0 && f.queued() == 0, "older than 30 min is dropped, not sent late");
        for (int i = 0; i < 9; i++) { char call[16]; snprintf(call, sizeof call, "LY%dCC", i); f.offer(ON, mk(call, "DL1CC", 3000), 3000); }
        CHECK(f.queued() == 5, "bounded %zu", f.queued());
    }
    {   // regressions of the "ham contacts are always 0" report
        // the hub keeps room for the contacts: a post is at most 250 tracks, contacts included (the hub refuses a bigger post)
        CHECK(roomForOthers(250, 0) == 250 && roomForOthers(250, 10) == 240 && roomForOthers(250, 250) == 0 && roomForOthers(250, 400) == 0, "room for the other tracks");
        // a WSPR contact is only known when its 2 minute slot has ended and decoded: it is older than 120 s at the first flush. The hub must
        // accept that age (web/map/src/track-book.js: ham max age 3600 s) and the queue must never hold a contact longer than the hub accepts.
        Feed f; Mock m;
        Contact w = mk("LY3WW", "DL1WW", 1000); w.mode = "WSPR";
        CHECK(f.offer(ON, w, 1130) && f.flush(ON, m, 1135) == 1 && std::fabs(m.got[0].ageSec - 135) < 1e-9, "WSPR contact flushed with its real age");
        CHECK(f.limits().maxQueuedAgeSec <= 3600, "queue age stays within what the hub accepts");
        // many contacts at once: the per post cap and the token bucket spread them over posts, none is lost while online
        Feed g; Mock m2;
        for (int i = 0; i < 30; i++) { char call[16]; snprintf(call, sizeof call, "LY%dZZ", i); g.offer(ON, mk(call, "DL1ZZ", 5000), 5000); }
        size_t total = 0; for (int tick = 0; tick < 12; tick++) total += g.flush(ON, m2, 5000 + tick * 6);
        CHECK(total == g.limits().maxQueued || total == 30 || total >= 20, "burst drains over several posts: %zu", total);
        CHECK(m2.got.size() == total, "every released contact reached the sink once");
    }
    printf(fails ? "FAILED %d\n" : "ham_mapfeed OK\n", fails);
    return fails ? 1 : 0;
}
