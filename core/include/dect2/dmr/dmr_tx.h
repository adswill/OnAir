// DMR subscriber transmit (ETSI TS 102 361-1/-2 Tier II, -4 Tier III subscriber side): OnAir builds a real MS-sourced transmission from the
// operator's own DMR ID and callsign and the chosen payload, so a radio that transmits can talk on a repeater's input or, where the system
// allows it, send a control-channel service request. The bursts carry correct framing, sync, colour code, embedded link control, BPTC/FEC,
// voice LC header and terminator, talker alias, group / private / all calls, SMS short data and LRRP position reports.
//
// Voice bits are a test pattern, NOT speech: AMBE+2 is patented and the user-installed codec plug-in (codec_registry.h) and mbelib expose a
// DECODE path only, so OnAir cannot vocode live speech (it says so in the panel). Everything else of a voice call is exactly on the air.
//
// Transmitting on DMR needs a licence (amateur, or the system operator's permission). OnAir never blocks a frequency or power; the panel warns
// and the user must arm TX. Tier III here is the SUBSCRIBER side only, for a system whose parameters the user entered: a registration or a call
// request on the control channel. OnAir never attempts access to an encrypted system and encrypts nothing without keys the user imported.
#pragma once
#include "dect2/dmr/dmr_gen.h"
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

enum class DmrTxCall {
    GroupVoice = 0,      // talkgroup voice call
    PrivateVoice = 1,    // unit-to-unit voice call
    AllCallVoice = 2,    // all call
    Sms = 3,             // short data text message
    Lrrp = 4,            // LRRP / GPS position report
    Kerchunk = 5,        // a brief keyup (one voice superframe): opens a repeater without a full call
    Tier3Register = 6,   // control channel: registration service request (C_RAND, TS 102 361-4)
    Tier3VoiceRequest = 7 // control channel: voice service request (C_RAND)
};

struct DmrTxConfig {
    double rate = 2.4e6;          // the radio's sample rate
    uint32_t myId = 0;            // the operator's own DMR ID (entered in settings); the source of every burst
    std::string callsign;         // sent as the talker alias of a voice call (empty: none)
    uint32_t dest = 0;            // talkgroup (group voice / SMS / LRRP to a group) or destination radio ID (private / Tier III target)
    int colourCode = 1;           // 0..15
    int slot = 1;                 // the repeater's logical time slot, 1 or 2
    DmrTxCall call = DmrTxCall::GroupVoice;
    bool mobile = true;           // MS sync patterns (a repeater uplink); false: direct-mode / simplex patterns
    std::string text;             // SMS text (UTF-8 on input)
    double lat = 0, lon = 0;      // LRRP position
    bool lrrpNoFix = false;       // LRRP: send a NoFix status instead of a position
    double voiceSeconds = 3.6;    // length of a voice call (rounded to whole 360 ms superframes)
    bool encrypted = false;       // set only the privacy flag of the service options; OnAir never scrambles the payload here
};

// The generator config for one transmission (the inverse mapping is not needed). dest 0 with AllCallVoice becomes 0xFFFFFF.
DmrGenConfig dmrTxGenConfig(const DmrTxConfig& t);
// A ModeSynth that plays exactly one transmission in the chosen slot, then the carrier goes off. nullptr for a rate outside 1..20 Msps.
std::unique_ptr<ModeSynth> makeDmrTxSynth(const DmrTxConfig& t);
// One line for the panel, the log and the documentation (call type, IDs, slot, colour code).
std::string dmrTxSummary(const DmrTxConfig& t);

} // namespace dect2
