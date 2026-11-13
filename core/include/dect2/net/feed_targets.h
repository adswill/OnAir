// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Outbound feed targets: send ADS-B (Beast / SBS) and AIS (NMEA) to aggregators and other programs you name.
// Every target is OFF by default; nothing is sent, and no connection is opened, until the user enables that one target.
// One decoder serves any number of targets (fan-out). TCP targets reconnect with a growing wait; UDP targets just send.
// The user's own host, port and key (the account the aggregator gave them) are entered by the user; none is built in.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

enum class FeedKind { AdsbBeast, AdsbSbs, AisNmea };
enum class FeedProto { Tcp, Udp };

struct FeedTarget {
    std::string name;
    FeedKind kind = FeedKind::AdsbBeast;
    FeedProto proto = FeedProto::Tcp;
    std::string host;
    int port = 0;
    std::string key;         // optional: sent as the first line after a TCP connect (some services want an id); empty = nothing
    bool enabled = false;
};

struct FeedTargetStatus {
    bool connected = false;  // TCP: connected; UDP: socket ready
    uint64_t sent = 0, dropped = 0, reconnects = 0;
    std::string state, lastError;
};

struct FeedPreset {
    const char* name; FeedKind kind; FeedProto proto; const char* host; int port; const char* note;
};
// Presets are only a starting point for the fields. Where an aggregator hands each user their own address, host stays empty.
const std::vector<FeedPreset>& feedPresets();

class FeedTargets {
public:
    static FeedTargets& get();
    void configure(const std::vector<FeedTarget>& t);   // starts the thread on the first enabled target; connections of disabled ones close
    std::vector<FeedTarget> targets() const;
    FeedTargetStatus status(size_t i) const;
    bool wantAdsb() const;   // cheap: any enabled ADS-B target
    bool wantAis() const;
    void adsbBeast(const std::string& frame);   // already formatted (feedfmt::beastFrame / sbsLine)
    void adsbSbs(const std::string& line);
    void ais(const std::vector<std::string>& sentences);
    void stop();
    struct Impl;
private:
    FeedTargets();
    ~FeedTargets() = default;
    std::unique_ptr<Impl> impl_;
};

} // namespace dect2
