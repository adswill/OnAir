// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Grants from the P25 / DMR control channel model (dect2/p25/trunk.h) into the pool's protocol-neutral GrantEvent.
#include "dect2/nxdn/nxdn_proto.h"
#include "dect2/p25/trunk.h"
#include "dect2/trunkpool/trunkpool.h"

namespace dect2 {
namespace tpool {

GrantEvent grantFromTrunk(const trunk::Grant& t) {
    GrantEvent g;
    g.proto = t.dmr ? Proto::DmrTier3 : (t.tdma ? Proto::P25p2 : Proto::P25p1);
    g.tg = t.tg; g.src = t.src; g.freqHz = t.freqHz; g.channel = t.channel;
    g.slot = t.tdma || t.dmr ? t.slot : -1;
    g.group = t.group; g.emergency = t.emergency; g.encrypted = t.encrypted || (t.algId >= 0 && t.algId != 0x80);
    g.priority = t.priority; g.unplacedType = t.unplacedType;
    return g;
}

GrantEvent grantFromNxdn(const nxdn::Msg& m, const ChannelPlan& plan) {
    GrantEvent g;
    g.proto = Proto::Nxdn;
    g.tg = m.dst; g.src = m.src; g.channel = m.channel;
    g.freqHz = plan.channelHz(m.channel);
    g.group = m.callType != 4;
    g.encrypted = m.cipher != 0;
    return g;
}

} // namespace tpool
} // namespace dect2
