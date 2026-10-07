// ADS-B test signal generator: determinism, levels, options, the message mix at its rates, and the correctness of what is sent (every frame must carry
// a valid CRC, every address / parity frame the address of an aircraft that also sends clean ones).
#include "dect2/adsb_gen.h"
#include "dect2/adsb_sim.h"
#include "dect2/modes.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void testSynth() {
    SynthConfig cfg;
    cfg.mode = 13; cfg.snrDb = 30;
    // the engine's factory finds it, and a rate below 2 Msps has no test signal
    CHECK(makeModeSynth(13, cfg, 4e6) != nullptr, "no generator from the registry");
    CHECK(makeModeSynth(13, cfg, 1.9e6) == nullptr, "a generator at 1.9 Msps");
    CHECK(modeTuningById("adsb") && modeTuningById("adsb")->stdMode == 13 && modeTuning(13) && std::string(modeTuning(13)->id) == "adsb", "tuning table");
    const ModeTuning* mt = modeTuningById("adsb");
    CHECK(mt->minMhz <= 1090 && mt->maxMhz >= 1090 && mt->defMhz == 1090.0 && mt->minSampleRate == 2e6 && mt->sampleRate >= mt->minSampleRate && mt->basebandHz > 0, "tuning values");
    // deterministic: the same options give the same samples; another seed gives other ones
    for (double rate : {2e6, 2.4e6, 4e6, 10e6, 20e6}) {
        auto a = makeAdsbSynth(cfg, rate), b = makeAdsbSynth(cfg, rate);
        SynthConfig c2 = cfg; c2.modeOpt[2] = 5;
        auto c = makeAdsbSynth(c2, rate);
        const size_t blkN = (size_t)(rate / 10);     // 0.1 s per block, 1.5 s in all: enough for the nearest aircraft to have been heard
        std::vector<cf32> x(blkN), y(blkN), z(blkN);
        double sumSq = 0, peak = 0;
        bool same = true, differs = false;
        for (int blk = 0; blk < 15; blk++) {
            a->generate(x.data(), x.size()); b->generate(y.data(), y.size()); c->generate(z.data(), z.size());
            same &= memcmp(x.data(), y.data(), x.size() * sizeof(cf32)) == 0;
            differs |= memcmp(x.data(), z.data(), x.size() * sizeof(cf32)) != 0;
            for (auto& v : x) { sumSq += std::norm(v); peak = std::max(peak, (double)std::max(std::fabs(v.real()), std::fabs(v.imag()))); }
        }
        const double rms = std::sqrt(sumSq / (15.0 * x.size()));
        printf("  %5.1f Msps: rms %.4f, peak %.3f\n", rate / 1e6, rms, peak);
        CHECK(same, "%.1f Msps: two generators with the same options differ", rate / 1e6);
        CHECK(differs, "a different seed gives the same samples");
        CHECK(peak < 0.96, "%.1f Msps: peak %.3f", rate / 1e6, peak);
        CHECK(rms > 0.001 && rms < 0.1, "%.1f Msps: rms %.4f (a signal of short bursts has an rms well below the continuous modes')", rate / 1e6, rms);
    }
    // the output does not depend on how it is asked for
    {
        auto a = makeAdsbSynth(cfg, 4e6), b = makeAdsbSynth(cfg, 4e6);
        std::vector<cf32> x(400000), y(400000);
        a->generate(x.data(), x.size());
        size_t at = 0;
        const size_t sizes[] = {1, 7, 4096, 65536, 333, 1000};
        int k = 0;
        while (at < y.size()) { const size_t n = std::min(sizes[k++ % 6], y.size() - at); b->generate(y.data() + at, n); at += n; }
        CHECK(memcmp(x.data(), y.data(), x.size() * sizeof(cf32)) == 0, "the samples depend on the block sizes");
    }
    // configure(): noise can change while it plays, the airspace cannot
    {
        auto a = makeAdsbSynth(cfg, 4e6);
        SynthConfig c2 = cfg; c2.snrDb = 20;
        CHECK(a->configure(c2), "a new snr needs a new generator");
        c2.modeOpt[0] = 30;
        CHECK(!a->configure(c2), "a new number of aircraft was applied to a running generator");
    }
}

