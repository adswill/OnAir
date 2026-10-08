// RS41: builder -> symbols -> decoder. Position, speed, time, battery, calibration progress, temperature and humidity (the builder inverts
// the decoder's formulas: this checks the plumbing and the unit handling, not the sensor model), frequency and kill timer.
#include "dect2/sonde_rs41.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const Rs41Cal cal = rs41MakeCal(403.0e6, 30600);
    Rs41Decoder dec;
    std::mt19937 rng(11);
    int ok = 0;
    std::vector<SondeFix> last;
    for (int fr = 0; fr < 60; fr++) {
        SondeTruth t;
        t.serial = "T4710042"; t.frame = 1000 + fr;
        t.lat = 25.20 + 0.0001 * fr; t.lon = 55.36 + 0.0002 * fr; t.altM = 12000 + 5.0 * fr;
        t.vSpeed = 5.0; t.hSpeed = 10.0; t.headingDeg = 90.0; t.sats = 9;
        t.unixTime = 1780272000.0 + fr; t.tempC = -56.5 + 0.1 * fr; t.humidity = 20 + 0.5 * fr; t.batteryV = 2.7;
        std::vector<uint8_t> f = rs41Frame(t, cal, fr % 51);
        std::vector<uint8_t> sym = rs41Symbols(f);
        for (int i = 0; i < 100; i++) sym.insert(sym.begin(), (uint8_t)(rng() & 1));
        std::vector<SondeFix> out;
        dec.push(sym.data(), sym.size(), fr * 1.0, out);
        CHECK(out.size() == 1, "frame %d: %zu fixes", fr, out.size());
        if (out.size() != 1) continue;
        const SondeFix& x = out[0];
        CHECK(x.crcOk && x.corrected == 0, "frame %d crc %d corr %d", fr, (int)x.crcOk, x.corrected);
        CHECK(x.serial == "T4710042" && x.frame == 1000 + fr, "ids %s %d", x.serial.c_str(), x.frame);
        CHECK(x.hasPos && std::fabs(x.lat - t.lat) < 2e-7 && std::fabs(x.lon - t.lon) < 2e-7 && std::fabs(x.altM - t.altM) < 0.02, "pos %.7f %.7f %.2f", x.lat, x.lon, x.altM);
        CHECK(x.hasVel && std::fabs(x.hSpeed - 10.0) < 0.02 && std::fabs(x.vSpeed - 5.0) < 0.02 && std::fabs(x.headingDeg - 90.0) < 0.2, "vel %.2f %.2f %.1f", x.hSpeed, x.vSpeed, x.headingDeg);
        CHECK(x.hasTime && std::fabs(x.unixTime - t.unixTime) < 0.002, "time %.3f", x.unixTime);
        CHECK(std::fabs(x.batteryV - 2.7) < 1e-9 && x.sats == 9, "battery/sats");
        const int done = std::min(fr + 1, 51);
        if (done < 51) CHECK(x.note == "calibrating " + std::to_string(done) + "/51", "note '%s' at frame %d", x.note.c_str(), fr);
        else CHECK(x.note.empty(), "note '%s'", x.note.c_str());
        // temperature needs subframes 3..6 (from the 7th frame on), humidity also 7 (from the 8th)
        if (fr >= 6) CHECK(x.hasTemp && std::fabs(x.tempC - t.tempC) < 0.01, "frame %d temp %.3f want %.3f", fr, x.tempC, t.tempC);
        else CHECK(!x.hasTemp, "temperature before calibration, frame %d", fr);
        if (fr >= 7) CHECK(x.hasHumidity && std::fabs(x.humidity - t.humidity) < 0.1, "frame %d rh %.2f want %.2f", fr, x.humidity, t.humidity);
        else CHECK(!x.hasHumidity, "humidity before calibration, frame %d", fr);
        CHECK(std::fabs(dec.announcedFreqHz() - (fr >= 0 ? 403.0e6 : 0)) < 1.0, "freq %.1f", dec.announcedFreqHz());
        if (fr >= 2) CHECK(x.burstKillS == 30600, "kill timer %d", x.burstKillS);
        ok++;
        last = out;
    }
    CHECK(dec.calibrationDone() == 51, "calibration %d", dec.calibrationDone());
    // a different sonde on the same decoder: the calibration starts again
    {
        SondeTruth t; t.serial = "U0000001"; t.frame = 5; t.lat = 25; t.lon = 55; t.altM = 100; t.sats = 8; t.unixTime = 1780272000; t.tempC = 20; t.humidity = 50; t.batteryV = 2.9;
        std::vector<SondeFix> out;
        std::vector<uint8_t> sym = rs41Symbols(rs41Frame(t, cal, 9));
        dec.push(sym.data(), sym.size(), 100.0, out);
        CHECK(out.size() == 1 && out[0].note == "calibrating 1/51" && !out[0].hasTemp, "new serial restarts calibration");
    }
    // a gap in the middle of a frame drops that frame and the next one still decodes
    {
        SondeTruth t; t.serial = "U0000001"; t.frame = 6; t.lat = 25; t.lon = 55; t.altM = 100; t.sats = 8; t.unixTime = 1780272001; t.tempC = 20; t.humidity = 50; t.batteryV = 2.9;
        std::vector<uint8_t> sym = rs41Symbols(rs41Frame(t, cal, 10));
        std::vector<SondeFix> out;
        dec.push(sym.data(), 1200, 200.0, out);
        t.frame = 7;
        std::vector<uint8_t> sym2 = rs41Symbols(rs41Frame(t, cal, 11));
        dec.push(sym2.data(), sym2.size(), 200.0 + 0.9, out);
        CHECK(out.size() == 1 && out[0].frame == 7 && out[0].crcOk, "after a gap: %zu fixes", out.size());
    }
    // an extended frame (type byte 0xF0, 518 bytes, codewords of 231 data bytes): built here from a normal one
    {
        SondeTruth t; t.serial = "X0000009"; t.frame = 77; t.lat = 25.2; t.lon = 55.36; t.altM = 9000; t.sats = 7; t.unixTime = 1780272000; t.tempC = -30; t.humidity = 40; t.batteryV = 2.8;
        std::vector<uint8_t> f = rs41Frame(t, cal, 4);
        f[0x38] = 0xF0;
        f.resize(kRs41FrameLenExt, 0);
        for (size_t i = 0x140; i < f.size(); i++) f[i] = (uint8_t)(i * 7);          // extra data after the blocks
        for (int w = 0; w < 2; w++) {
            uint8_t data[231];
            for (int i = 0; i < 231; i++) data[i] = f[(size_t)(56 + 2 * i + w)];
            rs41RsParity(data, 231, &f[(size_t)(8 + 24 * w)]);
        }
        std::vector<uint8_t> sym = rs41Symbols(f);
        sym[900] ^= 1; sym[1900] ^= 1; sym[2900] ^= 1; sym[3900] ^= 1;               // errors in the stretch beyond byte 320 and before it
        Rs41Decoder d4;
        std::vector<SondeFix> out;
        d4.push(sym.data(), sym.size(), 0.0, out);
        CHECK(out.size() == 1 && out[0].crcOk && out[0].serial == "X0000009" && out[0].frame == 77 && out[0].corrected >= 3, "extended frame: %zu fixes, crc %d", out.size(), out.empty() ? 0 : (int)out[0].crcOk);
        if (out.size() == 1) CHECK(out[0].hasPos && std::fabs(out[0].altM - 9000) < 0.02, "extended frame position");
    }
    // announced frequency in 10 kHz steps (rs41mod.c): every step of a 40 kHz cell
    for (double f : {402.37e6, 403.01e6, 404.02e6, 405.31e6}) {
        const Rs41Cal c2 = rs41MakeCal(f);
        Rs41Decoder d2;
        SondeTruth t; t.serial = "T4710043"; t.frame = 1; t.lat = 25.2; t.lon = 55.4; t.altM = 5000; t.sats = 9; t.unixTime = 1780272000.0;
        std::vector<SondeFix> out;
        std::vector<uint8_t> sym = rs41Symbols(rs41Frame(t, c2, 0));
        d2.push(sym.data(), sym.size(), 0.0, out);
        CHECK(std::fabs(d2.announcedFreqHz() - f) < 1.0, "announced %.1f want %.1f", d2.announcedFreqHz(), f);
    }
    if (fails) return 1;
    printf("ok (%d frames)\n", ok);
    return 0;
}
