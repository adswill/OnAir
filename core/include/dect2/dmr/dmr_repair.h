// DMR voice header / terminator repair from the radio and group IDs heard on this channel (voice/heard_ids.h): the destination and source of a link
// control word whose Reed-Solomon (12,9) check failed are tried again with heard IDs within two bit flips, and a candidate is kept only when the word
// then checks out clean. Unique or nothing: an ID cannot be made up.
#pragma once
#include "dmr_fec.h"
#include "dect2/voice/heard_ids.h"

namespace dect2 {

// info96: the 96 bits after the BPTC decoder (LC + masked RS parity). fixed receives the repaired bits.
bool lcRepairHeard(const dmr::Bits& info96, bool terminator, const voice::HeardIds& ids, dmr::Bits& fixed);

} // namespace dect2
