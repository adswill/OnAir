// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The parameters of every transmit mode, declared in one place (docs/TX-4.0.md). Each row says what the control is called, its type, unit, range,
// default, whether it is in the Simple view or under Advanced (and in which group), which sub-mode it applies to, a one-line help, and where its value
// goes in the TxRequest. The transmit panel is drawn from this table, so every control it shows reaches the generator; tests/tx/test_tx_param_model.cpp
// checks that every row changes the generated signal (or is marked as not doing so, with the reason) and that no generator reads a key that is not here.
// Nothing here limits a value to what is lawful: a range is what the generator can make. A row's `block` rule only keeps out combinations
// a standard forbids outright for the signal it makes (DVB-T2-Lite).
#pragma once
#include "dect2/tx/tx.h"
#include <map>
#include <string>
#include <vector>

namespace dect2 {

enum class TxPGroup { Payload, Audio, Modulation, Filters, Framing, Timing, Output };
enum class TxPType { Float, Int, Bool, Choice, Text, File, Hex };
enum class TxPLevel { Simple, Advanced };

// A request field a row writes (the numbers and texts of TxRequest that are not keys of TxRequest::opt). None: the row writes opt[key], or uses `set`.
enum class TxPField {
    None, Callsign, Grid, Text, Dest, Path, File, Name, Network, Address, Lat, Lon, ToneHz, Variant, Kind, BaudIdx, ShiftIdx, Wpm, Dbm, Function,
    Reverse, RdsPi, Slot, ColorCode, Talkgroup, BandwidthMhz, Deviation, Preemph
};

struct TxParamDef {
    const char* key = "";          // the value's name: the opt key for Opt rows ("am.depthPct"), a stable name for the others ("pager.capcode")
    const char* label = "";
    const char* help = "";         // one line
    const char* unit = "";         // "Hz", "dB", "%", "s" ... (shown after the number)
    TxPType type = TxPType::Float;
    TxPGroup group = TxPGroup::Modulation;
    TxPLevel level = TxPLevel::Advanced;
    TxPField field = TxPField::None;
    void (*set)(TxRequest&, double) = nullptr;   // where a number goes when it is neither opt[key] nor a field (a SynthConfig member)
    double def = 0, lo = 0, hi = 1;
    const char* fmt = "%.2f";
    std::vector<const char*> choices;   // Choice: the names; the value is values[i] when given, else base + i
    std::vector<double> values;
    double base = 0;
    const char* when = nullptr;    // shown only when another row of the table has this value: "key=v", "key=v1|v2", "key>v", "key<v", "key!=v"
    bool custom = false;           // drawn by the mode's own widget in the panel (a picture source, the multiplex, a node identity ...), not by the generic rows
    bool hidden = false;           // declared (a generator reads it) but not drawn: a test hook or a value another screen sets
    bool needsAudio = false;       // only changes the signal when the audio is the microphone or a file (the test tone bypasses it)
    const char* meta = nullptr;    // not nullptr: this row does not change the generated samples, and why (shown in docs/TX-4.0.md, skipped by the test)
    // A Choice or Bool row: may it take value `idx` (a choice index; 0 / 1 for a Bool) with the other values `num` (every row of the table,
    // see txParamBlocked)? nullptr: yes;
    // otherwise why not, naming the clause of the standard (the panel greys the choice out with this as its tooltip). Used where a standard
    // forbids a combination outright (DVB-T2-Lite, EN 302 755 annex I); a combination that merely may not work is a warning (txParamWarnings).
    const char* (*block)(const std::map<std::string, double>& num, int idx) = nullptr;
    // the test: the value the row is set to (NaN: chosen from the range), and the sample text of a text row (empty: a sample of its field)
    double testValue = 0;
    bool hasTestValue = false;
    const char* testText = "";
    double probeSec = 0;           // how much of the signal the test compares (0: the mode's own default)
};

// What the table depends on besides the mode: the analog fall-back of a digital mode, the path / sub-mode choice (QO-100 SSB or DATV), the list entry.
struct TxParamCtx {
    bool analogVoice = false;
    int variant = 0;               // QO-100: 0 SSB, 1 DATV
    std::string subId;             // the mode list's protocol entry ("wefax", "navtex", ...)
};

// The rows of a mode, in drawing order. The same pointer for the same mode and context.
const std::vector<TxParamDef>& txParams(const TxModeSpec& m, const TxParamCtx& ctx);
// A row by key, or nullptr
const TxParamDef* txParamFind(const std::vector<TxParamDef>& rows, const std::string& key);
// Is the row shown with these values (the `when` condition)? Values missing from `num` count as the row's default.
bool txParamShown(const TxParamDef& d, const std::vector<TxParamDef>& rows, const std::map<std::string, double>& num);
// Why the row may not take value idx with these values (its `block` rule), or nullptr when it may. Values missing from `num` count as the
// rows' defaults: the rule always sees every row of the table.
const char* txParamBlocked(const TxParamDef& d, const std::vector<TxParamDef>& rows, const std::map<std::string, double>& num, int idx);
// Every row whose value its `block` rule forbids is moved to an allowed one: its default when that is allowed, else the first allowed choice.
// Rows are taken in table order, so a row's rule may depend on the rows above it (DVB-T2: profile, then FFT, guard, pilot pattern ...).
// Returns true when a value changed. txParamsApplyNumbers applies the same to its copy, so a generator never gets a forbidden combination.
bool txParamsFix(const std::vector<TxParamDef>& rows, std::map<std::string, double>& num);
// The number sent for a row's value: a Choice index turned into its value, other types unchanged
double txParamValue(const TxParamDef& d, double v);
// One row's value into the request: numbers by `field`, `set` or opt[key]; texts (Text / File rows) from `text`
void txParamApply(const TxParamDef& d, double v, const std::string& text, TxRequest& r);
// Every numeric row of the table into the request (defaults for the ones `num` does not have). Texts are left as they are.
void txParamsApplyNumbers(const std::vector<TxParamDef>& rows, const std::map<std::string, double>& num, TxRequest& r);
// The request fields no row of the table writes are put back to their defaults, so a value typed for another mode (a callsign, a text, an
// address, a tone) can never reach this mode's generator unseen. Fields set by a mode's own screen (Winlink, MeshCore) are kept for those modes.
void txParamsSanitize(const TxModeSpec& m, const TxParamCtx& ctx, TxRequest& r);
// The keys of TxRequest::opt a generator may read that are not rows of any mode: the radio's calibration (tx_control.h) and the shared output stage
const std::vector<std::string>& txParamRadioKeys();
// The group's name for the panel ("Modulation", ...)
const char* txParamGroupName(TxPGroup g);
// Hints that do not block: a pilot pattern a DVB-T2 FFT size does not allow, a text longer than the mode carries ... ("" each when fine)
std::vector<std::string> txParamWarnings(const TxModeSpec& m, const TxRequest& r);

// Key trace for the test: while set, TxRequest::o() adds every key it is asked for (see tx.h)
void txTraceKeys(std::vector<std::string>* sink);

} // namespace dect2
