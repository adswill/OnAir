// Inmarsat-C channel search: three more channels in the same 2 Msps capture (LES TDM channels with their own messages, stations and carrier errors) next to
// the one tuned to. The receiver must find them from the spectrum, decode their frames and messages, and not disturb the main channel.
#include "dect2/inmc_testutil.h"
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void run(bool search, int extras, double ebn0, double secs) {
    InmcGenConfig g;
    g.rate = 2e6; g.ebn0Db = ebn0; g.seed = 7; g.extraChannels = extras; g.cfoHz = 2200;
    auto gen = makeInmcGenerator(g);
    InmcReceiver rx;
    rx.configure(g.rate);
    rx.setSignalOffset(-50000);
    rx.setChannelSearch(search);
    std::vector<std::string> logs;
    rx.setLogCallback([&](const std::string& s) { logs.push_back(s); });
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(secs * g.rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        gen->generate(buf.data(), buf.size());
        for (auto& v : buf) v = cf32(std::round(v.real() * 127.f) / 127.f, std::round(v.imag() * 127.f) / 127.f);
        rx.feed(buf.data(), buf.size());
    }
    InmcTelemetry t;
    rx.telemetry(t, 0);
    printf("  search %s, %d extra channels, Eb/N0 %.0f dB, %.0f s: %zu channels, main channel %llu good frames, %zu messages in the list\n", search ? "on" : "off", extras, ebn0, secs,
           t.channels.size(), (unsigned long long)t.blocksOk, t.messages.size());
    for (auto& c : t.channels)
        printf("    channel %d at %+.1f kHz (carrier %+.1f Hz from the centre), state %d, %llu frames, %u messages, station %d %s, %s\n", c.index, c.offsetHz / 1000, c.carrierHz,
               c.state, (unsigned long long)c.framesOk, c.messages, c.lesId, c.lesName.c_str(), c.channelTypeName.c_str());
    for (auto& l : logs) printf("    log: %s\n", l.c_str());
    CHECK(t.blocksOk >= (uint64_t)(secs / 8.64) - 2 && t.blocksBad == 0, "main channel: %llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    CHECK(t.state == 2 && !t.channels.empty() && t.channels[0].index == 0, "main channel state");
    if (!search || extras == 0) {
        CHECK(t.channels.size() == 1, "%zu channels without a search or extra channels", t.channels.size());
        return;
    }
    CHECK((int)t.channels.size() == extras + 1, "%zu channels, wanted %d", t.channels.size(), extras + 1);
    static const double off[3] = {-20000, -90000, 30000}, cfo[3] = {1200, -2300, 700};
    static const int les[3] = {4, 12, 21};
    for (int i = 0; i < extras; i++) {
        bool found = false;
        for (auto& c : t.channels) {
            if (c.index == 0) continue;
            if (std::fabs(c.carrierHz - (off[i] + 2200 + cfo[i])) < 6) {
                found = true;
                CHECK(c.state == 2 && c.framesOk >= 2 && c.framesBad <= 1, "channel at %+.0f Hz: state %d, %llu frames", off[i], c.state, (unsigned long long)c.framesOk);
                CHECK(c.sat == 3 && c.lesId == les[i] && c.channelType == 2, "channel at %+.0f Hz: sat %d station %d type %d", off[i], c.sat, c.lesId, c.channelType);
            }
        }
        CHECK(found, "no channel decoded at %+.0f Hz", off[i]);
    }
    int fromOthers = 0;
    for (auto& m : t.messages) if (m.channel > 0 && m.complete) fromOthers++;
    CHECK(fromOthers >= 1, "no complete message from the other channels (%d)", fromOthers);
}

int main() {
    run(true, 3, 10, 70);
    run(true, 0, 10, 40);         // nothing else in the band: no false channels
    run(false, 3, 10, 30);        // search off: only the main channel
    printf(fails ? "FAILED\n" : "inmc_multi ok\n");
    return fails ? 1 : 0;
}
