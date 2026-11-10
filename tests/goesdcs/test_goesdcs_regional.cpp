// The DCS receiver with the Meteosat and Himawari channel plans: EUMETSAT TD 16 table 11 numbers, the EUMETSAT reference platform address, standard-rate
// (100 bps) Meteosat platforms on regional and international channels, Himawari 300 bps platforms without the GOES flag word, the 5 s carrier, frequency
// errors up to the 1.5 ppm stability limit, clock errors, a noise sweep and two platforms 1.5 kHz apart. Simulated signals only.
#include "dect2/goesdcs/goesdcs_gen.h"
#include "dect2/goesdcs/goesdcs_live.h"
#include "dect2/goesdcs/goesdcs_proto.h"
#include "dect2/goesdcs/goesdcs_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> ascii(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static GoesDcsGenTx txAt(double hz, int bps, double start, const std::string& text, uint32_t addr) {
    GoesDcsGenTx t;
    t.freqHz = hz; t.bps = bps; t.manchester = true; t.ext = false; t.startSec = start; t.address21 = addr; t.data = ascii(text);
    t.carrierSec = 5.0;   // EUMETSAT and JMA: 5 s of unmodulated carrier
    return t;
}

static DcsTelemetry run(const GoesDcsGenConfig& c, double seconds, int plan) {
    GoesDcsReceiver r;
    r.configure(c.rate);
    r.setCenterHz(c.centreHz);
    r.setPlan(plan);
    r.setStartUtc(1790000000);
    GoesDcsGenerator g(c);
    std::vector<cf32> x(32768);
    for (int64_t n = 0; n < (int64_t)(seconds * c.rate); n += (int64_t)x.size()) {
        g.generate(x.data(), x.size());
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
    // ---- the plans
    using dcs::Plan;
    CHECK(dcs::channelHz(1, Plan::Meteosat) == 402035500.0 && dcs::channelHz(267, Plan::Meteosat) == 402434500.0, "Meteosat 1 / 267");
    CHECK(dcs::channelHz(268, Plan::Meteosat) == 402002500.0 && dcs::channelHz(289, Plan::Meteosat) == 402034000.0, "Meteosat 268 / 289");
    CHECK(dcs::channelHz(101, Plan::Meteosat) == 402185500.0 && dcs::channelHz(200, Plan::Meteosat) == 402334000.0, "Meteosat table rows 101 / 200");
    for (int ch = 1; ch <= 289; ch++) CHECK(dcs::channelOf(dcs::channelHz(ch, Plan::Meteosat) + 600, Plan::Meteosat) == ch, "Meteosat round trip %d", ch);
    CHECK(dcs::channelOf(402.5e6, Plan::Meteosat) == 0 && dcs::channelOf(401.9e6, Plan::Meteosat) == 0, "outside the Meteosat plan");
    CHECK(dcs::channelOf(402.2e6, Plan::Himawari) == 0 && dcs::channelHz(5, Plan::Himawari) == 0, "Himawari: no numbers");
    CHECK(dcs::channelOf(dcs::channelHz(100), Plan::Goes) == 100, "GOES plan unchanged");
    CHECK(dcs::planForCentre(401.85e6) == Plan::Goes && dcs::planForCentre(402.22e6) == Plan::Meteosat, "auto plan");
    // ---- the EUMETSAT reference platform (TD 16 section 6): address 162096C4, a regional self-timed land platform of EUMETSAT
    const uint32_t refWord = 0x162096C4u >> 1;
    {
        uint32_t cw; int ne;
        CHECK(dcs::addressDecode(refWord, cw, ne) && ne == 0 && cw == refWord, "reference address passes the BCH check");
        CHECK(dcs::addressHex(refWord) == "162096C4", "shown as EUMETSAT writes it: %s", dcs::addressHex(refWord).c_str());
        CHECK(((refWord >> 27) & 0xF) == 1 && ((refWord >> 23) & 0xF) == 6 && ((refWord >> 10) & 1) == 0, "authority 1, type 6, bit 21 zero");
        CHECK(dcs::addressWord(refWord >> 10) == refWord, "rebuilt from its 21 bits");
    }
    const uint32_t refAddr21 = refWord >> 10;
    const std::string body = "SYNOP 06590 32970 10125 20087 39982 40213 58004=\r\n";
    // ---- Meteosat 100 bps on regional channel 150, the plan Auto (centre 402.22 MHz) and set
    for (int plan : {-1, 1}) {
        GoesDcsGenConfig c;
        c.rate = 500000; c.centreHz = 402.22e6; c.snrDb = 36;
        c.tx = {txAt(dcs::channelHz(150, Plan::Meteosat), 100, 0.5, body, refAddr21)};
        const DcsTelemetry t = run(c, 15.0, plan);
        const DcsMessage* m = find(t, refAddr21);
        CHECK(m != nullptr, "Meteosat ch 150 plan %d: no report (%llu ok, %llu bad)", plan, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        if (m) {
            CHECK(m->channel == 150 && m->plan == 1 && m->bps == 100 && m->eot && m->parityErrors == 0, "ch %d plan %d bps %d eot %d", m->channel, m->plan, m->bps, (int)m->eot);
            CHECK(m->text.find("SYNOP 06590") != std::string::npos, "text %s", m->text.c_str());
            CHECK(dcs::addressHex(m->address) == "162096C4", "address %s", dcs::addressHex(m->address).c_str());
        }
        CHECK(t.plan == 1, "telemetry plan %d", t.plan);
    }
    // ---- an international channel (268, 402.0025 MHz) and a regional one far from the centre (267), at once
    {
        GoesDcsGenConfig c;
        c.rate = 1e6; c.centreHz = 402.22e6; c.snrDb = 36;
        c.tx = {txAt(dcs::channelHz(268, Plan::Meteosat), 100, 0.5, "SHIP 1\r\n", 0x0A1B2), txAt(dcs::channelHz(267, Plan::Meteosat), 100, 1.0, "BUOY 2\r\n", 0x03FFF)};
        const DcsTelemetry t = run(c, 10.5, 1);
        const DcsMessage* a = find(t, 0x0A1B2);
        const DcsMessage* b = find(t, 0x03FFF);
        CHECK(a && a->channel == 268, "international channel 268: %d", a ? a->channel : -1);
        CHECK(b && b->channel == 267, "regional channel 267: %d", b ? b->channel : -1);
    }
    // ---- two platforms on adjacent 1.5 kHz channels (overlapping spectra), offset start
    {
        GoesDcsGenConfig c;
        c.rate = 250000; c.centreHz = 402.30e6; c.snrDb = 36;
        c.tx = {txAt(dcs::channelHz(200, Plan::Meteosat), 100, 0.5, "AAAA 1111\r\n", 0x0A1B2), txAt(dcs::channelHz(202, Plan::Meteosat), 100, 2.0, "BBBB 2222\r\n", 0x03FFF)};
        const DcsTelemetry t = run(c, 14.0, 1);
        CHECK(find(t, 0x0A1B2) && find(t, 0x03FFF), "channels 200 and 202 at once: %llu reports", (unsigned long long)t.blocksOk);
    }
    // ---- frequency error to the 1.5 ppm limit (600 Hz at 402 MHz) and clock errors
    {
        const double fe[] = {-600, 600, -250, 250};
        const double ppm[] = {60, 100, -60, 0};
        for (int i = 0; i < (std::getenv("ONAIR_TEST_LONG") ? 4 : 2); i++) {
            GoesDcsGenConfig c;
            c.rate = 250000; c.centreHz = 402.22e6; c.snrDb = 32; c.seed = 7 + (uint32_t)i;
            auto a = txAt(dcs::channelHz(120, Plan::Meteosat), 100, 0.5, body, refAddr21);
            a.extraHz = fe[i]; a.ppm = ppm[i];
            c.tx = {a};
            const DcsTelemetry t = run(c, 15.0, 1);
            const DcsMessage* m = find(t, refAddr21);
            CHECK(m && m->eot && m->parityErrors == 0, "offset %.0f Hz ppm %.0f", fe[i], ppm[i]);
            if (m) CHECK(std::fabs(m->freqHz - (dcs::channelHz(120, Plan::Meteosat) + fe[i])) < 40, "frequency %.0f", m->freqHz - dcs::channelHz(120, Plan::Meteosat));
        }
    }
    // ---- Himawari: 300 bps, 450-bit preamble, no flag word, frequency only
    {
        GoesDcsGenConfig c;
        c.rate = 250000; c.centreHz = 402.25e6; c.snrDb = 34;
        auto a = txAt(402.2615e6, 300, 0.5, "TSUNAMI 0.12 M 2026-11-10 23:50\r\n", 0x1C0DE);
        a.ext = false;
        c.tx = {a};
        const DcsTelemetry t = run(c, 9.0, 2);
        const DcsMessage* m = find(t, 0x1C0DE);
        CHECK(m && m->bps == 300 && m->channel == 0 && m->plan == 2 && m->flags < 0, "Himawari 300 bps: bps %d ch %d flags %d", m ? m->bps : 0, m ? m->channel : -1, m ? m->flags : 0);
        if (m) CHECK(m->text.find("TSUNAMI 0.12") != std::string::npos && std::fabs(m->freqHz - 402.2615e6) < 40, "Himawari text / freq");
    }
    // ---- noise sweep at 100 bps (carrier over the noise in 100 Hz): the report must come through down to 24 dB (ONAIR_TEST_LONG=1; else 24 dB only)
    {
        const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;
        int lowest = 99;
        for (double snr : {36.0, 30.0, 27.0, 24.0, 21.0}) {
            if (!longRun && snr != 24.0) continue;
            GoesDcsGenConfig c;
            c.rate = 250000; c.centreHz = 402.22e6; c.snrDb = snr; c.seed = 33;
            c.tx = {txAt(dcs::channelHz(150, Plan::Meteosat), 100, 0.5, body, refAddr21)};
            const DcsTelemetry t = run(c, 15.0, 1);
            const DcsMessage* m = find(t, refAddr21);
            const bool ok = m && m->text.find("SYNOP 06590") != std::string::npos;
            printf("  Meteosat 100 bps at %.0f dB: %s\n", snr, ok ? "decoded" : "lost");
            if (ok) lowest = (int)snr;
            if (snr >= 24) CHECK(ok, "noise sweep: lost at %.0f dB", snr);
        }
        printf("  lowest level decoded: %d dB in 100 Hz\n", lowest);
    }
    return testSummary("dcs regional plans");
}
