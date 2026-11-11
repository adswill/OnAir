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
