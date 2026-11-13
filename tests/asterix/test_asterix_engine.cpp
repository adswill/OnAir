// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ASTERIX receiver: the demo traffic of the three categories merged into a track table, positions from range and azimuth, UDP input on the loopback
// interface, a raw file and a pcap capture, live tracks aging out while file tracks stay, and the origin-less case.
#include "dect2/asterix/asterix_engine.h"
#include "../../core/src/net/netcompat.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static double haversineKm(double la1, double lo1, double la2, double lo2) {
    const double p = 3.14159265358979323846 / 180.0, R = 6371.0088;
    const double a = std::sin((la2 - la1) * p / 2) * std::sin((la2 - la1) * p / 2) + std::cos(la1 * p) * std::cos(la2 * p) * std::sin((lo2 - lo1) * p / 2) * std::sin((lo2 - lo1) * p / 2);
    return 2 * R * std::asin(std::sqrt(a));
}
static double bearingDeg(double la1, double lo1, double la2, double lo2) {
    const double p = 3.14159265358979323846 / 180.0;
    const double y = std::sin((lo2 - lo1) * p) * std::cos(la2 * p), x = std::cos(la1 * p) * std::sin(la2 * p) - std::sin(la1 * p) * std::cos(la2 * p) * std::cos((lo2 - lo1) * p);
    double b = std::atan2(y, x) / p;
    return b < 0 ? b + 360 : b;
}
static AsterixTelemetry snapshot(AsterixReceiver& rx) {
    AsterixTelemetry t;
    rx.telemetry(t, 0);
    return t;
}
static std::string tmpPath(const char* name) {
    const char* d = std::getenv("TMPDIR");
    return std::string(d && *d ? d : "/tmp") + "/" + name;
}
static void put32(std::vector<uint8_t>& v, uint32_t x, bool little) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(little ? x >> (8 * i) : x >> (8 * (3 - i)))); }

