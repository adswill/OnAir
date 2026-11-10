// DMR test signal: the generator the synthetic source plays and dmrtool writes to a file. Nothing is transmitted.
//
// What it makes: the downlink of a two slot base station (a continuous 4FSK carrier at 4800 symbols/s with CACH, idle bursts, group and private
// voice calls with header, six burst superframes with embedded link control and talker alias, and terminator, control blocks, short data text
// messages) or, in direct mode, a handset that transmits now and then in one time slot. The voice bits are random: AMBE+2 is not part of it.
//
// SynthConfig::modeOpt and modeVal carry this mode's options. Zero always means the default.
//   modeOpt[0]  colour code: 0 = 1 (default), 1..15 = that code, -1 = colour code 0
//   modeOpt[1]  talkgroups in use, 1..8 (0 = 3)
//   modeOpt[2]  link: 0 base station (both slots, continuous), 1 direct mode (bursts of single calls, silence between),
//               2 mobile: a handset talking to a repeater (the uplink: as direct mode, with the mobile station sync patterns)
//   modeOpt[3]  traffic: 0 normal, 1 quiet, 2 busy
//   modeOpt[4]  random seed (0 = 1)
//   modeOpt[5]  bit 0: no text messages, bit 1: no talker alias, bit 2: no sync pattern in the voice headers (embedded signalling instead, which
//               older editions of the standard allowed), bit 3: reverse channel signalling in place of the sync pattern in every second data block of
//               a text message (the current standard allows it in data continuation bursts)
//   modeOpt[6]  hang time: terminator bursts a base station repeats after a call, 0 = 6 (a real repeater sends them for seconds), -1 = none
//   modeVal[0]  deviation as a factor of the nominal 1944 Hz of the outer level (0 = 1.0)
//   modeVal[1]  DC offset of the radio as a fraction of full scale (0 = none; the spike a HackRF puts at the centre)
//   modeVal[2]  IQ gain imbalance in dB (a phase error of 3 degrees per dB goes with it)
//   modeVal[3]  unused
// SynthConfig::snrDb is the signal to noise ratio in a 12.5 kHz channel, cfoHz the carrier offset, sroPpm the clock error of the sample clock.
#pragma once
#include "dmr_proto.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct DmrGenConfig {
    double rate = 2.4e6;
    int cc = 1;
    int talkgroups = 3;
    bool direct = false;         // a handset: single calls in one time slot, silence in between
    bool mobile = false;         // with direct: the sync patterns of a handset transmitting to a repeater (MS sourced) instead of direct mode
    bool encrypted = false;      // the voice calls carry the privacy flag of the service options (the receiver must not decode them)
    int traffic = 0;             // 0 normal, 1 quiet, 2 busy
    uint32_t seed = 1;
    bool textMessages = true, talkerAlias = true;
    bool motoLrrp = false;       // the text messages are a Motorola LRRP position behind a compressed IP/UDP header instead (#335: a real report)
    int hangBursts = 6;          // base station: terminators repeated after a voice call (hang time)
    bool headerSync = true;      // false: the voice headers carry embedded signalling in the centre instead of a sync pattern
    bool xptSite = false;        // base station: Hytera XPT site status blocks (dmr_xpt.h) are sent now and then, and one XPT call set-up per voice call
    bool blockSync = true;       // false: every second data block of a message has reverse channel signalling in the centre instead of a sync pattern
    double snrDb = 40;           // in 12.5 kHz
    double cfoHz = 0;
    double sroPpm = 0;
    double devScale = 1.0;
    double dcOffset = 0;
    double iqImbalanceDb = 0;
    int superMin = 0, superSpan = 0;   // direct mode: length of a voice call in superframes of 360 ms, superMin + 0..superSpan-1 (0: the default 2..5, or 2..7 when busy)
    double rms = 0.22;           // total power of signal plus noise, as an rms of the complex samples

    // ---- real subscriber transmit (dmr_tx.h). When txEnable is set the generator plays exactly ONE MS-sourced transmission built from the user's
    // own identity and payload in time slot txSlot, then the carrier goes off (a real push-to-talk). Nothing random is added; truth() still describes
    // what was sent so the receiver round-trip can be scored. These override the base-station / direct-mode scripts above.
    bool txEnable = false;
    int txCallType = 0;          // 0 group voice, 1 private voice, 2 all-call voice, 3 SMS text, 4 LRRP position, 5 carrier kerchunk, 6 Tier III registration, 7 Tier III voice service request
    uint32_t txSrc = 0;          // the user's own DMR ID (the source of every burst)
    uint32_t txDst = 0;          // talkgroup (group / all call) or destination radio ID (private, SMS, LRRP, Tier III)
    int txSlot = 1;              // 1 or 2
    bool txMobile = true;        // MS-sourced sync patterns (talking to a repeater); false: direct-mode patterns
    std::string txAlias;         // talker alias sent in the embedded signalling of a voice call (from the callsign); empty: none
    std::string txText;          // SMS text (txCallType 3)
    int txTextDd = 0;            // SMS character coding: 0 = auto (ISO 8859-1 if it fits, else UTF-16LE)
    double txLat = 0, txLon = 0; // LRRP position (txCallType 4)
    bool txLrrpNoFix = false;    // LRRP: send a NoFix status instead of a position
    int txVoiceSuper = 10;       // voice: number of 360 ms superframes (10 = 3.6 s); kerchunk uses 1
    double txLeadSec = 0;        // silence before the first slot (the slot sync of dmr_slot_sync.h puts the first burst into the repeater's inbound slot)
};

// What the generator has put on the air (for tests)
struct DmrTruth {
    double startSec = 0, endSec = 0;
    int slot = 0;                // 1 or 2 (direct mode: the time slot used)
    int kind = 0;                // as DmrCall::kind: 0 group voice, 1 private voice, 2 all call, 3 data message, 4 control
    uint32_t src = 0, dst = 0;
    int voiceBursts = 0;
    int cc = 0;
    int csbkOpcode = -1;
    int dataRate = 0;            // data type of the blocks of a message
    std::string alias, text;
    bool finished = false;
};

class DmrSignal {
public:
    explicit DmrSignal(const DmrGenConfig& c);
    ~DmrSignal();
    void generate(cf32* out, size_t n);
    const std::vector<DmrTruth>& truth() const;
    double seconds() const;      // signal time generated so far (symbol time)
    // Insert nothing: silence for `secs` seconds of the *symbol* clock (tests of the carrier going away); the carrier is off, noise continues
    void setCarrier(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

DmrGenConfig dmrGenConfigFrom(const SynthConfig& cfg, double sampleRate);
std::unique_ptr<ModeSynth> makeDmrSynth(const SynthConfig& cfg, double sampleRate);   // nullptr only for a rate below 1 Msps or above 20 Msps
std::unique_ptr<ModeSynth> makeDmrSynthFromConfig(const DmrGenConfig& cfg);           // same generator from a prepared config (the transmit path, dmr_tx.h)

} // namespace dect2
