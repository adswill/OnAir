// AIS messages beyond positions and static data, turned into readable records: the binary application messages 6 and 8 (IMO SN.1/Circ.236 and 289,
// CCNR Inland AIS, IALA A-126) and message types 7, 10, 12, 13, 14, 15, 16, 17, 20, 22, 23, 25, 26 of ITU-R M.1371-5.
// Field layouts are from the gpsd AIVDM document and driver_ais.c.
#pragma once
#include "ais_proto.h"
#include "ais_tel.h"

namespace dect2 {
namespace ais {

// Fills 'r' (title, summary, fields, position, shapes). False when nothing could be read: not one of these types, or an application message
// whose length does not fit its layout. Unknown DAC / FI pairs return true with a title naming them, so they stay visible.
bool interpretApp(const Bits& payload, const AisMsg& m, AisAppRecord& r);

// a position 'distM' metres away on 'bearingDeg' (sphere, good for the few tens of kilometres of an area notice)
void aisOffset(double lat, double lon, double bearingDeg, double distM, double& outLat, double& outLon);

} // namespace ais
} // namespace dect2
