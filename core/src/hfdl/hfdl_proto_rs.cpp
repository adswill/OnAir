// HFDL bit layers, FCS, station table and PDU parser on top of the Rust crate rust/onair_hfdl: the functions of hfdl_proto.h;
// hfdl_proto.cpp keeps the PSK maps, the builders for the test signal and the ACARS reader.
#include "dect2/hfdl/hfdl_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/hfdl.h"
#include "hfdl_internal.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace dect2 {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[3];
        onair_hfdl_struct_sizes(s);
        dect2::rsAbiCheck("onair_hfdl", s, {sizeof(OnairHfdlRate), sizeof(OnairHfdlMsg), sizeof(OnairHfdlStation)});
    }
} abiCheck;
template <class S> std::string str(const S& s) { return std::string((const char*)s.b, s.len); }
int clampM1(int m1) { return std::max(0, std::min(7, m1)); }
} // namespace

const HfdlRate& hfdlRate(int m1) {
    static HfdlRate rates[8];
    static std::once_flag once;
    std::call_once(once, [] {
        for (int m = 0; m < 8; m++) {
            OnairHfdlRate r;
            onair_hfdl_rate(m, &r);
            rates[m] = {r.m1, r.bps, r.bits_per_sym, r.quarter != 0, r.two_slots != 0, r.blocks, r.col_shift};
        }
    });
    return rates[clampM1(m1)];
}
int hfdlBurstSyms(int m1) { return onair_hfdl_burst_syms(m1); }
int hfdlCodedBits(int m1) { return onair_hfdl_coded_bits(m1); }
int hfdlPayloadOctets(int m1) { return onair_hfdl_payload_octets(m1); }

int hfdlABit(int k) { return onair_hfdl_a_bit(k); }
int hfdlMBit(int shift, int k) { return onair_hfdl_m_bit(shift, k); }

const std::vector<float>& hfdlPreambleRef(int m1) {
    static std::vector<float> ref[8];
    static std::once_flag once;   // built once, thread-safe (the channels of the receiver run in parallel)
    std::call_once(once, [] {
        for (int m = 0; m < 8; m++) {
            int8_t t[kHfdlPreLen];
            onair_hfdl_preamble_ref(m, t, sizeof t);
            for (int i = 0; i < kHfdlPreLen; i++) ref[m].push_back((float)t[i]);
        }
    });
    return ref[clampM1(m1)];
}

void hfdlScramblerSigns(int n, std::vector<int8_t>& out) {
    out.resize((size_t)std::max(0, n));
    onair_hfdl_scrambler_signs(out.data(), out.size());
}

const std::vector<int>& hfdlInterleave(int m1) {
    static std::vector<int> tab[8];
    static std::once_flag once[8];
    m1 = clampM1(m1);
    std::call_once(once[m1], [m1] {
        const size_t N = (size_t)onair_hfdl_coded_bits(m1);
        std::vector<int32_t> t(2 * N);
        onair_hfdl_interleave(m1, t.data(), t.size());
        tab[m1].assign(t.begin(), t.begin() + (long)N);
    });
    return tab[m1];
}

std::vector<uint8_t> hfdlConvEncode(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> o(bits.size() * 2);
    onair_hfdl_conv_encode(bits.data(), bits.size(), o.data(), o.size());
    return o;
}

std::vector<uint8_t> hfdlViterbi(const std::vector<float>& soft, int n) {
    const size_t cnt = (size_t)std::max(0, std::min(n, (int)(soft.size() / 2)));   // negative n gives no bits; a short input is decoded as far as it goes
    std::vector<uint8_t> o((size_t)std::max(0, n), 0);
    std::vector<uint64_t> surv(cnt);
    std::vector<uint8_t> dec(cnt);
    if (onair_hfdl_viterbi(soft.data(), soft.size(), cnt, dec.data(), dec.size(), surv.data(), surv.size())) std::copy(dec.begin(), dec.end(), o.begin());
    return o;
}

uint16_t hfdlFcs(const uint8_t* p, size_t n) { return onair_hfdl_fcs(p, n); }
bool hfdlFcsOk(const uint8_t* p, size_t n) { return onair_hfdl_fcs_ok(p, n); }

const std::vector<HfdlStationInfo>& hfdlStations() {
    static std::vector<HfdlStationInfo> t;
    static std::once_flag once;
    std::call_once(once, [] {
        const size_t n = onair_hfdl_station_count();
        for (size_t i = 0; i < n; i++) {
            OnairHfdlStation s;
            if (!onair_hfdl_station_get(i, &s)) continue;
            t.push_back({s.id, s.name, s.lat, s.lon, std::vector<int>(s.khz, s.khz + s.nkhz)});   // name: static text in the Rust library
        }
    });
    return t;
}
const HfdlStationInfo* hfdlStation(int id) { for (const auto& s : hfdlStations()) if (s.id == id) return &s; return nullptr; }
std::vector<int> hfdlMaskKhz(int gsId, uint32_t mask) {
    int32_t t[20];
    const size_t n = onair_hfdl_mask_khz(gsId, mask, t);
    return std::vector<int>(t, t + n);
}
int hfdlStationOnKhz(int khz) { return onair_hfdl_station_on_khz(khz); }

bool hfdlParsePdu(const uint8_t* p, size_t n, std::vector<HfdlMessage>& out) {
    constexpr size_t kCap = 128;
    std::unique_ptr<OnairHfdlMsg[]> r(new OnairHfdlMsg[kCap]);   // default-initialised: no 400 kB memset per burst
    size_t cnt = 0;
    if (!onair_hfdl_parse_pdu(p, n, r.get(), kCap, &cnt)) return false;
    for (size_t i = 0; i < cnt; i++) {
        const OnairHfdlMsg& c = r[i];
        HfdlMessage m;
        m.squitter = c.squitter != 0; m.uplink = c.uplink != 0; m.gsId = c.gs_id; m.acId = c.ac_id; m.icao = c.icao;
        m.kind = str(c.kind); m.hfnpdu = str(c.hfnpdu); m.summary = str(c.summary); m.detail = str(c.detail);
        m.frameIndex = c.frame_index; m.utcSync = c.utc_sync != 0;
        if (m.squitter && n >= 55) m.systable = (int)p[53] | ((int)(p[54] & 0xF) << 8);   // the SPDU's system table version (octets 53 and 54)
        m.activeKhz.assign(c.active_khz, c.active_khz + std::max(0, std::min(20, (int)c.active_n)));
        m.flight = str(c.flight);
        m.hasPos = c.has_pos != 0; m.lat = c.lat; m.lon = c.lon; m.posKind = str(c.pos_kind); m.utcSec = c.utc_sec;
        if (c.acars && c.acars_at >= 0 && (size_t)c.acars_at <= n) hfdlReadAcars(p + c.acars_at, std::min((size_t)std::max(0, c.acars_len), n - (size_t)c.acars_at), m);
        out.push_back(std::move(m));
    }
    return true;
}

} // namespace dect2
