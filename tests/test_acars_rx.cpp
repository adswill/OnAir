// ACARS receiver, clean signal: the generator's blocks come out bit-exact, three channels at once, eight channels with 25 kHz neighbours,
// the same result whatever the chunk size, a channel list for 8.33 kHz channels, a tuning that is not on the grid.
#include "dect2/acars_sim.h"
#include <cstdio>
#include <set>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static AcarsSimCfg base() {
    AcarsSimCfg c;
    c.syn.snrDb = 35;
    c.gen.rateFactor = 3;
    c.secs = 24;
    return c;
}

// every field of every decoded block against what was sent
static void fieldsMatch(const AcarsSimResult& r, const char* what) {
    int checked = 0, bad = 0;
    for (const auto& m : r.msgs) {
        for (const auto& s : r.sentAll) {
            std::string lab(2, ' '); lab[0] = s.spec.label[0]; lab[1] = s.spec.label[1];
            if (s.freqHz != m.freqHz || s.spec.blockId != m.blockId || lab != m.label) continue;
            std::string reg = s.spec.reg;
            if (reg != m.reg) continue;
            const bool dl = s.spec.blockId >= '0' && s.spec.blockId <= '9';
            std::string t = s.spec.text;
            bool ok = m.downlink == dl && m.finalBlock == s.spec.lastBlock && m.mode == s.spec.mode && (uint8_t)m.ack == (s.spec.ack == 0x15 ? '!' : s.spec.ack == 0x06 ? '^' : s.spec.ack);
            if (dl && t.size() >= 10) ok = ok && m.msgNum == t.substr(0, 3) && m.msgSeq == t[3] && m.flightId == t.substr(4, 6);
            if (m.crcOk != true) ok = false;
            if (!(m.timeSec >= s.endSec - 0.2 && m.timeSec <= s.endSec + 0.6)) continue;      // the right instance of a repeated message
            checked++;
            if (!ok) { bad++; printf("  field mismatch: %s %s %c\n", m.reg.c_str(), m.label.c_str(), m.blockId); }
            break;
        }
    }
    CHECK(bad == 0 && checked * 100 >= (int)r.msgs.size() * 98, "%s: fields of %d messages checked, %d wrong", what, checked, bad);
}

static void expectClean(const AcarsSimResult& r, const char* what, int minSent) {
    printf("  %-34s sent %3d  decoded %3d  wrong %d  blocks ok %llu bad %llu  %.0fx real time\n", what, r.sent, r.decoded, r.wrong, (unsigned long long)r.last.blocksOk,
           (unsigned long long)r.last.blocksBad, r.rtf);
    CHECK(r.sent >= minSent, "%s: only %d blocks sent", what, r.sent);
    CHECK(r.decoded == r.sent, "%s: %d of %d blocks decoded", what, r.decoded, r.sent);
    CHECK(r.wrong == 0, "%s: %d decoded messages match nothing that was sent", what, r.wrong);
    CHECK(r.last.blocksBad == 0, "%s: %llu blocks failed their check", what, (unsigned long long)r.last.blocksBad);
    for (const auto& s : r.missing) printf("    missing %.2f s %.3f MHz %s %c%c %c\n", s.startSec, s.freqHz / 1e6, s.spec.reg.c_str(), s.spec.label[0], s.spec.label[1], s.spec.blockId);
}