static void testMessageMix() {
    // 12 aircraft for 60 s: rates per aircraft, and every frame valid
    AdsbAirspaceConfig ac; ac.aircraft = 12; ac.seed = 1;
    AdsbAirspace air(ac);
    std::vector<AdsbTx> tx;
    air.run(60.0, tx);
    std::map<uint32_t, std::map<std::string, int>> perAc;
    std::set<uint32_t> es;   // addresses that sent clean DF17 / DF11
    int bad = 0, total = 0;
    std::set<int> dfs;
    for (auto& t : tx) {
        total++;
        const int df = (int)adsb::getBits(t.frame.b, 1, 5);
        dfs.insert(df);
        const uint32_t rem = adsb::crc24(t.frame.b, t.frame.bits);
        uint32_t addr = rem;
        std::string kind;
        if (df == 17) { bad += rem != 0; addr = adsb::getBits(t.frame.b, 9, 24); es.insert(addr); adsb::Msg m; adsb::decodeMsg(t.frame.b, 112, m); kind = "tc" + std::to_string(m.tc); }
        else if (df == 11) { bad += rem >= 128; addr = adsb::getBits(t.frame.b, 9, 24); es.insert(addr); kind = "df11"; }
        else kind = "df" + std::to_string(df);
        perAc[addr][kind]++;
    }
    printf("  60 s of 12 aircraft: %d transmissions, formats:", total);
    for (int d : dfs) printf(" DF%d", d);
    printf("\n");
    CHECK(bad == 0, "%d frames with a wrong CRC", bad);
    for (auto& kv : perAc) CHECK(es.count(kv.first), "address %06X sends reply frames but no DF17 / DF11", kv.first);
    CHECK(perAc.size() == 12 || perAc.size() == 13 || perAc.size() == 14, "%zu addresses (aircraft that left the range are replaced)", perAc.size());
    CHECK(dfs.count(0) && dfs.count(4) && dfs.count(5) && dfs.count(11) && dfs.count(16) && dfs.count(17) && dfs.count(20) && dfs.count(21), "not every format is sent");
    // the rates of an aircraft that was around for the whole minute (positions 2/s in all, velocity 2/s, identification every 5 s)
    int checked = 0;
    for (auto& kv : perAc) {
        auto& m = kv.second;
        if (m["tc11"] + m["tc9"] + m["tc10"] + m["tc12"] + m["tc13"] + m["tc14"] < 100) continue;
        const int pos = m["tc9"] + m["tc10"] + m["tc11"] + m["tc12"] + m["tc13"] + m["tc14"], vel = m["tc19"], id = m["tc4"];
        CHECK(pos > 100 && pos < 140 && vel > 100 && vel < 140 && id >= 9 && id <= 15, "%06X: %d positions, %d velocity, %d identification in 60 s", kv.first, pos, vel, id);
        CHECK(m["df11"] > 15 && m["df11"] < 50 && m["tc31"] > 15 && m["tc31"] < 33, "%06X: %d all-call, %d operational status", kv.first, m["df11"], m["tc31"]);
        checked++;
    }
    CHECK(checked >= 8, "only %d aircraft with the full rate", checked);
    // the rate multiplier and the options
    AdsbAirspaceConfig fast = ac; fast.rateMultiplier = 4;
    AdsbAirspace air2(fast);
    std::vector<AdsbTx> tx2;
    air2.run(20.0, tx2);
    CHECK(tx2.size() > 3 * tx.size() / 3 * 0.9 / 1 && tx2.size() > 1.0 * tx.size() / 3 * 3.0 * 0.5, "message rate x4: %zu in 20 s against %zu in 60 s", tx2.size(), tx.size());
    AdsbAirspaceConfig few = ac; few.aircraft = 3; few.replies = false;
    AdsbAirspace air3(few);
    std::vector<AdsbTx> tx3;
    air3.run(20.0, tx3);
    bool onlyEs = true;
    for (auto& t : tx3) { const int df = (int)adsb::getBits(t.frame.b, 1, 5); onlyEs &= df == 17 || df == 11; }
    CHECK(onlyEs && air3.aircraft().size() == 3, "replies were sent although they are off");
    AdsbAirspaceConfig emer = ac; emer.emergencyDemo = true;
    AdsbAirspace air4(emer);
    std::vector<AdsbTx> tx4;
    air4.run(10.0, tx4);
    int tc28 = 0;
    for (auto& t : tx4) { adsb::Msg m; if (adsb::getBits(t.frame.b, 1, 5) == 17 && adsb::decodeMsg(t.frame.b, 112, m) && m.tc == 28 && m.emergency == 1 && m.squawk == 7700) tc28++; }
    CHECK(tc28 > 3, "the emergency demo sent %d status messages", tc28);
    // levels fall with the distance
    double lo = 1, hi = 0;
    for (auto& t : tx) { lo = std::min(lo, (double)t.amp); hi = std::max(hi, (double)t.amp); }
    CHECK(hi < 0.55 && lo > 0.01 && hi / lo > 4, "amplitudes %.3f to %.3f", lo, hi);
    // the aircraft stay near the reference point: within 220 NM
    for (auto& i : air.aircraft()) CHECK(i.distNm < 225 && std::isfinite(i.lat) && std::isfinite(i.lon), "%06X at %.1f NM", i.icao, i.distNm);
}

