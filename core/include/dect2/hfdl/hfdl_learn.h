// HFDL ground-station frequency learning (3.7, docs/SMART-maritime-3.7.md). The receiver reads the frequency masks of the squitters against a built-in
// station table (hfdl_proto.h); the table the ground stations really use is the HFDL system table (HFNPDU 0xD0) that changes now and then, and the
// squitter carries its version number. This keeps what the air says, so the plan follows the stations without a table file to maintain:
//  - a station heard directly on a frequency that its table does not list: learned (after two squitters),
//  - a station whose system table version changed: the masks cannot be trusted any more (they index the old table), so the frequencies it was
//    actually heard on take the place of the announcements ("drift"), until the table is the one built in again,
//  - a built-in frequency that the station no longer uses: dropped ("retired") when, since the version change, this receiver listened to that
//    frequency for two hours in total, heard the station on other frequencies at least three times and never on this one.
// Pure logic, no clock of its own (the caller passes UNIX seconds), thread safe, saved between runs as text.
#pragma once
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

class HfdlFreqLearner {
public:
    struct Config {
        int confirmHits = 2;              // squitters on a new frequency before it is learned
        double retireListenSec = 2 * 3600;
        int retireMinSquitters = 3;       // squitters of the station since the change, on any frequency
        double announcedAgeSec = 3600;    // observed frequencies older than this are no longer "announced"
        int driftWindow = 8, driftMismatch = 3;   // this many of the last squitters heard on a frequency that their own mask does not list
    };
    struct Row {
        int gsId = 0;
        int version = -1;
        bool drift = false;
        std::vector<int> plan;            // built-in minus retired plus learned (kHz)
        std::vector<int> learned, retired;
        double lastSquitterSec = 0;
        int squitters = 0;
    };
    HfdlFreqLearner() = default;
    explicit HfdlFreqLearner(const Config& c) : cfg_(c) {}
    // the one the receiver feeds; its built-in table is hfdlStations() (hfdl_proto.h)
    static HfdlFreqLearner& global();

    // Off while a recording or the test signal plays: their stations and tables are not today's (the receiver feeds the learner only when it is on)
    void setEnabled(bool on) { std::lock_guard<std::mutex> lk(mu_); enabled_ = on; }
    bool enabled() const { std::lock_guard<std::mutex> lk(mu_); return enabled_; }
    void setBuiltin(const std::map<int, std::vector<int>>& table);          // gsId -> kHz, in the order of the system table
    // A squitter of ground station gsId heard on dialKhz; version = its system table version (-1 unknown); maskKhz = the frequencies its mask gives against the built-in table
    void squitter(int gsId, int dialKhz, int version, const std::vector<int>& maskKhz, double nowSec);
    // The receiver has been listening to these frequencies (kHz) for dtSec seconds
    void listen(const std::vector<int>& khz, double dtSec);

    std::vector<int> plan(int gsId) const;
    std::vector<int> allPlanKhz() const;                                      // every station's plan, sorted, no duplicates
    // What to hand to the band hopper as "announced as in use" for a station heard directly: its mask when the mask can be trusted, else the frequencies seen lately
    std::vector<int> announced(int gsId, const std::vector<int>& maskKhz, double nowSec) const;
    bool drifted(int gsId) const;
    std::vector<Row> rows() const;
    std::string save() const;
    void load(const std::string& text);
    void clear();

private:
    struct F { int hits = 0, hitsSince = 0; double lastSec = 0, listenSince = 0; };
    struct G {
        int version = -1;
        double changedSec = 0;            // when a version change was seen (0: none)
        int sqSince = 0, squitters = 0;
        double lastSec = 0;
        std::map<int, F> f;
        std::deque<bool> consistent;      // last squitters: the dial frequency is in the mask
    };
    bool builtinHas(int gs, int khz) const;
    bool retiredLocked(const G& g, int gs, int khz, const F* f) const;
    bool driftLocked(const G& g) const;
    std::vector<int> planLocked(int gs) const;

    mutable std::mutex mu_;
    Config cfg_;
    bool enabled_ = true;
    std::map<int, std::vector<int>> builtin_;
    std::map<int, G> g_;
};

} // namespace dect2
