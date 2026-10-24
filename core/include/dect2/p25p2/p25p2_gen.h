// P25 Phase 2 test signal: a base station downlink of two logical channels made of the bursts of p25p2_proto.h, differential QPSK through a root
// raised cosine, with a carrier offset and noise. The voice frames are a known pattern (p25p2VoiceBits): there is no free AMBE+2 encoder.
// The superframe (which slots carry the sync word, the I-ISCH, the order of 4V / 2V / SACCH bursts of each channel), the voice and ESS places and
// the scrambling follow the on-air layout of p25p2_proto.h; the MAC PDUs in the SACCH / FACCH bursts are the program's own layout.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {

struct P25p2GenCall {
    int channel = 1;               // logical channel (time slot) 1 or 2
    int startSf = 1;               // superframe in which the PTT is sent
    int nSf = 6;                   // superframes the call lasts (the END_PTT is in the last)
    int src = 1234567, group = 1001;
    int algId = 0x80;              // 0x80 clear; anything else: reported as encrypted, its voice is never decoded
    bool unitToUnit = false;       // group stays the target unit
    int svcOpts = 0;               // 0x80 emergency, 0x40 encrypted in the service options of the voice channel user message
};

struct P25p2GenConfig {
    double rate = 240000;          // sample rate of the signal
    double snrDb = 40;             // in a 6 kHz bandwidth (the symbol rate)
    double cfoHz = 0;              // carrier offset
    int superframes = 12;
    int leadDibits = 13;           // random dibits in front of the first sync (not a whole slot)
    int colorCode = 0x293;         // the NAC: sent in the END_PTT, and a part of the scrambling seed
    bool scramble = false;         // scramble the bursts with the seed WACN / system / colour code (the receiver needs setSystem with the same)
    int wacn = 0xBEE00, sysId = 0x3A5;
    uint32_t seed = 7;
    std::vector<P25p2GenCall> calls;
    std::vector<std::vector<uint8_t>> sysMsgs;   // MAC messages sent round robin in the signalling bursts of idle channels (and of both channels before a call)
};

// The 72 voice bits of the index-th voice frame of a logical channel (a pattern known to the tests)
void p25p2VoiceBits(int channel, int index, uint8_t bits72[72]);
// The dibits of the whole signal, starting with the lead-in
std::vector<uint8_t> p25p2Dibits(const P25p2GenConfig& c);
// Samples of those dibits
std::vector<cf32> p25p2Modulate(const std::vector<uint8_t>& dibits, const P25p2GenConfig& c);
inline std::vector<cf32> p25p2Signal(const P25p2GenConfig& c) { return p25p2Modulate(p25p2Dibits(c), c); }
// How many voice frames a call put on the air (clear or not)
int p25p2VoiceFrames(const P25p2GenConfig& c, int channel);

} // namespace dect2
