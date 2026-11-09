// D-STAR header repair from the callsigns heard on this channel (voice/heard_ids.h). A header whose CRC fails is tried again with each callsign
// field replaced by a heard callsign that differs in at most two characters; it is accepted only when the CRC then passes, so a wrong callsign
// cannot be invented. D-STAR has no other header repair than the Viterbi decoder.
#pragma once
#include "dstar_proto.h"
#include "dect2/voice/heard_ids.h"

namespace dect2::dstar {

// raw: the 41 header bytes as received (CRC wrong). On success fills out (repaired, CRC valid) and the number of characters changed.
bool headerRepair(const uint8_t raw[kHeaderBytes], const voice::HeardIds& heard, Header& out, int* subs = nullptr);
// the 8 / 4 character raw fields of a header, for feeding the dictionary
void headerNoteHeard(const Header& h, voice::HeardIds& heard);

} // namespace dect2::dstar
