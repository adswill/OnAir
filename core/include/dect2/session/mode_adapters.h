// Adapters from a mode's telemetry to Sessions (docs/MODE-EXPERIENCE.md). Each is called by the receiver when the telemetry is read, with the telemetry
// it just built; it only pushes to the session store through a TrackBook (never SQLite, never a file). Message text is not stored here.
//   ADS-B (family 11)  -> Track per aircraft (ICAO), gap 2 min
//   AIS   (family 14)  -> Track per vessel (MMSI), gap 10 min
//   APRS  (family 24)  -> Track per station, Thread per message pair, gap 10 min
//   Pager (family 23)  -> Thread per POCSAG RIC / FLEX capcode, 10 min window (ids are hashed by the store: privacy class Personal)
#pragma once
#include <cstdint>

namespace dect2 {
struct AdsbTelemetry; struct AisTelemetry; struct PacketTelemetry; struct PagerTelemetry;
namespace sess {

constexpr uint16_t kModeAdsb = 11, kModeAis = 14, kModePager = 23, kModeAprs = 24, kModeDmr = 9;

void observeAdsb(const AdsbTelemetry& t, uint16_t receiver = 0);
void observeAis(const AisTelemetry& t, uint16_t receiver = 0);
void observePacket(const PacketTelemetry& t, uint16_t receiver = 0);
void observePager(const PagerTelemetry& t, uint16_t receiver = 0);
// the mode was switched or the receiver closed: end its open sessions
void adaptersEnd(uint16_t mode, uint16_t receiver = 0);

}  // namespace sess
}  // namespace dect2
