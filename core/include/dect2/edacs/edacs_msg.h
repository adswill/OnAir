// EDACS control channel message layer: what the two 28-bit words of a frame mean, Standard / Networked and Extended Addressing, and the same words built for the
// test signal.
//
// Sources: the Standard / Networked layout follows TIA/EIA TSB-69.3 (EDACS digital air interface, 1998) as implemented in DSD-FME's edacs-fme.c (GPL-3, rewritten
// "to spec" by ilyacodes, credited in THIRD_PARTY.md); the Extended Addressing layout follows US 7,546,135 B2 and the reverse-engineered messages in the same file.
// No recording of a real system was available: the BCH check bits are verified against an independent encoder, the field positions are not (they are the
// published ones, nothing is guessed). Command word = the first message of the frame; the second message carries the rest of the fields of the commands that
// use two words (the calling unit ID of a group call, the source of an individual call, the port of a data call).
// ESK: some systems XOR a fixed byte into the top bits of every word (0xA0, "ESK"); it is a setting.
// ProVoice (the manufacturer's digital voice) goes to the user-installed IMBE plug-in (edacs/provoice.h).
#pragma once
#include <cstdint>
#include <string>

namespace dect2::edacs {

struct Layout {
    bool extended = false;      // Extended Addressing (EA) instead of Standard / Networked
    int esk = 0;                // 0 or 0xA0: XORed into bits 20 - 27 of both words before they are read
};

enum class MsgType {
    Unknown, GroupAssign, IndividualAssign, DataAssign, InterconnectAssign, SiteId, AdjacentSite, CallEnd, LoginAck, StatusRequest, AllCall, SystemAssignedId, Other
};

struct Msg {
    MsgType type = MsgType::Unknown;
    int lcn = -1, gid = -1, lid = -1, source = -1, site = -1, area = -1, priority = -1, port = -1, status = -1, index = -1;
    bool emergency = false, digital = false, update = false, failsoft = false, scat = false, aux = false, txTrunk = false, individual = false;
    uint32_t raw = 0;
    const char* other = "";     // Other: the name of the command that is known but is not acted on
};

Layout defaultLayout();
Msg parse(uint32_t word1, uint32_t word2, const Layout& l);            // word2 only supplies the second part of the two-word commands
void build(const Msg& m, const Layout& l, uint32_t& word1, uint32_t& word2);   // the test signal; word2 is 0 for the one-word commands
const char* typeName(MsgType t);
std::string describe(const Msg& m);                                    // one line of text

} // namespace dect2::edacs
