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
struct MeshTelemetry; struct IridiumTelemetry; struct OrbcommTelemetry; struct AeroTelemetry; struct InmcTelemetry;
struct AcarsTelemetry; struct Vdl2Telemetry; struct HfdlTelemetry; struct UatTelemetry; struct SondeTelemetry;
namespace sess {

constexpr uint16_t kModeAdsb = 11, kModeAis = 14, kModePager = 23, kModeAprs = 24, kModeDmr = 9;
constexpr uint16_t kModeInmc = 17, kModeAero = 18, kModeIridium = 19, kModeMesh = 20, kModeOrbcomm = 44;

void observeAdsb(const AdsbTelemetry& t, uint16_t receiver = 0);
void observeAis(const AisTelemetry& t, uint16_t receiver = 0);
void observePacket(const PacketTelemetry& t, uint16_t receiver = 0);
void observePager(const PagerTelemetry& t, uint16_t receiver = 0);
// group 2 (mode_adapters_air.cpp): aircraft that talk and radiosondes, one Track each
void observeAcars(const AcarsTelemetry& t, uint16_t receiver = 0);   // family 16
void observeVdl2(const Vdl2Telemetry& t, uint16_t receiver = 0);     // 27
void observeHfdl(const HfdlTelemetry& t, uint16_t receiver = 0);     // 35
void observeUat(const UatTelemetry& t, uint16_t receiver = 0);       // 83
void observeSonde(const SondeTelemetry& t, uint16_t receiver = 0);   // 13
void adaptersEndAir(uint16_t mode, uint16_t receiver = 0);
// group 3 (mode_adapters_data.cpp): LoRa mesh / LoRaWAN / LoRa APRS (family 20), Iridium (19), Orbcomm (44), Inmarsat Aero (18), Inmarsat-C (17)
void observeMesh(const MeshTelemetry& t, uint16_t receiver = 0);
void observeIridium(const IridiumTelemetry& t, uint16_t receiver = 0);
void observeOrbcomm(const OrbcommTelemetry& t, uint16_t receiver = 0);
void observeAero(const AeroTelemetry& t, uint16_t receiver = 0);
void observeInmc(const InmcTelemetry& t, uint16_t receiver = 0);
void adaptersEndData(uint16_t mode, uint16_t receiver = 0);
// every adapter of every mode: the program is closing, open tracks end now
void adaptersEndAll();
// the mode was switched or the receiver closed: end its open sessions
void adaptersEnd(uint16_t mode, uint16_t receiver = 0);

}  // namespace sess
}  // namespace dect2
