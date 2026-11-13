// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM packet mode demultiplexer for the Journaline data service (ES 201 980 clause 6.6.1), see dab_journaline.h.
#include "dect2/dab/dab_journaline.h"
#include "dect2/dab/dab_proto.h"
#include "dect2/common/diag.h"

namespace dect2 {

void DrmPacketDemux::configure(int dataLen, int packetId) {
    if (dataLen != len_ || packetId != id_) { len_ = dataLen; id_ = packetId; reset(); }
}

bool DrmPacketDemux::packet(const uint8_t* p, size_t sz, std::vector<uint8_t>& out) {
    const dabp::DrmPacket k = dabp::drmPacket(p, sz);   // header fields and CRC-16 (dab_proto: Rust)
    if (!k.crcOk) {
        DIAG_DEBUG_EVERY(150274, 10, "A DRM packet mode packet failed its CRC-16, so it is dropped together with the data unit it belonged to.");
        badPackets++;
        if (on_) unitsDropped++;
        on_ = false; asm_.clear();
        return false;
    }
    const bool first = k.first, last = k.last;
    const int id = k.id, ci = k.ci;
    if (id != id_) { otherIdPackets++; return false; }
    packets++;
    const size_t n = k.n, at = k.at;
    if (k.lenBad) { DIAG_DEBUG_EVERY(338311, 10, "A DRM packet mode packet announces a useful length that does not fit the packet, so it is dropped."); badPackets++; packets--; return false; }   // the useful length does not fit: a damaged packet
    // the continuity index counts the packets of one Packet Id modulo 8, empty ones included
    if (lastCi_ >= 0) {
        const int gap = (ci - lastCi_ - 1) & 7;
        if (gap > 0) {
            lostPackets += (uint64_t)gap;
            if (on_) { unitsDropped++; on_ = false; asm_.clear(); }   // a packet was lost: the data unit is gone
        }
    }
    lastCi_ = ci;
    if (n == 0 && first && last) { emptyPackets++; return false; }   // an empty packet
    if (first) { if (on_) unitsDropped++; asm_.clear(); on_ = true; }
    else if (!on_) { orphanPackets++; return false; }
    asm_.insert(asm_.end(), p + at, p + at + n);
    if (asm_.size() > 8215) { DIAG_DEBUG_EVERY(857307, 10, "A DRM Journaline data unit grew beyond %d bytes without ending, so it is dropped.", 8215); on_ = false; asm_.clear(); unitsDropped++; return false; }
    if (last) { out.swap(asm_); asm_.clear(); on_ = false; unitsOk++; return true; }
    return false;
}

} // namespace dect2
