// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The AX.25 frame parser, text forms, FCS and HDLC receiver step on top of the Rust crate rust/onair_ax25: the functions of packet_ax25.h;
// packet_ax25.cpp keeps frame building and the transmit-side bit helpers.
#include "dect2/packet/packet_ax25.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/ax25.h"
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace ax25 {

static_assert(sizeof(HdlcState) == sizeof(OnairAx25Hdlc), "HdlcState mirrors OnairAx25Hdlc");

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t f = 0, a = 0, i = 0, h = 0;
        onair_ax25_struct_sizes(&f, &a, &i, &h);
        dect2::rsAbiCheck("onair_ax25", {f, a, i, h}, {sizeof(OnairAx25Frame), sizeof(OnairAx25AddrIn), sizeof(OnairAprsInfo), sizeof(OnairAx25Hdlc)});
    }
} abiCheck;

OnairAx25AddrIn in(const Address& a) {
    OnairAx25AddrIn r{};
    r.call = reinterpret_cast<const uint8_t*>(a.call.data());
    r.call_len = a.call.size();
    r.ssid = a.ssid;
    r.h = a.h;
    return r;
}

// Runs a Rust text writer with a buffer that grows until the text fits
template <class F> std::string text(F f) {
    std::string s(256, '\0');
    for (;;) {
        const size_t n = f((uint8_t*)&s[0], s.size());
        if (n != SIZE_MAX) { s.resize(n); return s; }
        s.assign(s.size() * 4, '\0');
    }
}

Address fromRs(const OnairAx25Addr& a) {
    Address r;
    r.call.assign((const char*)a.call, a.call_len);
    r.ssid = a.ssid;
    r.h = a.h != 0;
    return r;
}
} // namespace

uint16_t fcs(const uint8_t* d, size_t n) { return onair_ax25_fcs(d, n); }
bool fcsOk(const uint8_t* d, size_t n) { return onair_ax25_fcs_ok(d, n); }

std::string Address::str() const {
    const OnairAx25AddrIn a = in(*this);
    return text([&](uint8_t* o, size_t c) { return onair_ax25_addr_str(&a, o, c); });
}

std::string Frame::pathStr() const {
    std::vector<OnairAx25AddrIn> p;
    for (const Address& a : path) p.push_back(in(a));
    return text([&](uint8_t* o, size_t c) { return onair_ax25_path_str(p.data(), p.size(), o, c); });
}

std::string Frame::tnc2() const {
    std::vector<OnairAx25AddrIn> p;
    for (const Address& a : path) p.push_back(in(a));
    const OnairAx25AddrIn f = in(from), t = in(to);
    return text([&](uint8_t* o, size_t c) {
        return onair_ax25_tnc2(&f, &t, p.data(), p.size(), reinterpret_cast<const uint8_t*>(info.data()), info.size(), o, c);
    });
}

std::string Frame::typeName() const { return text([&](uint8_t* o, size_t c) { return onair_ax25_type_name(control, o, c); }); }

bool parseFrame(const uint8_t* d, size_t n, Frame& f) {
    f = Frame();
    OnairAx25Frame r;
    if (!onair_ax25_parse_frame(d, n, &r)) return false;
    f.to = fromRs(r.to);
    f.from = fromRs(r.from);
    for (uint32_t i = 0; i < r.npath; i++) f.path.push_back(fromRs(r.path[i]));
    f.control = (uint8_t)r.control;
    f.pid = r.pid;
    f.info.assign((const char*)d + r.info_off, n - r.info_off);
    return true;
}

int hdlcStep(HdlcState& s, int level, bool nrzi, size_t& len) {
    len = 0;
    return onair_ax25_hdlc_bit(reinterpret_cast<OnairAx25Hdlc*>(&s), level, nrzi, &len);
}

void hdlcReset(HdlcState& s) { onair_ax25_hdlc_reset(reinterpret_cast<OnairAx25Hdlc*>(&s)); }

} // namespace ax25
} // namespace dect2