static void testBurstShape() {
    // a burst at different positions between the samples is the same waveform sampled differently: the energy does not depend on the phase much
    for (double rate : {2e6, 4e6, 20e6}) {
        double lo = 1e9, hi = 0;
        for (int ph = 0; ph < 10; ph++) {
            AdsbMixer mix(rate, 0);
            AdsbTx t;
            t.t = 20e-6 + ph * 0.05e-6;
            t.frame = adsb::encodeAllCall(0x4840D6, 5, 0);
            t.amp = 1; t.phase = 0.3f; t.cfoHz = 100e3;
            mix.add(t);
            std::vector<cf32> x((size_t)(120e-6 * rate));
            mix.render(x.data(), x.size());
            double e = 0;
            for (auto& v : x) e += std::norm(v);
            e /= rate / 1e6;     // in units of amplitude^2 x microseconds
            lo = std::min(lo, e); hi = std::max(hi, e);
        }
        printf("  %5.1f Msps: energy of a 56 bit burst of amplitude 1 over ten phases: %.1f to %.1f (60 pulses of 0.5 us would be 30)\n", rate / 1e6, lo, hi);
        // 8 preamble pulses plus 56 data pulses = 60 pulses of 0.5 us, but touching pulses merge: the energy is still about 60 x 0.5 = 30 up to the filter
        // (at 2 Msps the samples are aliased and the energy of a burst depends on its phase against the sample clock, by 25 % here)
        CHECK(lo > (rate < 3e6 ? 18 : 25) && hi < 34, "%.1f Msps: pulse energy %.1f to %.1f", rate / 1e6, lo, hi);
        CHECK(hi / lo < (rate < 3e6 ? 1.35 : 1.1), "%.1f Msps: the energy changes with the phase by %.2f", rate / 1e6, hi / lo);
    }
}

static void testEncodersAgainstPublished() {
    // The encoders must give back the bytes of published messages (The 1090 Megahertz Riddle, ADS-B chapters 2, 3 and 5), not only something the decoder
    // of the same author accepts.
    auto hex = [](const adsb::Frame& f) { return adsb::toHex(f.b, f.bits); };
    // identification: ICAO 4840D6, CA 5, TC 4, category 0, callsign KLM1023
    CHECK(hex(adsb::encodeIdentification(0x4840D6, 5, 4, 0, "KLM1023")) == "8D4840D6202CC371C32CE0576098", "identification: %s", hex(adsb::encodeIdentification(0x4840D6, 5, 4, 0, "KLM1023")).c_str());
    // airborne position pair of 40621D at 38000 ft: the positions are the Riddle's decoded ones (the even frame's grid point, and the odd frame's)
    const adsb::Frame e = adsb::encodeAirbornePosition(0x40621D, 5, 11, 38000, false, false, 52.25720214843750, 3.91937255859375);
    const adsb::Frame o = adsb::encodeAirbornePosition(0x40621D, 5, 11, 38000, false, true, 52.26578017412606, 3.938912527901786);
    CHECK(hex(e) == "8D40621D58C382D690C8AC2863A7", "even position: %s", hex(e).c_str());
    CHECK(hex(o) == "8D40621D58C386435CC412692AD6", "odd position: %s", hex(o).c_str());
    // velocity of 485020: the published message has a reserved bit set that the encoder does not, so the fields are compared, not the bytes
    adsb::Msg m;
    const adsb::Frame v = adsb::encodeVelocity(0x485020, 5, 159.2, 182.88, -832, true, 550);
    CHECK(adsb::crc24(v.b, 112) == 0 && adsb::decodeMsg(v.b, 112, m) && m.st == 1 && std::fabs(m.speedKt - 159.2) < 0.1 && std::fabs(m.headingDeg - 182.88) < 0.01 && m.vrateFpm == -832 && m.gnssDiffFt == 550,
          "velocity: %.2f kt %.2f deg %d ft/min %d ft", m.speedKt, m.headingDeg, m.vrateFpm, m.gnssDiffFt);
    // the squawk field of the identity reply, and the altitude reply of the Riddle's Mode S chapter 3 (36000 ft; the address is in the parity, so it is taken from the published message)
    const uint8_t pub[7] = {0x20, 0x00, 0x17, 0x18, 0x06, 0xA9, 0x83};
    const uint32_t addr = adsb::crc24(pub, 56);
    CHECK(hex(adsb::encodeAltitudeReply(addr, 0, 36000, false)) == "2000171806A983", "altitude reply: %s", hex(adsb::encodeAltitudeReply(addr, 0, 36000, false)).c_str());
    // identity reply (squawk 0356, flight status 2): the published message has the spare bit X and a bit of the utility message set, which the encoder leaves at 0
    const uint8_t pub5[7] = {0x2A, 0x00, 0x51, 0x6D, 0x49, 0x2B, 0x80};
    const adsb::Frame id5 = adsb::encodeIdentityReply(adsb::crc24(pub5, 56), 2, 356);
    CHECK(adsb::getBits(id5.b, 1, 13) == adsb::getBits(pub5, 1, 13) && (adsb::getBits(id5.b, 20, 13) & ~0x40u) == (adsb::getBits(pub5, 20, 13) & ~0x40u), "identity reply: %s", hex(id5).c_str());
    // all-call reply 5D484FDEA248F5: address 484FDE, CA 5, interrogator code 22
    CHECK(hex(adsb::encodeAllCall(0x484FDE, 5, 22)) == "5D484FDEA248F5", "all-call: %s", hex(adsb::encodeAllCall(0x484FDE, 5, 22)).c_str());
}

int main() {
    testEncodersAgainstPublished();
    testSynth();
    testMessageMix();
    testBurstShape();
    printf(fails ? "adsb_gen: %d FAILED\n" : "adsb_gen: all passed\n", fails);
    return fails ? 1 : 0;
}
