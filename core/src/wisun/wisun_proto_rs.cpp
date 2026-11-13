// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-SUN FAN / IEEE 802.15.4g frame layers on top of the Rust crate rust/onair_wisun: the functions of wisun_proto.h;
// wisun_proto.cpp keeps the channel plan tables, the MPDU builder and the text helpers that call the 2.4 GHz mode.
#include "dect2/wisun/wisun_proto.h"
#include <memory>
#include "dect2/common/rs_abi.h"
#include "dect2/rs/wisun.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace wisun {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[3];
        onair_wisun_struct_sizes(s);
        dect2::rsAbiCheck("onair_wisun", s, {sizeof(OnairWisunFrame), sizeof(OnairWisunSchedule), sizeof(OnairWisunPhr)});
    }
} abiCheck;

template <class S> std::string str(const S& s) { return std::string((const char*)s.b, s.len); }

OnairWisunSchedule toC(const Schedule& s) {
    OnairWisunSchedule c;
    c.present = s.present; c.bcast_interval_ms = s.bcastIntervalMs; c.bsi = s.bsi; c.dwell_ms = s.dwellMs; c.clock_drift = s.clockDrift;
    c.timing_accuracy = s.timingAccuracy; c.ch_plan = s.chPlan; c.ch_function = s.chFunction; c.excluded = s.excluded; c.reg_domain = s.regDomain;
    c.op_class = s.opClass; c.ch_plan_id = s.chPlanId; c.ch0_mhz = s.ch0Mhz; c.spacing_code = s.spacingCode; c.num_channels = s.numChannels;
    c.fixed_channel = s.fixedChannel; c.excluded_count = s.excludedCount;
    return c;
}
void fromC(const OnairWisunSchedule& c, Schedule& s) {
    s.present = c.present != 0; s.bcastIntervalMs = c.bcast_interval_ms; s.bsi = c.bsi; s.dwellMs = c.dwell_ms; s.clockDrift = c.clock_drift;
    s.timingAccuracy = c.timing_accuracy; s.chPlan = c.ch_plan; s.chFunction = c.ch_function; s.excluded = c.excluded; s.regDomain = c.reg_domain;
    s.opClass = c.op_class; s.chPlanId = c.ch_plan_id; s.ch0Mhz = c.ch0_mhz; s.spacingCode = c.spacing_code; s.numChannels = c.num_channels;
    s.fixedChannel = c.fixed_channel; s.excludedCount = c.excluded_count;
}
} // namespace

Phr parsePhr(uint16_t v) {
    OnairWisunPhr c;
    onair_wisun_parse_phr(v, &c);
    Phr p;
    p.modeSwitch = c.mode_switch != 0; p.fcs2 = c.fcs2 != 0; p.whitened = c.whitened != 0; p.length = c.length;
    return p;
}

uint16_t buildPhr(const Phr& p) {
    OnairWisunPhr c;
    c.mode_switch = p.modeSwitch; c.fcs2 = p.fcs2; c.whitened = p.whitened; c.length = p.length;
    return onair_wisun_build_phr(&c);
}

void whiten(uint8_t* d, size_t n) { onair_wisun_whiten(d, n); }
uint32_t crc32(const uint8_t* d, size_t n) { return onair_wisun_crc32(d, n); }
uint16_t crc16(const uint8_t* d, size_t n) { return onair_wisun_crc16(d, n); }
bool fcsOk(const uint8_t* p, size_t n, bool fcs2) { return onair_wisun_fcs_ok(p, n, fcs2); }

const char* regDomainName(int c) { return onair_wisun_reg_domain_name(c); }
const char* uttName(int t) { return onair_wisun_utt_name(t); }
const char* uttShort(int t) { return onair_wisun_utt_short(t); }
const char* chFunctionName(int f) { return onair_wisun_ch_function_name(f); }

std::string scheduleText(const Schedule& s) {
    const OnairWisunSchedule c = toC(s);
    uint8_t b[512];
    const size_t n = onair_wisun_schedule_text(&c, b, sizeof b);
    return n == (size_t)-1 ? std::string() : std::string((const char*)b, n);
}

Frame parse(const uint8_t* mpdu, size_t n) {
    auto c = std::make_unique<OnairWisunFrame>();
    onair_wisun_parse(mpdu, n, c.get());
    Frame f;
    f.ok = c->ok != 0; f.type = c->kind; f.version = c->version;
    f.secured = c->secured != 0; f.pending = c->pending != 0; f.ackReq = c->ack_req != 0; f.panCompress = c->pan_compress != 0;
    f.seqSuppressed = c->seq_suppressed != 0; f.ieList = c->ie_list != 0;
    f.destMode = c->dest_mode; f.srcMode = c->src_mode; f.seq = c->seq;
    f.hasDestPan = c->has_dest_pan != 0; f.hasSrcPan = c->has_src_pan != 0;
    f.destPan = c->dest_pan; f.srcPan = c->src_pan; f.destShort = c->dest_short; f.srcShort = c->src_short; f.destExt = c->dest_ext; f.srcExt = c->src_ext;
    f.secLevel = c->sec_level; f.keyIdMode = c->key_id_mode; f.keyIndex = c->key_index; f.frameCounter = c->frame_counter;
    f.utt = c->utt; f.ufsi = c->ufsi; f.hasBt = c->has_bt != 0; f.bsn = c->bsn; f.bio = c->bio; f.rslDbm = c->rsl_dbm;
    f.hasFc = c->has_fc != 0; f.txFlow = c->tx_flow; f.rxFlow = c->rx_flow; f.eaEui = c->ea_eui;
    f.hasLbc = c->has_lbc != 0; f.lfnBcastMs = c->lfn_bcast_ms;
    fromC(c->us, f.us); fromC(c->bs, f.bs);
    f.hasPanIe = c->has_pan_ie != 0; f.panSize = c->pan_size; f.routingCost = c->routing_cost; f.tpsVersion = c->tps_version;
    f.useParentBs = c->use_parent_bs != 0; f.rplRouting = c->rpl_routing != 0;
    f.netName = str(c->net_name); f.hasNetName = c->has_net_name != 0;
    f.panVersion = c->pan_version; f.hasGtkHash = c->has_gtk_hash != 0; f.lfnVersion = c->lfn_version;
    for (int i = 0; i < c->phy_mode_count && i < 2048; i++) f.phyModes.push_back(c->phy_modes[i]);
    f.mpxId = c->mpx_id; f.kmpId = c->kmp_id; f.eapolType = c->eapol_type; f.eapCode = c->eap_code; f.eapType = c->eap_type;
    f.ies = str(c->ies);
    f.payloadAt = (size_t)c->payload_at; f.payloadLen = (size_t)c->payload_len;
    f.encrypted = c->encrypted != 0;
    f.info = str(c->info);
    return f;
}

} // namespace wisun
} // namespace dect2
