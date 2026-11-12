// 4.0 group A - D features in the mode screens (docs/WIRING-4.0.md): the pure engines of core/include/dect2/d40 and friends, fed from the
// live engine data. d40Tick() runs every frame (from the Insights tick); each d40...Card() draws one compact card in its mode's screen.
// Settings persist through plat::prefs() (key prefix "d40"), tables as text files in plat::dataDir(). Offline; nothing leaves the computer.
#pragma once
#include "../app.h"
#include <functional>
#include <string>

void d40Tick(App& a);
void d40FmCard(App& a);        // d40_fm_ui.cpp: station names list and DX opening alarm
void d40FmTick(App& a);
void d40SatTick(App& a);
void d40SatCard(App& a);       // d40_sat_ui.cpp: blind scan list, modcod fit, dish pointing readout
namespace dect2::sweep { struct Sweep; struct PersistentSignal; }
void d40ScanFeed(const dect2::sweep::Sweep& s);                         // d40_scan_ui.cpp: from the sweep thread after each sweep
void d40ScanCard(const std::vector<dect2::sweep::PersistentSignal>& persist);   // under the Band survey in the sweep scanner
void d40AmCard(App& a);        // d40_am_ui.cpp: carrier offset, best sideband, AM or DRM hint, station ID from the user's schedule
void d40DabTick(App& a);
void d40DabCard(App& a);       // d40_dab_ui.cpp: service ranking, transmitter table from a user CSV
void d40AdsbFeed(const AdsbTelemetry& t);   // d40_adsb_ui.cpp: every position fix into the replay
void d40AdsbReplayView(App& a, double refLat, double refLon, const std::function<std::string(uint32_t)>& nameOf);   // the Replay view
void d40P25Activity(const P25Telemetry& t);      // d40_tg_ui.cpp: talkgroup airtime ranking, timeline, busiest talkgroup
void d40NxdnActivity(const NxdnTelemetry& t);
void d40TetraTick(const TetraTelemetry& t);          // builds TETRA calls from the slot states (talkgroups only, never terminal numbers)
void d40TetraActivity();
void d40AcarsFeed(const dect2::AcarsTelemetry& t);   // d40_acars_ui.cpp: every new message into the channel load
void d40AcarsLoadCard();
void d40AisFeed(const AisTelemetry& t, double lat, double lon);   // d40_ais_ui.cpp: every position into the coverage map
void d40AisRangeCard(const AisTelemetry& t);
void d40PagerFeed(const PagerTelemetry& t);   // d40_pager_ui.cpp: paged capcodes into the group finder
void d40PagerGroupsCard();
void d40SarsatFeed(const dect2::SarsatTelemetry& t);   // d40_sarsat_ui.cpp: beacons grouped into incidents
void d40SarsatIncidentsCard();
void d40TonesFeed(const dect2::TonesTelemetry& t);   // d40_tones_ui.cpp: DTMF number assembler, tone system guess
void d40TonesCard(const dect2::TonesTelemetry& t);
namespace dect2 { struct IsmRaw; }
void d40IsmClassifyLine(const dect2::IsmRaw& r);   // d40_ism_ui.cpp: under the pulse trace of an unknown burst
namespace dect2 { struct HfdigFtxTelemetry; }
void d40FtxCard(App& a, const dect2::HfdigFtxTelemetry& f);   // d40_ftx_ui.cpp: heard stations, band summary, clear TX frequency, band suggestion
namespace dect2 { struct CwTelemetry; }
void d40CwCard(App& a, const dect2::CwTelemetry& t);   // d40_cw_ui.cpp: callsigns heard and CQ callers
std::string d40GsmrFrequencies(const std::vector<int>& arfcns);   // d40_gsmr_ui.cpp: "955: 921.2 / 876.2 (R)" per channel number: downlink / uplink MHz, R = railway band
namespace dect2 { struct HfbTelemetry; }
void d40HfbFeed(const std::vector<double>& khz, const dect2::HfbTelemetry& t);   // d40_hf_ui.cpp: beacon readings into the hour-of-day table
void d40HfbBestCard();
void d40AirbandCard();   // d40_airband_ui.cpp: ATIS letter tracker and VOLMET airports from the live transcripts