int main() {
    // ---- the default scene: six aircraft on 131.525, 131.725 and 131.825 MHz
    {
        auto c = base();
        auto r = runAcarsSim(c);
        expectClean(r, "default scene, 2 Msps", 40);
        fieldsMatch(r, "default scene");
        std::set<double> ch;
        for (const auto& m : r.msgs) ch.insert(m.freqHz);
        CHECK(ch.size() == 3 && ch.count(131.525e6) && ch.count(131.725e6) && ch.count(131.825e6), "channels heard: %zu", ch.size());
        CHECK(r.last.aircraft.size() == 6, "%zu aircraft in the table", r.last.aircraft.size());
        CHECK(r.last.channels.size() == 3, "%zu channels in the table", r.last.channels.size());
        CHECK(r.last.state == 2 && r.last.dataValid && r.last.blocksOk == (uint64_t)r.total, "state %d, blocks %llu of %d", r.last.state, (unsigned long long)r.last.blocksOk, r.total);
        // downlinks of one aircraft carry its flight id, the table has it
        int withFlight = 0;
        for (const auto& a : r.last.aircraft) withFlight += !a.flight.empty();
        CHECK(withFlight == 6, "flight ids in the aircraft table: %d", withFlight);
        // OOOI text is read
        int oooi = 0;
        for (const auto& m : r.msgs) oooi += !m.decoded.empty();
        CHECK(oooi >= 3, "%d messages with an OOOI reading", oooi);
        // the message list is capped at 200, newest first, with serials that only grow
        CHECK(r.last.messages.size() <= 200, "message list %zu", r.last.messages.size());
        for (size_t i = 1; i < r.last.messages.size(); i++) CHECK(r.last.messages[i - 1].serial > r.last.messages[i].serial, "newest first");
    }
    // ---- eight channels at once, three pairs of them 25 kHz apart
    {
        auto c = base();
        c.gen.channelsHz = {131.125e6, 131.150e6, 131.525e6, 131.550e6, 131.725e6, 131.825e6, 131.850e6, 131.875e6};
        c.gen.aircraft = 16;
        c.gen.rateFactor = 2;
        c.secs = 30;
        auto r = runAcarsSim(c);
        expectClean(r, "eight channels, 25 kHz neighbours", 80);
        std::set<double> ch;
        for (const auto& m : r.msgs) ch.insert(m.freqHz);
        CHECK(ch.size() == 8, "channels heard: %zu of 8", ch.size());
    }
    // ---- chunk size does not matter: the same samples in pieces of 1, 7, 4096 and 65536
    {
        auto c = base();
        c.secs = 5;
        c.gen.rateFactor = 6;
        std::vector<std::string> ref;
        for (size_t chunk : {(size_t)65536, (size_t)4096, (size_t)7, (size_t)1}) {
            c.chunk = chunk;
            auto r = runAcarsSim(c);
            std::vector<std::string> keys;
            for (const auto& m : r.msgs) keys.push_back(acarsSimKey(m.reg, m.label, m.blockId, m.text) + "@" + std::to_string(m.freqHz));
            if (ref.empty()) ref = keys;
            printf("  chunk %6zu: %zu messages\n", chunk, keys.size());
            CHECK(keys.size() >= 8 && keys == ref, "chunk %zu: %zu messages against %zu", chunk, keys.size(), ref.size());
        }
    }
    // ---- 8.33 kHz channels need the channel list: 131.5167 MHz is not on the 25 kHz grid
    {
        auto c = base();
        c.gen.channelsHz = {131.5167e6};
        c.gen.aircraft = 3;
        c.secs = 20;
        auto none = runAcarsSim(c);
        CHECK(none.total == 0, "without a channel list the off-grid channel gave %d messages", none.total);
        c.userChannels = {131.5167e6};
        auto r = runAcarsSim(c);
        expectClean(r, "channel list: 131.5167 MHz", 8);
    }
    // ---- the radio is tuned to 131.512 MHz, not to a multiple of 25 kHz: the channels stay where they are on the air
    {
        auto c = base();
        c.gen.centerHz = 131.512e6;
        auto r = runAcarsSim(c);
        expectClean(r, "tuned to 131.512 MHz", 40);
        std::set<double> ch;
        for (const auto& m : r.msgs) ch.insert(m.freqHz);
        CHECK(ch.count(131.525e6) && ch.count(131.725e6) && ch.count(131.825e6), "channel frequencies with an odd tuning");
    }
    // ---- a clean signal with the radio's 1 MHz minimum rate
    {
        auto c = base();
        c.rate = 1e6;
        c.gen.channelsHz = {131.525e6, 131.725e6, 131.125e6};
        c.secs = 20;
        auto r = runAcarsSim(c);
        expectClean(r, "1 Msps", 40);
    }
    printf(fails ? "acars rx: %d FAILED\n" : "acars rx: all passed\n", fails);
    return fails ? 1 : 0;
}
