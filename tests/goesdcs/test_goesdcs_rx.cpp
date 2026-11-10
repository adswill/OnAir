// The GOES DCS receiver on generated signals: the three bit rates with Manchester and NRZ coding, several platforms at once on a wide capture, frequency and
// clock errors, noise, a steady carrier that is not a platform, noise only, bad input, the CSV log and clearing.
#include "dect2/goesdcs/goesdcs_gen.h"
#include "dect2/goesdcs/goesdcs_live.h"
#include "dect2/goesdcs/goesdcs_proto.h"
#include "dect2/goesdcs/goesdcs_tel.h"
#include "../common/tmp_path.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> ascii(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static GoesDcsGenTx tx(int ch, int bps, bool man, double start, const std::string& text, uint32_t addr = 0x1C0DE) {
    GoesDcsGenTx t;
    t.channel = ch; t.bps = bps; t.manchester = man; t.ext = bps != 100; t.startSec = start; t.address21 = addr; t.data = ascii(text);
    return t;
}

static DcsTelemetry run(GoesDcsGenConfig c, double seconds, bool poison = false, GoesDcsReceiver* keep = nullptr, std::vector<std::string>* logs = nullptr, float scale = 1.f) {
    GoesDcsReceiver local;
    GoesDcsReceiver& r = keep ? *keep : local;
    r.configure(c.rate);
    r.setCenterHz(c.centreHz);
    r.setStartUtc(1790000000);
    if (logs) r.setLogCallback([logs](const std::string& s) { logs->push_back(s); });
    GoesDcsGenerator g(c);
    std::vector<cf32> x(32768);
    for (int64_t n = 0; n < (int64_t)(seconds * c.rate); n += (int64_t)x.size()) {
        g.generate(x.data(), x.size());
        if (scale != 1.f) for (auto& v : x) v *= scale;
        if (poison) { x[5] = cf32(NAN, 0); x[99] = cf32(0, INFINITY); }
        r.feed(x.data(), x.size());
    }
    DcsTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const DcsMessage* find(const DcsTelemetry& t, uint32_t addr21) {
    for (const auto& m : t.messages) if (m.address == dcs::addressWord(addr21)) return &m;
    return nullptr;
}

int main() {
    const std::string body = "WL 12.34 TW 18.5 BV 12.6\r\n";
    // each rate and coding alone
    struct Case { int bps; bool man; };
    const Case cases[] = {{100, true}, {300, true}, {1200, true}, {1200, false}, {300, false}};
    for (const auto& cs : cases) {
        GoesDcsGenConfig c;
        c.rate = 250000; c.snrDb = 38;
        c.tx = {tx(100, cs.bps, cs.man, 0.5, body)};
        const DcsTelemetry t = run(c, cs.bps == 100 ? 8.0 : 5.0);
        const DcsMessage* m = find(t, 0x1C0DE);
        printf("  %4d bps %s: %s\n", cs.bps, cs.man ? "Manchester" : "NRZ", dcsSummary(t).c_str());
        CHECK(m != nullptr, "%d bps %d: no report (%llu ok, %llu bad)", cs.bps, (int)cs.man, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        if (!m) continue;
        CHECK(m->bps == cs.bps && m->manchester == cs.man, "rate/coding %d %d", m->bps, (int)m->manchester);
        CHECK(m->channel == 100 && m->eot && m->parityErrors == 0 && m->addressFixed == 0, "channel %d eot %d perr %d", m->channel, (int)m->eot, m->parityErrors);
        CHECK(m->text.find("WL 12.34 TW 18.5 BV 12.6") != std::string::npos, "text '%s'", m->text.c_str());
        CHECK(m->snrDb > 18, "snr %.1f", (double)m->snrDb);
        CHECK(t.blocksOk == 1 && t.platforms.size() == 1 && t.platforms[0].count == 1, "one report, %llu", (unsigned long long)t.blocksOk);
    }
    // three platforms at once on a 1 Msps capture, one of each rate, sending twice
    {
        GoesDcsGenConfig c;
        c.rate = 1e6; c.snrDb = 36;
        auto a = tx(60, 100, true, 0.5, "A 1.0 B 2.0\r\n", 0x0A1B2); a.repeatSec = 8;
        auto b = tx(100, 300, true, 1.0, "RAIN 0.4 TEMP 21.7\r\n", 0x1C0DE); b.repeatSec = 6;
        auto d = tx(140, 1200, true, 1.5, "STAGE 4.21 FT\r\n", 0x03FFF); d.repeatSec = 5;
        c.tx = {a, b, d};
        const DcsTelemetry t = run(c, 14.0);
        printf("  three platforms: %s\n", dcsSummary(t).c_str());
        CHECK(t.platforms.size() == 3, "platforms %zu", t.platforms.size());
        for (const auto& p : t.platforms) CHECK(p.count >= 2, "platform %08X heard %u times", p.address, p.count);
        const DcsMessage* ma = find(t, 0x0A1B2);
        const DcsMessage* md = find(t, 0x03FFF);
        CHECK(ma && ma->channel == 60 && ma->bps == 100, "platform A");
        CHECK(md && md->channel == 140 && md->bps == 1200 && md->text.find("STAGE 4.21 FT") != std::string::npos, "platform D");
        CHECK(t.blocksBad == 0, "bad reports %llu", (unsigned long long)t.blocksBad);
        CHECK(!t.waterfall.empty() && t.waterfall.size() == (size_t)kDcsWfRows * kDcsWfCols, "waterfall");
    }
    // pseudo-binary B data
    {
        GoesDcsGenConfig c;
        c.rate = 250000; c.snrDb = 38;
        auto a = tx(100, 300, true, 0.5, "");
        a.data = dcs::pseudoBinaryB({0, 5, 17, 1, 0, 44, 12, 9, 33, 0, 0, 63});
        c.tx = {a};
        const DcsTelemetry t = run(c, 5);
        const DcsMessage* m = find(t, 0x1C0DE);
        CHECK(m && m->format == 1 && m->fields.size() == 4 && m->fields[0] == "1: 337", "pseudo-binary report");
    }
    // input scale above the old fixed clip at 8 (float recordings, high-rate captures): the same report must decode at 1 Msps and 250 ksps
    {
        const double rates[] = {250000, 1e6};
        for (double rate : rates) {
            GoesDcsGenConfig c;
            c.rate = rate; c.snrDb = 36;
            c.tx = {tx(100, 300, true, 0.5, body)};
            const DcsTelemetry t = run(c, 5.0, false, nullptr, nullptr, 200.f);
            const DcsMessage* m = find(t, 0x1C0DE);
            CHECK(m && m->parityErrors == 0 && m->text.find("WL 12.34 TW 18.5 BV 12.6") != std::string::npos, "scale 200 at %.0f sps: %s", rate, dcsSummary(t).c_str());
        }
    }
    // frequency error, clock error, weaker signal, a big capture rate (decimation), bad samples
    {
        const double ppms[] = {-80, 80};
        const double fe[] = {-180, 220};
        for (int i = 0; i < 2; i++) {
            GoesDcsGenConfig c;
            c.rate = 2e6; c.snrDb = 30;
            auto a = tx(101, 1200, true, 0.5, "STAGE 4.21 FT BATT 12.6 TEMP 19.3 WIND 2.4\r\n" + std::string(200, 'X'));
            a.ppm = ppms[i]; a.extraHz = fe[i];
            c.tx = {a};
            const DcsTelemetry t = run(c, 4, true);
            const DcsMessage* m = find(t, 0x1C0DE);
            CHECK(m && m->eot && m->parityErrors == 0, "error %d: ppm %.0f offset %.0f Hz (ok %llu bad %llu)", i, ppms[i], fe[i], (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
            if (m) CHECK(std::fabs(m->freqHz - (dcs::channelHz(101) + fe[i])) < 40, "frequency %.1f", m->freqHz - dcs::channelHz(101));
        }
        GoesDcsGenConfig c;
        c.rate = 250000; c.snrDb = 27;
        c.tx = {tx(100, 300, true, 0.5, "WL 12.34\r\n")};
        c.tx[0].ppm = 150; c.tx[0].extraHz = 90;
        const DcsTelemetry t = run(c, 5);
        CHECK(find(t, 0x1C0DE) != nullptr, "weak signal 27 dB with errors");
    }
    // not platforms: a steady carrier, noise only
    {
        GoesDcsGenConfig c;
        c.rate = 250000; c.snrDb = 36;
        c.carrierChannels = {180, 40};
        const DcsTelemetry t = run(c, 30);
        CHECK(t.blocksOk == 0 && t.messages.empty(), "steady carriers give no report (%llu)", (unsigned long long)t.blocksOk);
        GoesDcsGenConfig n;
        n.rate = 250000; n.snrDb = 20;
        n.tx = {tx(100, 300, true, 1000, "X")};
        const DcsTelemetry t2 = run(n, 20);
        CHECK(t2.blocksOk == 0 && t2.carriers.empty(), "noise only: %llu reports, %zu carriers", (unsigned long long)t2.blocksOk, t2.carriers.size());
    }
    // the log file, the log callback, clearing
    {
        const std::string path = tmpPath("goesdcs_test.csv");
        std::remove(path.c_str());
        GoesDcsGenConfig c;
        c.rate = 250000; c.snrDb = 38;
        c.tx = {tx(100, 300, true, 0.5, "WL 12.34, \"x\"\r\n")};
        GoesDcsReceiver r;
        r.setLogFile(path);
        std::vector<std::string> logs;
        DcsTelemetry t = run(c, 5, false, &r, &logs);
        CHECK(t.blocksOk == 1 && t.logging, "log run");
        CHECK(!logs.empty() && logs[0].find("GOES DCS") != std::string::npos, "log callback");
        std::ifstream f(path);
        std::stringstream ss; ss << f.rdbuf();
        const std::string s = ss.str();
        CHECK(s.find("id,unix_time,address") == 0 && s.find(dcs::addressHex(dcs::addressWord(0x1C0DE))) != std::string::npos && s.find("\"\"x\"\"") != std::string::npos, "csv: %s", s.c_str());
        r.clearResults();
        uint8_t dummy[8] = {};
        r.feed(reinterpret_cast<const cf32*>(dummy), 1);
        DcsTelemetry t2;
        CHECK(r.telemetry(t2, t.seq) && t2.messages.empty() && t2.platforms.empty() && t2.blocksOk == 0, "cleared");
    }
    return testSummary("goesdcs rx");
}
