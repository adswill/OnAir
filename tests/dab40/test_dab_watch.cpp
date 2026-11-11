#include "dect2/dab/dab_watch.h"
#include <cstdio>
using namespace dect2::dabwatch;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); fails++; } }
static Obs ens(std::vector<Svc> s, bool active = true, uint16_t eid = 0xC181, const char* name = "BBC National DAB") {
    Obs o; o.channel = "12B"; o.active = active; o.eid = eid; o.ensemble = name; o.services = std::move(s); return o;
}
static std::vector<Svc> base() { return {{0xC221, "BBC Radio 2", true, 128}, {0xC222, "BBC Radio 3", true, 128}, {0xC223, "BBC Radio 4", true, 96}}; }
static int count(const std::vector<Event>& e, Kind k) { int n = 0; for (auto& x : e) if (x.kind == k) n++; return n; }
int main() {
    Watch w;
    double t = 0;
    // learning: the FIC delivers one service at a time; no added-service events
    std::vector<Event> all;
    auto b = base();
    for (size_t n = 1; n <= 3; n++) { auto ev = w.update(t += 5, {ens({b.begin(), b.begin() + (long)n})}); all.insert(all.end(), ev.begin(), ev.end()); }
    check(count(all, Kind::EnsembleAppeared) == 1 && all.size() == 1, "appearance reported once, growth while learning is silent");
    for (int i = 0; i < 10; i++) { auto ev = w.update(t += 5, {ens(b)}); check(ev.empty(), "stable ensemble stays quiet"); }
    // a weak-signal glitch: one update with a service missing, then back: no event
    auto glitch = b; glitch.pop_back();
    check(w.update(t += 5, {ens(glitch)}).empty() && w.update(t += 5, {ens(b)}).empty() && w.update(t += 5, {ens(glitch)}).empty() && w.update(t += 5, {ens(b)}).empty(), "single-update glitches are not reported");
    // real removal: confirmed after 3 updates
    int removed = 0;
    for (int i = 0; i < 5; i++) removed += count(w.update(t += 5, {ens(glitch)}), Kind::ServiceRemoved);
    check(removed == 1, "a removed service is reported exactly once");
    // add + codec change together
    auto next = glitch;
    next.push_back({0xC300, "BBC Radio 5", true, 64});
    next[0].bitrate = 80; next[0].dabPlus = true;
    int added = 0, changed = 0;
    for (int i = 0; i < 5; i++) { auto ev = w.update(t += 5, {ens(next)}); added += count(ev, Kind::ServiceAdded); changed += count(ev, Kind::ServiceChanged); }
    check(added == 1 && changed == 1, "added service and bit rate change reported once each");
    // ensemble id/label change
    int ec = 0;
    for (int i = 0; i < 5; i++) ec += count(w.update(t += 5, {ens(next, true, 0xC182, "BBC National 2")}), Kind::EnsembleChanged);
    check(ec == 1, "ensemble change reported once");
    // goes away: not before goneSec, then once
    int gone = 0;
    for (int i = 0; i < 4; i++) gone += count(w.update(t += 20, {ens({}, false)}), Kind::EnsembleGone);
    check(gone == 0, "short loss of signal is not 'gone'");
    for (int i = 0; i < 5; i++) gone += count(w.update(t += 20, {ens({}, false)}), Kind::EnsembleGone);
    check(gone == 1, "gone reported once after goneSec");
    // comes back
    auto ev = w.update(t += 20, {ens(next, true, 0xC182, "BBC National 2")});
    check(count(ev, Kind::EnsembleAppeared) == 1 && ev[0].text.find("back") != std::string::npos, "back reported as returning");
    // two channels independent
    Watch w2;
    Obs a = ens(base()); a.channel = "11A"; Obs c = ens({{1, "X", true, 64}}); c.channel = "12C";
    double tt = 0;
    int app = 0;
    for (int i = 0; i < 8; i++) app += count(w2.update(tt += 5, {a, c}), Kind::EnsembleAppeared);
    check(app == 2, "channels tracked separately");
    check(w2.log().size() == 2, "log keeps the events");
    std::printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
