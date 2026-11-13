// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Lightning position (network): the mode's receiver. It times the sferics of this radio (StationCore) and, by role, only shows them, sends them to a
// server, or collects them from the clients and locates the strikes (ServerCore). Stations talk over UDP datagrams authenticated with a shared
// network key: off until the user sets a role and a key, nothing is relayed anywhere else, and a datagram that does not authenticate is dropped.
// Engine standard 792, active standard 791, family 790.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/sferics/ltpos.h"
#include "dect2/sferics/ltpos_tel.h"
#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct LtPosSettings {
    int role = 0;                 // 0 this station only, 1 client, 2 server
    std::string host;             // client: the server's address
    int port = 47390;             // UDP, client and server
    std::string key;              // the network key, at least 12 characters; empty: no network
    std::string name;             // this station's name
    double lat = 0, lon = 0, altM = 0;
    bool posSet = false;          // without a position this station's detections are not sent
    bool serverIsStation = true;  // a server's own radio counts as a station too
    int sysKind = 1;              // the computer's clock: 0 not synchronised, 1 NTP, 2 PTP
    double sysSigmaUs = 0;        // 0: the typical figure
    bool useHwTime = true;
    int refId = 0;                // the common-view reference, 0 none (ltpos::builtinRefs ids)
    double thresholdDb = 12;
    double velocityFactor = 0.9993;
    bool hops = false;
    double hopHeightKm = 80;
    int minStations = 4;          // 3 or more
};

class LtPosReceiver {
public:
    LtPosReceiver();
    ~LtPosReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);          // not used: the impulses are wideband
    void setCenterHz(double hz);              // where the reference carrier falls in the capture depends on it
    bool ready() const;
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    bool telemetry(LtPosTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setClockKey(const void* key);        // the engine's ring: identifies this radio's hardware time (source/hw_time.h)
    void apply(const LtPosSettings& s);       // from the UI; starts, restarts or stops the network as needed
    void addExternalMark(const ltpos::TimeMark& m);   // OnAir's GNSS receiver on a second radio, or a test
    void clearStrikes();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning ltposTuning();
std::string ltposSummary(const LtPosTelemetry& t);

}  // namespace dect2
