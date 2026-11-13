// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Ham station (4.0): what the Ham screen needs around the operating panels, as pure logic.
//  - the mode catalog: every amateur mode OnAir decodes, in one list, with where it runs (the slot engine of the Ham screen, the keyboard panel of
//    the Ham screen, or its own receiver screen), its ADIF mode / submode and the report style (dB, RST 599, RS 59);
//  - the frequency memory: the last frequency per band and mode, so one click on a band brings back where you were;
//  - the usual dial frequencies of the modes the slot table (ham_ops.h bandDials) does not cover;
//  - the simplified IARU band plan segments (CW, narrow digital, all modes, beacons, satellites, FM) for a warning: never a refusal;
//  - the S-meter scale (S9 = -73 dBm below 30 MHz, -93 dBm above, 6 dB per S unit), the setup checklist, the region of a country,
//    CW speed matching, the band-hop schedule and the WSJT-X style "fake it" split.
// Nothing here transmits or touches the network.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace dect2::ham {

// ---- the mode catalog ----
enum class HamRun { Slot, Keyboard, Screen };   // the Ham slot engine (ham.h), the Ham keyboard panel (ham_kbd.h), the mode's own screen
struct HamModeInfo {
    const char* id;            // "ft8", "psk31", "js8"
    const char* name;          // "FT8", "PSK31", "JS8Call" (as shown)
    const char* group;         // "Weak signal", "Keyboard", "Image", "Voice", "Packet"
    HamRun run;
    int slotMode;              // HamRun::Slot: the hfdig mode number (ham.h); -1 otherwise
    const char* kbdId;         // HamRun::Keyboard: the kbdPresets() id; nullptr otherwise
    const char* screen;        // HamRun::Screen: the mode-list name of the receiver ("JS8Call", "SSTV", "D-STAR"); nullptr otherwise
    const char* adifMode;      // ADIF MODE ("FT8", "MFSK" for FT4 / JS8 / Q65 ..., "PSK", "SSB")
    const char* adifSub;       // ADIF SUBMODE ("FT4", "JS8", "PSK31"), "" none
    const char* rst;           // the report style: "dB" (the decoder's SNR), "RST" (599), "RS" (59)
    bool tx;                   // the Ham screen can transmit it (behind the same Arm and warnings)
};
const std::vector<HamModeInfo>& hamModeCatalog();
const HamModeInfo* hamModeInfo(const std::string& id);          // by id, case-insensitive; nullptr when unknown
const HamModeInfo* hamModeBySlot(int slotMode);
const HamModeInfo* hamModeByKbd(const std::string& kbdId);
std::vector<std::string> hamModeGroups();                        // the groups in catalog order, each once
// The report to fill in before anything is heard: "599" (RST), "59" (RS), "-10" (dB)
std::string defaultRst(const HamModeInfo& m);

// ---- usual dial frequencies (MHz, the radio's USB dial; 0: no usual frequency, keep the current one) ----
// The slot modes use ham_ops.h dialFor (WSJT-X defaults); this adds the keyboard, image and voice calling frequencies that are in common use
// (they differ by region: region 1, 2 or 3, 0 = region 1 values). These are customs, not rules: the frequency memory overrides them.
double usualDialMhz(const std::string& band, const std::string& modeId, int region);

// ---- the frequency memory ----
class FreqMemory {
public:
    void remember(const std::string& band, const std::string& modeId, double hz);
    // the remembered frequency, else `fallbackHz`
    double recall(const std::string& band, const std::string& modeId, double fallbackHz) const;
    bool has(const std::string& band, const std::string& modeId) const;
    void forget(const std::string& band, const std::string& modeId);
    size_t size() const { return m_.size(); }
    std::string toText() const;               // "20m|ft8=14074000;40m|cw=7023500" (stored in the settings)
    void fromText(const std::string& s);      // what does not read is skipped
private:
    std::map<std::string, double> m_;         // "band|mode" -> Hz
};

// ---- band plan segments (warning only) ----
enum class SegUse { Cw, Narrow, All, Beacon, Satellite, Fm };   // Narrow: CW and narrow digital modes; All: every mode (phone, digital, image)
struct BandSegment { double loHz, hiHz; SegUse use; const char* label; };
// A simplified IARU band plan (region 1, 2 or 3; 0 = region 1), HF to 70 cm. National plans can be narrower or differ: this is a hint.
const std::vector<BandSegment>& bandSegments(int region);
const BandSegment* segmentAt(int region, double hz);
const char* segUseName(SegUse u);
// "" when the mode class ("CW", "DIGI", "PHONE": ham_log.h modeClass) fits the segment at hz, else one plain sentence. The caller still sends.
std::string segmentWarning(int region, double hz, const std::string& modeClass);

// ---- the S-meter ----
double sUnits(double dbm, double hz);          // S units (fractional): S9 = -73 dBm below 30 MHz, -93 dBm from 30 MHz; 6 dB per unit
std::string sMeterText(double dbm, double hz); // "S0" .. "S9", "S9+10", "S9+40"

// ---- the region of a country (ISO 3166 code or band plan id), from the built-in band plans; 0 when not known ----
int regionForCountry(const std::string& country);

// ---- the setup checklist: what is missing or looks wrong (never a lock; the Ham screen shows them as hints) ----
struct StationSetup {
    std::string call, grid, licClass, country, power;
    std::vector<std::string> antennas;
    int region = 0;
    bool licence = false;      // the operator confirmed a licence
    bool rig = false;          // a CAT rig, VOX or an SDR transmitter is chosen
    bool audioOut = false;     // an audio output is chosen (or the system default is accepted)
};
std::vector<std::string> setupHints(const StationSetup& s);

// ---- CW speed matching: follows the other station's speed (the CW decoder's WPM readings) ----
class CwSpeedMatch {
public:
    int minWpm = 5, maxWpm = 45;
    void heard(double wpm);                    // one reading; readings far from the running value (under 0.5x or over 2x after 3 readings) are outliers
    void reset() { n_ = 0; est_ = 0; }
    bool ready() const { return n_ >= 2; }
    int wpm(int currentWpm) const;             // the keyer speed to use: the match rounded and clamped, or currentWpm when not ready
private:
    double est_ = 0;
    int n_ = 0;
};

// ---- the band-hop schedule (UTC): lines "0000-0600 40m ft8", "0600-1800 20m", "1800-2400 auto" ----
struct HopEntry { int startMin = 0, endMin = 0; std::string band, modeId; };   // minutes of the UTC day; end < start wraps over midnight
struct HopSchedule {
    std::vector<HopEntry> entries;
    // the entry covering utc (the first in the list), nullptr when none. band "auto": let the propagation hopper (ham_bandhop.h) choose.
    const HopEntry* at(double utc) const;
    static bool parse(const std::string& text, HopSchedule& out, std::string& err);
    std::string toText() const;
};

// ---- split: keep the transmitted audio in the clean part of the passband (WSJT-X "fake it"), 500 Hz steps ----
struct SplitTx { double dialShiftHz = 0, audioHz = 0; };
SplitTx fakeItSplit(double txAudioHz, double lo = 1500, double hi = 2000);

}  // namespace dect2::ham
