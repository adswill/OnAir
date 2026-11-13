// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared parts of the Winlink tests: a local mock gateway (the B2F gateway role on a link that listens) and a simulated air between
// the client station and it. Nothing here opens a network connection or a radio: the "air" is a queue in memory, or the packet / ARDOP
// generators feeding the receivers in the same process.
#pragma once
#include "dect2/ardop/ardop_proto.h"
#include "dect2/winlink/b2f.h"
#include "dect2/winlink/wl_link.h"
#include "dect2/winlink/wl_station.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace wltest {
using namespace dect2;
using namespace dect2::winlink;

constexpr const char* kGwCall = "LY0GW-10";
constexpr const char* kMyCall = "LY2TST";
constexpr const char* kPassword = "Secret-Pass1";
constexpr const char* kChallenge = "23753528";

inline std::string tempDir(const char* tag) {
    std::random_device rd;
    const auto p = std::filesystem::temp_directory_path() / ("onair-winlink-" + std::string(tag) + "-" + std::to_string(rd()));
    std::filesystem::create_directories(p);
    return p.string();
}

inline B2Message msg(const std::string& from, const std::string& to, const std::string& subj, const std::string& body, std::time_t t,
                     std::vector<B2File> files = {}) {
    return makeMessage(from, {to}, {}, subj, body, files, t);
}

// The mock gateway: B2F gateway role with a secure login challenge, the messages waiting for the caller, and what it received
struct MockGateway {
    std::vector<B2Message> waiting;          // for the caller
    std::set<std::string> callerHas;         // MIDs the gateway pretends it already has (answers '-')
    std::vector<B2Message> got;
    std::set<std::string> delivered;
    std::unique_ptr<B2fSession> s;
    std::string password = kPassword;
    std::vector<std::string> trace;
    bool quiet = true;

    void begin(std::function<void(const uint8_t*, size_t)> send) {
        B2fSession::Config c;
        c.role = B2fSession::Role::Gateway;
        c.mycall = "CMS"; c.targetCall = kMyCall;
        c.challenge = kChallenge;
        c.expectPassword = password;
        B2fSession::Hooks h;
        h.send = std::move(send);
        h.outbound = [this]() {
            std::vector<B2Proposal> v;
            for (const auto& m : waiting) if (!delivered.count(m.mid())) v.push_back(makeProposal(m));
            return v;
        };
        h.inboundAnswer = [this](const B2Proposal& p) { return callerHas.count(p.mid) ? '-' : '+'; };
        h.received = [this](const B2Message& m, const B2Proposal&) { got.push_back(m); };
        h.sent = [this](const std::string& mid, bool) { delivered.insert(mid); };
        h.log = [this](const std::string& l) { trace.push_back(l); if (!quiet) printf("  GW %s\n", l.c_str()); };
        s = std::make_unique<B2fSession>(c, h);
    }
};

inline std::string desc(const ax25::Frame& f) { char b[96]; snprintf(b, sizeof b, "%s>%s ctl %02X %s len %zu", f.from.str().c_str(), f.to.str().c_str(), f.control, f.typeName().c_str(), f.info.size()); return b; }
inline std::string desc(const ArdopLinkFrame& f) { char b[64]; snprintf(b, sizeof b, "ardop %02X sid %02X body %zu", f.code, f.sid, f.body.size()); return b; }

// Frames in flight on the simulated air
template <class F> struct Air {
    struct InFlight { double at; F f; };
    std::vector<InFlight> toGw, toClient;
    std::mt19937 rng{1};
    double loss = 0;
    double delay = 0.6;
    uint64_t dropped = 0, carried = 0;
    void send(std::vector<InFlight>& q, const F& f, double now) {
        const bool drop = loss > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < loss;
        if (getenv("WLDEBUG")) printf("%7.2f %s %s %s\n", now, &q == &toGw ? "->GW" : "->CL", drop ? "LOST" : "    ", desc(f).c_str());
        if (drop) { dropped++; return; }
        carried++;
        q.push_back({now + delay, f});
    }
    template <class Fn> void deliver(std::vector<InFlight>& q, double now, Fn fn) {
        std::vector<InFlight> due, keep;
        for (auto& x : q) (x.at <= now ? due : keep).push_back(x);
        q.swap(keep);
        for (auto& x : due) fn(x.f);
    }
};

// frame conversions for the simulated air
inline const ax25::Frame& toGwIn(const ax25::Frame& f) { return f; }
inline ArdopRxFrameIn toGwIn(const ArdopLinkFrame& f) {
    ArdopRxFrameIn o;
    o.code = f.code; o.sid = f.sid;
    ardop::Parsed p;
    const int need = ardop::bodyLength(f.code);
    o.bodyOk = need > 0 && ardop::parseBody(f.code, f.body.data(), f.body.size(), p) && p.bodyOk;
    o.from = p.from; o.to = p.to; o.payload = p.payload;
    return o;
}
inline void deliverClient(WinlinkStation& st, const ax25::Frame& f, double now) { st.rxAx25(f, now); }
inline void deliverClient(WinlinkStation& st, const ArdopLinkFrame& f, double now) { st.rxArdop(toGwIn(f), now); }

} // namespace wltest
