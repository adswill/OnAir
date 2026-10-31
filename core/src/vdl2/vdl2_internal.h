// Shared by vdl2_proto.cpp and vdl2_proto_rs.cpp only.
#pragma once
#include "dect2/vdl2/vdl2_tel.h"
#include <cstddef>
#include <cstdint>

namespace dect2 {

// Decodes an ACARS block carried in an AVLC information field (p: from the SOH, n octets): the block layer, ADS-C, position text, FANS-1/A CPDLC; sets the
// ACARS fields, position and summary of m and appends to m.detail. Stays C++ (uses the ACARS and aero code).
void vdl2ReadAcars(const uint8_t* p, size_t n, Vdl2Message& m);
void vdl2ReanalyseJoined(Vdl2Message& m);     // rebuild summary, position and detail from the joined text of a multi-block message

} // namespace dect2
