// DGPS beacons: RTCM SC-104 word parity against the GPS LNAV code OnAir runs on real satellites, the message layers against the decoded example messages of the public
// "RTCM SC-104 transmission characteristics of marine DGPS stations" paper (types 1, 3, 7), every message type through the MSK demodulator, and a sweep of bit rate,
// carrier offset, sample rate, clock error, polarity and noise.
#include "dect2/dgps/dgps_engine.h"
#include "dect2/dgps/dgps_rx.h"
#include "dect2/gnss/gnss_nav.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

static dgps::Stats run(const std::vector<uint8_t>& bits, double fs, double offsetHz, double bitRate = 200, double rateScale = 1.0, double noise = 0, bool blocks = true, unsigned seed = 1) {
    auto iq = dgps::modulate(bits, fs, offsetHz, 0.5f, rateScale, bitRate);
    if (noise > 0) {
        std::mt19937 g(seed);
        std::normal_distribution<float> nd(0.f, (float)noise);
        for (auto& v : iq) v += std::complex<float>(nd(g), nd(g));
    }
    dgps::Receiver rx(bitRate);
    const size_t blk = blocks ? (size_t)fs : iq.size();   // one second at a time, like the engine
    for (size_t i = 0; i < iq.size(); i += blk) rx.process(iq.data() + i, std::min(blk, iq.size() - i), fs);
    return rx.stats();
}

static std::vector<uint8_t> frame(const dgps::Header& h, const std::vector<uint32_t>& body, int pol = 0) {
    dgps::Header x = h; x.length = (int)body.size();
    std::vector<uint8_t> bits(137, 0);   // leading bits so the bit timing has to be found
    for (size_t i = 0; i < bits.size(); i++) bits[i] = (uint8_t)((i * 7 + (i >> 3)) & 1);
    const auto m = dgps::encodeMessage(x, body, pol);
    bits.insert(bits.end(), m.begin(), m.end());
    for (int i = 0; i < 60; i++) bits.push_back((uint8_t)(i & 1));
    return bits;
}

static const dgps::Message* lastOf(const dgps::Stats& s, int type) {
    for (size_t i = s.messages.size(); i-- > 0;) if (s.messages[i].h.type == type && s.messages[i].complete) return &s.messages[i];
    return nullptr;
}

