// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// fec_proto.h on top of the Rust crate rust/onair_fec: thin wrappers over the C ABI of core/include/dect2/rs/fec.h.
#include "dect2/fec/fec_proto.h"
#include "dect2/rs/fec.h"
#include <cstdio>
#include <cstdlib>

namespace dect2::fecp {

void rs204Encode(const uint8_t* in, uint8_t* out) { onair_fec_rs_encode(0, in, 188, out, 204); }
int rs204Decode(uint8_t* r) { return onair_fec_rs_decode(0, r, 204); }
void rs207Encode(const uint8_t* in, uint8_t* out) { onair_fec_rs_encode(1, in, 187, out, 207); }
int rs207Decode(uint8_t* r) { return onair_fec_rs_decode(1, r, 207); }
void rs120Encode(const uint8_t* in, uint8_t* out) { onair_fec_rs_encode(2, in, 110, out, 120); }
int rs120Decode(uint8_t* r) { return onair_fec_rs_decode(2, r, 120); }

// the field tables, generator and remainder table live in the Rust context
Bch::Bch(bool shortFrame, int t) : deg_((shortFrame ? 14 : 16) * t) {
    rsctx_.assign(onair_fec_bch_ctx_size() / 8 + 1, 0);
    if (!onair_fec_bch_init((uint8_t*)rsctx_.data(), rsctx_.size() * 8, shortFrame, t)) { fprintf(stderr, "onair_fec: BCH t = %d is not supported\n", t); abort(); }
}

void Bch::encode(std::vector<uint8_t>& bits, int k) const {
    const size_t kk = k < 0 ? 0 : (size_t)k > bits.size() ? bits.size() : (size_t)k;
    bits.resize(kk + (size_t)deg_);
    onair_fec_bch_encode((const uint8_t*)rsctx_.data(), bits.data(), bits.size(), kk);
}

int Bch::decode(std::vector<uint8_t>& bits) const { return onair_fec_bch_decode((const uint8_t*)rsctx_.data(), bits.data(), bits.size()); }

bool ldpcBuild(int k, int n, const std::vector<std::vector<int>>& rows, LdpcTables& t) {
    t = LdpcTables();
    if (k < 0 || n < 0) return false;
    std::vector<uint32_t> len;
    std::vector<int32_t> addrs;
    len.reserve(rows.size());
    for (const auto& r : rows) { len.push_back((uint32_t)r.size()); addrs.insert(addrs.end(), r.begin(), r.end()); }
    size_t d[5];
    if (!onair_fec_ldpc_dims((size_t)k, (size_t)n, addrs.size(), d)) return false;
    t.m = (int)d[0]; t.q = (int)d[1];
    t.chkStart.resize(d[0] + 1); t.chkVar.resize(d[4]); t.encP.resize(d[3]); t.encD.resize(d[3]);
    t.layerStart.resize(d[1] + 1); t.layerGroup.resize(addrs.size()); t.layerShift.resize(addrs.size());
    OnairFecLdpcOut o = {t.chkStart.data(), t.chkStart.size(), t.chkVar.data(), t.chkVar.size(), t.encP.data(), t.encP.size(), t.encD.data(), t.encD.size(),
                         t.layerStart.data(), t.layerStart.size(), t.layerGroup.data(), t.layerGroup.size(), t.layerShift.data(), t.layerShift.size()};
    if (!onair_fec_ldpc_build((size_t)k, (size_t)n, len.data(), len.size(), addrs.data(), addrs.size(), &o)) { t = LdpcTables(); return false; }
    return true;
}

#if defined(ONAIR_RUST_VITERBI) && defined(__aarch64__)
// the NEON kernel in Rust, opt-in (cmake -DDECT2_RUST_VITERBI=ON): it measures ~4% slower than the C++ NEON one, so by default (and on x86: SSE2 / AVX2 / portable)
// fec_proto.cpp keeps the C++ viterbiRange
void viterbiRange(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    static thread_local std::vector<uint64_t> dec;   // one 64 bit decision word per step
    dec.resize(steps);
    bits.assign(steps, 0);
    int64_t m = 0;
    onair_fec_viterbi_range(soft, steps, bits.data(), dec.data(), &m);
    if (metricOut) *metricOut = (long)m;
}
#endif

} // namespace dect2::fecp
