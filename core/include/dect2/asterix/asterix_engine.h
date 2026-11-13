// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ASTERIX surveillance data decoder: UDP datagrams (unicast or multicast) or a recorded file in, a track table out. No radio is involved: a radar's
// own pulses cannot be demodulated by a software defined radio in any useful way, so this mode takes the data a radar or an ADS-B / multilateration
// ground station sends on a network (categories 021, 048 and 062, asterix_proto.h). Engine standard 153 (family 151, "ASTERIX"); the receiver lives in
// the engine object but runs on its own threads, so it works whether or not a radio is started.
// Receive only. The UDP port is closed until the user starts listening, and listens on this computer only unless the user lets others send.
#pragma once
#include "dect2/asterix/asterix_proto.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AsterixTrack {
    uint64_t key = 0;
    int cat = 0, sac = -1, sic = -1;
    int trackNo = -1;
    int64_t address = -1;
    std::string callsign;
    int squawk = -1;
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool posReported = false;         // WGS-84 in the record (else worked out from range and azimuth, or Cartesian, with the origin: approximate)
    bool hasLevel = false;
    double flightLevel = 0;
    bool hasAltFt = false;
    double altFt = 0;
    bool hasGs = false;
    double gsKt = 0;
    bool hasTrack = false;
    double trackDeg = 0;
    bool hasVrate = false;
    double vrateFpm = 0;
    bool onGround = false;
    std::string kind, dep, dst;
    uint32_t reports = 0;
    double ageS = 0;                  // seconds since the last report (receiver clock; 0 for tracks from a file)
    double todS = -1;                 // time of day of the last record that had one
    std::vector<std::pair<float, float>> trail;   // lat, lon, the last 40 points
};

struct AsterixSource {
    int sac = 0, sic = 0;
    uint32_t perCat[3] = {};           // 021, 048, 062
    uint32_t reports = 0;
    double ageS = 0;
};

struct AsterixTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 idle, 1 listening / file / demo, 2 tracks held
    double cfoHz = 0;
    bool dataValid = false;
    float snrDb = 0;
    uint64_t blocksOk = 0, blocksBad = 0;
    std::string input;                 // "UDP port 8600", "file x.ast", "demo", "idle"
    std::string error;
    bool listening = false;
    bool demo = false;
    uint64_t datagrams = 0, bytes = 0;
    asterix::BlockStats stats;
    uint64_t noIdentity = 0;           // records (plots) that could not be given a track
    std::vector<AsterixTrack> tracks;  // most recently heard first, at most 3000
    std::vector<AsterixSource> sources;
    std::vector<std::string> recent;   // the last 80 records, one line each, oldest first
    bool haveOrigin = false;
    double originLat = 0, originLon = 0;
    double fileFraction = -1;          // while a file is being read: 0..1, else -1
};

// The payloads (UDP data) of a classic pcap capture (Ethernet, Linux cooked or raw IP link layer; IPv4, not fragmented). False with err for another format.
bool asterixPcapPayloads(const std::vector<uint8_t>& file, std::vector<std::vector<uint8_t>>& out, std::string& err);
// A position from a radar's range and azimuth (slant range corrected by the altitude, spherical earth, no refraction) and from Cartesian metres: approximate
void asterixPolarToLatLon(double originLat, double originLon, double rhoNm, double thetaDeg, double altFt, double& lat, double& lon);
void asterixXyToLatLon(double originLat, double originLon, double xM, double yM, double& lat, double& lon);

class AsterixReceiver {
public:
    AsterixReceiver();
    ~AsterixReceiver();
    // The engine contract; the receiver does not use the radio's samples
    void configure(double) {}
    void setSignalOffset(double) {}
    void setCenterHz(double) {}
    bool ready() const { return true; }
    void reset() {}
    void feed(const cf32*, size_t) {}
    bool telemetry(AsterixTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    // Input. All of these may be called from the interface thread at any time.
    bool startUdp(int port, bool allowOtherComputers, const std::string& multicastGroup, std::string& err);
    void stopUdp();
    bool loadFile(const std::string& path, std::string& err);    // raw data blocks or a pcap capture; read on a worker thread
    void setDemo(bool on);                                         // made-up aircraft in all three categories, one block a second per source
    // Feed bytes as if they came from the network (tests, the demo, the file reader)
    void ingest(const uint8_t* d, size_t n, double nowS = -1);
    void clear();
    void setOrigin(double latDeg, double lonDeg, bool valid);      // where a radar (or the system centre) is: for range / azimuth and Cartesian positions
    void setMaxAge(double seconds);                                // live tracks are dropped after this long without a report (default 60 s, 0 = never)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning asterixTuning();
std::string asterixSummary(const AsterixTelemetry& t);
std::unique_ptr<ModeSynth> makeAsterixSynth(const SynthConfig& cfg, double sampleRate);   // silence: the mode needs no samples, but the engine can still be started

// The demo traffic: the block (several records) that source k (0 = CAT 048 radar, 1 = CAT 021 ground station, 2 = CAT 062 system tracks) sends at second t
// for a made-up airspace around (lat, lon). Exposed for the tests.
std::vector<uint8_t> asterixDemoBlock(int source, double t, double lat, double lon);

}  // namespace dect2