int main() {
    // ---- geometry
    {
        double lat, lon;
        asterixPolarToLatLon(50.0, 14.0, 60.0, 90.0, 0, lat, lon);
        CHECK(std::fabs(haversineKm(50, 14, lat, lon) - 60 * 1.852) < 0.2, "60 NM east: %.3f km", haversineKm(50, 14, lat, lon));
        CHECK(std::fabs(bearingDeg(50, 14, lat, lon) - 90.0) < 0.05, "bearing %.3f", bearingDeg(50, 14, lat, lon));
        asterixPolarToLatLon(50.0, 14.0, 100.0, 215.0, 35000, lat, lon);
        const double dKm = haversineKm(50, 14, lat, lon);
        const double slantKm = 100 * 1.852, hKm = 35000 * 0.3048 / 1000.0;
        CHECK(std::fabs(dKm - std::sqrt(slantKm * slantKm - hKm * hKm)) < 0.2, "slant range corrected by the altitude: %.3f km", dKm);
        CHECK(std::fabs(bearingDeg(50, 14, lat, lon) - 215.0) < 0.1, "bearing 215: %.3f", bearingDeg(50, 14, lat, lon));
        asterixXyToLatLon(50.0, 14.0, 0, 20000, lat, lon);
        CHECK(std::fabs(haversineKm(50, 14, lat, lon) - 20.0) < 0.05 && lat > 50.0, "20 km north");
    }
    // ---- the demo traffic, ingested
    {
        AsterixReceiver rx;
        rx.setOrigin(50.0, 14.0, true);
        for (int s = 0; s < 3; s++) {
            const std::vector<uint8_t> b = asterixDemoBlock(s, 12.0, 50.0, 14.0);
            rx.ingest(b.data(), b.size());
        }
        const AsterixTelemetry t = snapshot(rx);
        CHECK(t.tracks.size() == 16, "tracks %zu, want 5 + 6 + 5", t.tracks.size());
        CHECK(t.sources.size() == 3, "sources %zu", t.sources.size());
        CHECK(t.stats.perCat[48] == 1 && t.stats.perCat[21] == 1 && t.stats.perCat[62] == 1 && t.stats.records == 16, "blocks / records");
        CHECK(t.state == 2 && t.dataValid, "state %d", t.state);
        int withPos = 0, reported = 0;
        for (const auto& a : t.tracks) {
            withPos += a.hasPos;
            reported += a.posReported;
            CHECK(a.hasPos, "track %d of CAT %d has no position", a.trackNo, a.cat);
            if (a.hasPos) {
                const double d = haversineKm(50, 14, a.lat, a.lon) / 1.852;
                CHECK(d > 15 && d < 105, "track %d is %.1f NM from the origin", a.trackNo, d);
            }
            CHECK(!a.callsign.empty() && a.hasLevel && a.hasGs, "track %d: fields", a.trackNo);
        }
        CHECK(withPos == 16 && reported == 11, "%d positions, %d reported (11 expected: the radar's five come from range and azimuth)", withPos, reported);
        // the same aircraft later: the track is updated, not duplicated
        const std::vector<uint8_t> b = asterixDemoBlock(1, 13.0, 50.0, 14.0);
        rx.ingest(b.data(), b.size());
        const AsterixTelemetry t2 = snapshot(rx);
        CHECK(t2.tracks.size() == 16, "still 16 tracks, got %zu", t2.tracks.size());
        for (const auto& a : t2.tracks) if (a.cat == 21) CHECK(a.reports == 2 && a.trail.size() == 2, "ADS-B track reports %u trail %zu", a.reports, a.trail.size());
        // the viewer lines
        CHECK(t2.recent.size() >= 16 && t2.recent.back().find("CAT 021") != std::string::npos, "recent lines");
        // clear
        rx.clear();
        CHECK(snapshot(rx).tracks.empty(), "clear");
    }
    // ---- no origin: the radar's polar targets have no position, the others do
    {
        AsterixReceiver rx;
        const std::vector<uint8_t> b = asterixDemoBlock(0, 3.0, 50.0, 14.0);
        rx.ingest(b.data(), b.size());
        const AsterixTelemetry t = snapshot(rx);
        CHECK(t.tracks.size() == 5, "tracks %zu", t.tracks.size());
        for (const auto& a : t.tracks) CHECK(!a.hasPos && a.hasLevel, "no origin, no position");
        rx.setOrigin(50.0, 14.0, true);
        rx.ingest(b.data(), b.size());
        for (const auto& a : snapshot(rx).tracks) CHECK(a.hasPos && !a.posReported, "position from the origin");
    }
    // ---- aging: live tracks go, file tracks stay
    {
        AsterixReceiver rx;
        rx.setMaxAge(0.3);
        const std::vector<uint8_t> b = asterixDemoBlock(1, 1.0, 50.0, 14.0);
        rx.ingest(b.data(), b.size());
        const std::vector<uint8_t> f = asterixDemoBlock(2, 1.0, 50.0, 14.0);
        rx.ingest(f.data(), f.size(), -2);
        CHECK(snapshot(rx).tracks.size() == 11, "11 tracks at first");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        const AsterixTelemetry t = snapshot(rx);
        CHECK(t.tracks.size() == 5, "the live tracks aged out, the file tracks stayed: %zu", t.tracks.size());
        for (const auto& a : t.tracks) CHECK(a.cat == 62 && a.ageS == 0, "file track");
    }
    // ---- UDP on the loopback interface
    {
        AsterixReceiver rx;
        std::string err;
        const int port = 38000 + (int)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000);
        CHECK(!rx.startUdp(0, false, "", err) && !err.empty(), "port 0 refused");
        err.clear();
        CHECK(!rx.startUdp(port, false, "10.0.0.1", err) && !err.empty(), "a unicast address is not a multicast group");
        err.clear();
        if (!rx.startUdp(port, false, "", err)) { std::printf("note: cannot listen on UDP %d here (%s), UDP part skipped\n", port, err.c_str()); }
        else {
            netInit();
            sock_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in to;
            std::memset(&to, 0, sizeof to);
            to.sin_family = AF_INET; to.sin_port = htons((uint16_t)port); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            for (int k = 0; k < 3; k++) {
                const std::vector<uint8_t> b = asterixDemoBlock(k, 5.0, 50.0, 14.0);
                sockSendTo(s, b.data(), b.size(), to);
            }
            const uint8_t junk[] = {0x22, 0x00, 0x04, 0x00};      // CAT 034, not one of ours
            sockSendTo(s, junk, sizeof junk, to);
            AsterixTelemetry t;
            for (int i = 0; i < 40; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                t = snapshot(rx);
                if (t.tracks.size() >= 11 && t.datagrams >= 4) break;
            }
            sockClose(s);
            CHECK(t.listening && t.input.find("this computer only") != std::string::npos, "input '%s'", t.input.c_str());
            CHECK(t.datagrams == 4 && t.tracks.size() == 16, "datagrams %llu tracks %zu", (unsigned long long)t.datagrams, t.tracks.size());
            CHECK(t.stats.otherCategory == 1, "the other-category block is counted: %u", t.stats.otherCategory);
            rx.stopUdp();
            t = snapshot(rx);
            CHECK(!t.listening, "stopped");
        }
    }
    // ---- a raw file, and the same data in a pcap capture
    {
        std::vector<uint8_t> raw;
        std::vector<std::vector<uint8_t>> dgs;
        for (int k = 0; k < 3; k++) for (int i = 0; i < 3; i++) {
            const std::vector<uint8_t> b = asterixDemoBlock(k, 2.0 + i, 50.0, 14.0);
            raw.insert(raw.end(), b.begin(), b.end());
            dgs.push_back(b);
        }
        const std::string pr = tmpPath("onair_asterix_test.ast");
        { std::ofstream o(pr, std::ios::binary); o.write((const char*)raw.data(), (std::streamsize)raw.size()); }
        // pcap, Ethernet / IPv4 / UDP, both byte orders
        for (int le = 0; le < 2; le++) {
            std::vector<uint8_t> pc;
            put32(pc, 0xA1B2C3D4u, le == 1);
            if (le == 0) { pc.clear(); pc.insert(pc.end(), {0xA1, 0xB2, 0xC3, 0xD4}); }
            else { pc.clear(); pc.insert(pc.end(), {0xD4, 0xC3, 0xB2, 0xA1}); }
            auto w16 = [&](uint16_t x) { if (le) { pc.push_back((uint8_t)x); pc.push_back((uint8_t)(x >> 8)); } else { pc.push_back((uint8_t)(x >> 8)); pc.push_back((uint8_t)x); } };
            w16(2); w16(4); put32(pc, 0, le == 1); put32(pc, 0, le == 1); put32(pc, 65535, le == 1); put32(pc, 1, le == 1);
            for (const auto& d : dgs) {
                std::vector<uint8_t> pkt = {1, 0, 0x5E, 0, 0, 1, 2, 3, 4, 5, 6, 7, 0x08, 0x00};            // Ethernet
                const uint16_t ulen = (uint16_t)(8 + d.size()), tlen = (uint16_t)(20 + ulen);
                pkt.insert(pkt.end(), {0x45, 0, (uint8_t)(tlen >> 8), (uint8_t)tlen, 0, 0, 0, 0, 16, 17, 0, 0, 10, 0, 0, 1, 239, 1, 2, 3});   // IPv4, UDP
                pkt.insert(pkt.end(), {0x21, 0x98, 0x21, 0x98, (uint8_t)(ulen >> 8), (uint8_t)ulen, 0, 0});
                pkt.insert(pkt.end(), d.begin(), d.end());
                put32(pc, 1, le == 1); put32(pc, 0, le == 1); put32(pc, (uint32_t)pkt.size(), le == 1); put32(pc, (uint32_t)pkt.size(), le == 1);
                pc.insert(pc.end(), pkt.begin(), pkt.end());
            }
            std::vector<std::vector<uint8_t>> pl;
            std::string err;
            CHECK(asterixPcapPayloads(pc, pl, err) && pl.size() == dgs.size(), "pcap payloads %zu (%s)", pl.size(), err.c_str());
            if (pl.size() == dgs.size()) for (size_t i = 0; i < pl.size(); i++) CHECK(pl[i] == dgs[i], "payload %zu differs", i);
            const std::string pp = tmpPath(le ? "onair_asterix_test_le.pcap" : "onair_asterix_test_be.pcap");
            { std::ofstream o(pp, std::ios::binary); o.write((const char*)pc.data(), (std::streamsize)pc.size()); }
            AsterixReceiver rx;
            CHECK(rx.loadFile(pp, err), "load pcap: %s", err.c_str());
            AsterixTelemetry t;
            for (int i = 0; i < 50; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); t = snapshot(rx); if (t.stats.records >= 48 && t.fileFraction < 0) break; }
            CHECK(t.tracks.size() == 16 && t.stats.records == 48, "pcap %d: %zu tracks, %u records", le, t.tracks.size(), t.stats.records);
            CHECK(t.datagrams == 9, "datagrams %llu", (unsigned long long)t.datagrams);
            for (const auto& a : t.tracks) CHECK(a.ageS == 0, "file tracks have no age");
            std::remove(pp.c_str());
        }
        AsterixReceiver rx;
        std::string err;
        CHECK(rx.loadFile(pr, err), "load raw: %s", err.c_str());
        AsterixTelemetry t;
        for (int i = 0; i < 50; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); t = snapshot(rx); if (t.stats.records >= 48 && t.fileFraction < 0) break; }
        CHECK(t.tracks.size() == 16 && t.stats.blocks == 9 && t.input.find("onair_asterix_test.ast") != std::string::npos, "raw: %zu tracks %u blocks '%s'", t.tracks.size(), t.stats.blocks, t.input.c_str());
        // not a recognised file
        const std::string bad = tmpPath("onair_asterix_test_bad.bin");
        { std::ofstream o(bad, std::ios::binary); const char junk[] = "this is not ASTERIX data at all"; o.write(junk, sizeof junk); }
        CHECK(!rx.loadFile(bad, err) && !err.empty(), "a text file should be refused");
        CHECK(!rx.loadFile(tmpPath("onair_asterix_no_such_file"), err), "a missing file");
        std::remove(bad.c_str());
        std::remove(pr.c_str());
        // pcapng is named
        std::vector<uint8_t> ng = {0x0A, 0x0D, 0x0D, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        std::vector<std::vector<uint8_t>> pl;
        err.clear();
        CHECK(!asterixPcapPayloads(ng, pl, err) && err.find("pcapng") != std::string::npos, "pcapng message '%s'", err.c_str());
    }
    // ---- the demo thread runs on its own
    {
        AsterixReceiver rx;
        rx.setDemo(true);
        AsterixTelemetry t;
        for (int i = 0; i < 30; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); t = snapshot(rx); if (t.tracks.size() >= 16) break; }
        CHECK(t.demo && t.input == "demo" && t.tracks.size() == 16, "demo: %zu tracks, input '%s'", t.tracks.size(), t.input.c_str());
        rx.setDemo(false);
        t = snapshot(rx);
        CHECK(!t.demo, "demo off");
        CHECK(!asterixSummary(t).empty(), "summary");
    }
    const ModeTuning mt = asterixTuning();
    CHECK(mt.stdMode == 153 && std::string(mt.id) == "asterix", "tuning");
    std::printf("%s: %d failure(s)\n", fails ? "test_asterix_engine FAILED" : "test_asterix_engine OK", fails);
    return fails ? 1 : 0;
}