int main() {
    // ---- the word parity is the GPS one: 30 bits per word built here equal OnAir's LNAV encoder for every previous-bits combination
    {
        std::mt19937 g(5);
        int bad = 0;
        for (int i = 0; i < 2000; i++) {
            const uint32_t d24 = (uint32_t)g() & 0xFFFFFF;
            const int p29 = (int)(g() & 1), p30 = (int)(g() & 1);
            dgps::Header h; h.type = 6; h.length = 1;
            // build a one-word body and compare its parity bits (bits 25..30 of the third word) with lnavParity
            const auto m = dgps::encodeMessage(h, {d24}, 0);
            // chain: the header's last two bits are the previous bits of the body word
            const int a29 = m[2 * 30 - 2], a30 = m[2 * 30 - 1];
            uint32_t par = 0;
            for (int k = 0; k < 6; k++) par = (par << 1) | m[2 * 30 + 24 + (size_t)k];
            const uint32_t want = lnavParity(d24, a29, a30);
            if (par != want) bad++;
            (void)p29; (void)p30;
        }
        CHECK(bad == 0, "RTCM word parity differs from the GPS LNAV parity in %d of 2000 words", bad);
    }
    // ---- message layers against the paper's decoded examples
    {
        dgps::Header h; h.type = 1; h.stationId = 815; h.zcount = 4083; h.seq = 1; h.health = 0;
        // Id: 815 Type: 1 Z: 4083 Seq: 1: Sat 24 PRC -4.76 m +.012 m/s IOD 207 UDRE < 4 m; Sat 4 PRC 1.66 -.022 IOD 0 UDRE < 1 m; Sat 25 -2.54 +.050 IOD 120
        std::vector<dgps::SatCorr> sats;
        auto add = [&](int sat, double prc, double rrc, int iod, int udre) { dgps::SatCorr s; s.sat = sat; s.prcRaw = (int)std::lround(prc / 0.02); s.rrcRaw = (int)std::lround(rrc / 0.002); s.iod = iod; s.udre = udre; sats.push_back(s); };
        add(24, -4.76, 0.012, 207, 1); add(4, 1.66, -0.022, 0, 0); add(25, -2.54, 0.050, 120, 1); add(1, 4.52, -0.008, 58, 0); add(19, -6.62, 0.010, 181, 1);
        const dgps::Stats s = run(frame(h, dgps::bodyType1(sats)), 4800, 12.0);
        const dgps::Message* m = lastOf(s, 1);
        CHECK(m && m->h.stationId == 815 && m->h.zcount == 4083 && m->h.seq == 1 && m->sats.size() == 5, "type 1: found %d, %zu satellites", m != nullptr, m ? m->sats.size() : 0);
        if (m && m->sats.size() == 5) {
            CHECK(m->sats[0].sat == 24 && std::fabs(m->sats[0].prc() + 4.76) < 1e-6 && std::fabs(m->sats[0].rrc() - 0.012) < 1e-9 && m->sats[0].iod == 207, "sat 24: %d %.2f %.3f %d", m->sats[0].sat, m->sats[0].prc(), m->sats[0].rrc(), m->sats[0].iod);
            CHECK(m->sats[1].sat == 4 && std::fabs(m->sats[1].prc() - 1.66) < 1e-6 && std::fabs(m->sats[1].rrc() + 0.022) < 1e-9, "sat 4");
            CHECK(m->sats[4].sat == 19 && std::fabs(m->sats[4].prc() + 6.62) < 1e-6 && m->sats[4].iod == 181, "sat 19");
        }
        // the scale-factor bit: 0.32 m and 0.032 m/s units
        dgps::SatCorr big; big.sat = 32; big.sf = true; big.prcRaw = -300; big.rrcRaw = 100; big.iod = 9;
        const dgps::Stats sb = run(frame(h, dgps::bodyType1({big, big, big})), 4800, 0.0);
        const dgps::Message* mb = lastOf(sb, 1);
        CHECK(mb && mb->sats.size() == 3 && mb->sats[0].sat == 32 && std::fabs(mb->sats[0].prc() + 96.0) < 1e-9 && std::fabs(mb->sats[0].rrc() - 3.2) < 1e-9, "scale factor");
    }
    {   // type 3: the three radiobeacons of the paper's figure 5.4
        struct Ex { double x, y, z, lat, lon, h; } ex[] = {
            {3705136.80, 514898.59, 5148735.87, 54 + 10 / 60.0 + 59 / 3600.0, 7 + 54 / 60.0 + 42 / 3600.0, 52.2},
            {3579683.44, 508397.25, 5236838.89, 55 + 33 / 60.0 + 28 / 3600.0, 8 + 5 / 60.0, 99.9},
            {3252028.07, 277209.65, 5461558.56, 59 + 18 / 60.0 + 24 / 3600.0, 4 + 52 / 60.0 + 20 / 3600.0, 121.2}};
        for (int i = 0; i < 3; i++) {
            dgps::Header h; h.type = 3; h.stationId = i ? (i == 1 ? 705 : 815) : 492; h.zcount = 5021; h.seq = 7;
            const dgps::Stats s = run(frame(h, dgps::bodyType3(ex[i].x, ex[i].y, ex[i].z)), 4800, -5.0);
            const dgps::Message* m = lastOf(s, 3);
            CHECK(m && m->hasPosition && std::fabs(m->x - ex[i].x) < 0.006 && std::fabs(m->y - ex[i].y) < 0.006 && std::fabs(m->z - ex[i].z) < 0.006, "type 3 example %d: ECEF", i);
            if (m) CHECK(std::fabs(m->lat - ex[i].lat) < 1.0 / 3600 && std::fabs(m->lon - ex[i].lon) < 1.0 / 3600 && std::fabs(m->height - ex[i].h) < 0.2, "type 3 example %d: %.5f %.5f %.1f m (want %.5f %.5f %.1f)", i, m->lat, m->lon, m->height, ex[i].lat, ex[i].lon, ex[i].h);
        }
    }
    {   // type 7: "52°00'n 4°07'e 120 km 287.5 kHz Health 0 Tx 425 200 bps" and a second beacon
        dgps::Header h; h.type = 7; h.stationId = 469; h.zcount = 5547; h.seq = 3;
        dgps::Almanac a, b;
        a.latitude = 52.0; a.longitude = 4 + 7 / 60.0; a.rangeKm = 120; a.frequencyKhz = 287.5; a.health = 0; a.stationId = 425; a.bitRate = 200;
        b.latitude = 56 + 18 / 60.0; b.longitude = 12 + 27 / 60.0; b.rangeKm = 150; b.frequencyKhz = 293.5; b.health = 1; b.stationId = 466; b.bitRate = 100; b.modMode = 0; b.syncType = 1; b.encoding = 0;
        const dgps::Stats s = run(frame(h, dgps::bodyType7({a, b})), 4800, 3.0);
        const dgps::Message* m = lastOf(s, 7);
        CHECK(m && m->almanac.size() == 2, "type 7 found %d", m != nullptr);
        if (m && m->almanac.size() == 2) {
            const auto& x = m->almanac[0]; const auto& y = m->almanac[1];
            CHECK(std::fabs(x.latitude - 52.0) < 0.006 && std::fabs(x.longitude - (4 + 7 / 60.0)) < 0.006 && x.rangeKm == 120 && std::fabs(x.frequencyKhz - 287.5) < 1e-9 && x.stationId == 425 && x.bitRate == 200 && x.health == 0, "beacon 1: %.3f %.3f %d km %.1f kHz id %d %d bit/s", x.latitude, x.longitude, x.rangeKm, x.frequencyKhz, x.stationId, x.bitRate);
            CHECK(std::fabs(y.latitude - (56 + 18 / 60.0)) < 0.006 && std::fabs(y.longitude - (12 + 27 / 60.0)) < 0.006 && y.rangeKm == 150 && std::fabs(y.frequencyKhz - 293.5) < 1e-9 && y.stationId == 466 && y.bitRate == 100 && y.health == 1 && y.syncType == 1, "beacon 2");
        }
        // southern and western hemispheres (two's complement)
        dgps::Almanac c; c.latitude = -33.9; c.longitude = -151.2; c.rangeKm = 300; c.frequencyKhz = 310.0; c.stationId = 1023; c.bitRate = 50;
        const dgps::Stats s2 = run(frame(h, dgps::bodyType7({c})), 4800, 0);
        const dgps::Message* m2 = lastOf(s2, 7);
        CHECK(m2 && m2->almanac.size() == 1 && std::fabs(m2->almanac[0].latitude + 33.9) < 0.006 && std::fabs(m2->almanac[0].longitude + 151.2) < 0.006 && m2->almanac[0].stationId == 1023 && m2->almanac[0].bitRate == 50, "type 7 south and west");
    }
    {   // type 16 text, type 5 health, type 6 null frame
        dgps::Header h; h.type = 16; h.stationId = 3; h.zcount = 100;
        const dgps::Stats s = run(frame(h, dgps::bodyType16("HELLO FROM THE BEACON")), 4800, 0);
        const dgps::Message* m = lastOf(s, 16);
        CHECK(m && m->text == "HELLO FROM THE BEACON", "type 16 text '%s'", m ? m->text.c_str() : "-");
        dgps::SatHealth a, b; a.sat = 7; a.dataHealth = 3; a.cn0 = 20; a.healthEnable = true; a.timeToUnhealthy = 9; a.lossWarning = true; b.sat = 32; b.cn0 = 31; b.newNavData = true; b.iodLink = true;
        h.type = 5;
        const dgps::Stats s5 = run(frame(h, dgps::bodyType5({a, b})), 4800, 0);
        const dgps::Message* m5 = lastOf(s5, 5);
        CHECK(m5 && m5->satHealth.size() == 2 && m5->satHealth[0].sat == 7 && m5->satHealth[0].dataHealth == 3 && m5->satHealth[0].cn0 == 20 && m5->satHealth[0].healthEnable && m5->satHealth[0].lossWarning &&
              m5->satHealth[0].timeToUnhealthy == 9 && m5->satHealth[1].sat == 32 && m5->satHealth[1].newNavData && m5->satHealth[1].iodLink, "type 5");
        h.type = 6; h.length = 0;
        const dgps::Stats s6 = run(frame(h, {}), 4800, 0);
        CHECK(lastOf(s6, 6) != nullptr, "type 6 null frame");
    }
    // ---- sequences, both polarities, a corrupted word, noise only
    {
        dgps::Header h; h.type = 9; h.stationId = 705; h.zcount = 1802; h.seq = 6;
        std::vector<dgps::SatCorr> sats(3);
        for (int i = 0; i < 3; i++) { sats[i].sat = 5 + 7 * i; sats[i].prcRaw = -700 + 400 * i; sats[i].rrcRaw = 10 * i - 7; sats[i].iod = 60 + i; }
        for (int pol = 0; pol < 2; pol++) {
            std::vector<uint8_t> bits;
            for (int k = 0; k < 5; k++) {
                h.zcount = 1802 + 3 * k; h.seq = (6 + k) & 7;
                const auto f = frame(h, dgps::bodyType1(sats), pol);
                bits.insert(bits.end(), f.begin() + (k ? 137 : 0), f.end());
            }
            const dgps::Stats s = run(bits, 4800, 7.0);
            CHECK(s.messagesComplete == 5 && s.wordsBad == 0, "pol %d: %d complete messages, bad words %d (the channel polarity does not matter: it inverts D29*, D30* and the data together)", pol, s.messagesComplete, s.wordsBad);
            CHECK(s.carrierOffsetHz > 5 && s.carrierOffsetHz < 9, "carrier offset %.1f", s.carrierOffsetHz);
        }
        // one flipped bit in a body word: the word fails, the message is not complete, nothing wrong is reported
        auto f = frame(h, dgps::bodyType1(sats));
        f[137 + 2 * 30 + 12] ^= 1;
        const dgps::Stats s = run(f, 4800, 0);
        CHECK(s.framesOk == 1 && s.messagesComplete == 0 && s.wordsBad == 1 && lastOf(s, 9) == nullptr, "corrupted word: frames %d complete %d bad words %d", s.framesOk, s.messagesComplete, s.wordsBad);
        // noise only
        std::vector<uint8_t> noise;
        unsigned seed = 12345u;
        for (int i = 0; i < 6000; i++) { seed = seed * 1103515245u + 12345u; noise.push_back((seed >> 16) & 1); }
        const dgps::Stats sn = run(noise, 4800, 0.0);
        CHECK(sn.framesOk == 0 && sn.messagesComplete == 0, "random bits give %d headers", sn.framesOk);
    }
    // ---- sweep: bit rate, sample rate, carrier offset, clock error, polarity, noise; a long run so the clock has to be followed
    {
        dgps::Header h; h.type = 1; h.stationId = 77; h.zcount = 10; h.seq = 0;
        std::vector<dgps::SatCorr> sats(4);
        for (int i = 0; i < 4; i++) { sats[i].sat = 3 + 5 * i; sats[i].prcRaw = 123 * i - 200; sats[i].rrcRaw = 5 * i; sats[i].iod = 20 + i; }
        int cases = 0, bad = 0;
        for (double rate : {200.0, 100.0, 50.0})
            for (double fsMul : {16.0, 24.0, 40.0})
                for (double off : {-60.0, 0.0, 85.0})
                    for (double clk : {-0.0004, 0.0, 0.0004})
                        for (int pol = 0; pol < 2; pol++) {
                            std::vector<uint8_t> bits(100, 0);
                            for (size_t i = 0; i < bits.size(); i++) bits[i] = (uint8_t)((i * 5 + (i >> 2)) & 1);
                            const int nmsg = rate == 200 ? 12 : 6;
                            for (int k = 0; k < nmsg; k++) {
                                h.zcount = 10 + k; h.seq = k & 7;
                                const auto f = frame(h, dgps::bodyType1(sats), pol);
                                bits.insert(bits.end(), f.begin() + 137, f.end());
                            }
                            const dgps::Stats s = run(bits, rate * fsMul, off, rate, 1.0 + clk, 0.03, true, (unsigned)cases + 3);
                            cases++;
                            if (s.messagesComplete < nmsg - 1 || s.wordsBad > 1) { bad++; std::printf("sweep miss rate %.0f fs x%.0f off %.0f clk %+.4f pol %d: %d of %d complete, %d bad words\n", rate, fsMul, off, clk, pol, s.messagesComplete, nmsg, s.wordsBad); }
                        }
        CHECK(bad == 0, "sweep: %d of %d cases failed", bad, cases);
        std::printf("dgps sweep: %d cases, %d failed\n", cases, bad);
    }
    // ---- the engine receiver: three bit rates side by side, the right one wins
    {
        dgps::Header h; h.type = 16; h.stationId = 99; h.zcount = 5;
        for (double rate : {200.0, 100.0, 50.0}) {
            std::vector<uint8_t> bits(110, 0);
            for (size_t i = 0; i < bits.size(); i++) bits[i] = (uint8_t)((i * 3 + (i >> 1)) & 1);
            for (int k = 0; k < (rate == 200 ? 4 : 2); k++) { const auto f = frame(h, dgps::bodyType16("TEST BEACON"), 0); bits.insert(bits.end(), f.begin() + 137, f.end()); }
            const double fs = 960000;
            auto iq = dgps::modulate(bits, fs, -50000.0 + 10.0, 0.5f, 1.0, rate);
            DgpsReceiver r;
            r.configure(fs);
            r.setSignalOffset(-50000.0);
            r.feed(iq.data(), iq.size());
            std::vector<std::complex<float>> pad((size_t)fs, std::complex<float>(0.3f, 0));
            r.feed(pad.data(), pad.size());
            DgpsTelemetry t;
            CHECK(r.telemetry(t, 0) && t.dataValid && t.stationId == 99 && t.bitRate == rate && t.specialText == "TEST BEACON", "engine at %.0f bit/s: valid %d station %d rate %.0f text '%s'", rate, (int)t.dataValid, t.stationId, t.bitRate, t.specialText.c_str());
        }
    }
    printf(fails ? "dgps: %d failure(s)\n" : "dgps: all checks passed\n", fails);
    return fails ? 1 : 0;
}
