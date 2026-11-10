// DMR subscriber transmit (see dmr_tx.h): translate the operator's request into a DmrGenConfig the generator plays once.
#include "dect2/dmr/dmr_tx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {

DmrGenConfig dmrTxGenConfig(const DmrTxConfig& t) {
    DmrGenConfig c;
    c.rate = t.rate;
    c.cc = std::max(0, std::min(15, t.colourCode));
    c.encrypted = t.encrypted;
    c.txEnable = true;
    c.txSrc = t.myId;
    c.txSlot = (t.slot == 2) ? 2 : 1;
    c.txMobile = t.mobile;
    c.txVoiceSuper = std::max(1, (int)std::lround(t.voiceSeconds / 0.36));
    switch (t.call) {
    case DmrTxCall::GroupVoice:        c.txCallType = 0; c.txDst = t.dest; break;
    case DmrTxCall::PrivateVoice:      c.txCallType = 1; c.txDst = t.dest; break;
    case DmrTxCall::AllCallVoice:      c.txCallType = 2; c.txDst = t.dest ? t.dest : 0xFFFFFF; break;
    case DmrTxCall::Sms:               c.txCallType = 3; c.txDst = t.dest; c.txText = t.text; break;
    case DmrTxCall::Lrrp:              c.txCallType = 4; c.txDst = t.dest; c.txLat = t.lat; c.txLon = t.lon; c.txLrrpNoFix = t.lrrpNoFix; break;
    case DmrTxCall::Kerchunk:          c.txCallType = 5; c.txDst = t.dest; break;
    case DmrTxCall::Tier3Register:     c.txCallType = 6; c.txDst = t.dest; break;
    case DmrTxCall::Tier3VoiceRequest: c.txCallType = 7; c.txDst = t.dest; break;
    }
    // the talker alias of a voice call comes from the callsign
    if (t.call == DmrTxCall::GroupVoice || t.call == DmrTxCall::PrivateVoice || t.call == DmrTxCall::AllCallVoice) {
        std::string a = t.callsign;
        while (!a.empty() && (a.back() == ' ' || a.back() == '\t')) a.pop_back();
        c.txAlias = a;
    }
    return c;
}

std::unique_ptr<ModeSynth> makeDmrTxSynth(const DmrTxConfig& t) {
    return makeDmrSynthFromConfig(dmrTxGenConfig(t));
}

std::string dmrTxSummary(const DmrTxConfig& t) {
    const char* kind = "voice";
    switch (t.call) {
    case DmrTxCall::GroupVoice: kind = "group voice"; break;
    case DmrTxCall::PrivateVoice: kind = "private voice"; break;
    case DmrTxCall::AllCallVoice: kind = "all call"; break;
    case DmrTxCall::Sms: kind = "SMS text"; break;
    case DmrTxCall::Lrrp: kind = t.lrrpNoFix ? "LRRP no-fix" : "LRRP position"; break;
    case DmrTxCall::Kerchunk: kind = "kerchunk"; break;
    case DmrTxCall::Tier3Register: kind = "Tier III registration"; break;
    case DmrTxCall::Tier3VoiceRequest: kind = "Tier III voice request"; break;
    }
    char b[160];
    const uint32_t dst = (t.call == DmrTxCall::AllCallVoice && !t.dest) ? 0xFFFFFF : t.dest;
    snprintf(b, sizeof b, "DMR %s: %u -> %u, slot %d, CC %d%s%s", kind, t.myId, dst, t.slot == 2 ? 2 : 1, std::max(0, std::min(15, t.colourCode)),
             t.callsign.empty() ? "" : (std::string(", alias ") + t.callsign).c_str(), t.mobile ? ", repeater uplink" : ", direct");
    return b;
}

} // namespace dect2
