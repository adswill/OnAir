// The "why isn't it decoding" verdict (voice_why.h) read from each digital voice mode's telemetry.
#pragma once
#include "dect2/dmr/dmr_tel.h"
#include "dect2/dstar/dstar_tel.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/tetra/tetra_tel.h"
#include "dect2/voice/voice_why.h"

namespace dect2 {

inline VoiceWhy dmrWhy(const DmrTelemetry& t) {
    VoiceWhyIn in; in.levelDb = t.levelDbfs;
    for (uint64_t s : t.syncCount) in.syncs += s;
    in.idBad = t.golayFail;                                   // slot types
    in.fecBad = t.bptcFail + t.rsFail + t.trellisFail + t.crcBad;
    in.ok = t.blocksOk;
    return voiceWhy(in);
}
inline VoiceWhy p25Why(const P25Telemetry& t) {
    VoiceWhyIn in; in.levelDb = t.levelDb;
    in.syncs = t.frames; in.idBad = t.nidBad;
    in.fecBad = t.tsbkBad + t.lcBad + t.esBad + t.hduBad;
    in.ok = t.tsbkOk + t.lcOk + t.esOk + t.hduOk + (t.frames > t.nidBad ? t.frames - t.nidBad : 0);
    return voiceWhy(in);
}
inline VoiceWhy nxdnWhy(const NxdnTelemetry& t) {
    VoiceWhyIn in; in.levelDb = t.levelDb;
    in.syncs = t.frames; in.idBad = t.lichBad;
    in.fecBad = t.sacchBad + t.facchBad + t.cacBad;
    in.ok = t.sacchOk + t.facchOk + t.cacOk;
    return voiceWhy(in);
}
inline VoiceWhy dstarWhy(const DstarTelemetry& t) {
    VoiceWhyIn in; in.levelDb = t.levelDb;
    in.syncs = t.headersOk + t.headersBad + t.voiceFrames;
    in.fecBad = t.headersBad + t.blocksBad;
    in.ok = t.headersOk + t.voiceFrames;
    return voiceWhy(in);
}
inline VoiceWhy tetraWhy(const TetraTelemetry& t) {
    VoiceWhyIn in; in.levelDb = t.levelDb;
    for (const auto& c : t.cells) in.syncs += c.bschOk;
    in.fecBad = t.blocksOk ? 0 : t.blocksBad; in.ok = t.blocksOk;   // while nothing decodes the failures are searches that found no cell: not a verdict
    return voiceWhy(in);
}

} // namespace dect2
