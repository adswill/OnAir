// NMEA 0183 position sentences ($--RMC / $--GGA) sent as plain text inside DMR short-data messages:
// some radios send their GPS position this way instead of Motorola LRRP. See dmr_nmea.cpp.
#pragma once
#include "dect2/dmr/dmr_tel.h"
#include <cstddef>

namespace dect2 {

// Parses the first $--RMC or $--GGA sentence in `text` and fills a position in `out`.
// Returns true only for a valid fix (RMC status A, GGA quality > 0) with a non-zero position;
// RMC speed (knots) and course (degrees) are copied when present. A checksum is verified when the
// sentence carries one. Never reads past `size`. The caller sets src/dst/slot/time.
bool parseDmrNmea(const char* text, size_t size, DmrLocation& out);

} // namespace dect2
