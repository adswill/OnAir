// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Worked-before and needed (4.0): for a station heard or spotted, what it would add to the log: a new DXCC entity, the entity on a new band or
// mode class, a new CQ or ITU zone, a new US state, a new grid square, a new WPX prefix, or a call not yet worked on this band and mode. Also
// the alert rules (needed entity / zone / state / grid / prefix, a watch list of calls with * and ? wildcards) with a quiet time per station.
// Pure logic; ham_log.h WorkedIndex stays for the old colour marks, this one adds the dimensions the award programs count.
#pragma once
#include "dect2/ham/ham_log.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dect2::ham {

// The CQ WPX prefix of a call: "LY1ABC" -> "LY1", "K1ABC/4" -> "K4", "PJ4/K1ABC" -> "PJ4", "DL/LY1ABC" -> "DL0", "RAEM" -> "RA0",
// "LY1ABC/P" -> "LY1". "" when the call does not read.
std::string wpxPrefix(const std::string& call);

struct NeedQuery {
    std::string call, grid, band, mode;   // mode: the ADIF mode ("FT8", "CW")
    std::string dxccKey;                  // the entity (ham_dxcc.h DxccHit::key); "" when not known: no entity marks then
    std::string state;                    // US state (two letters) when known
    int cqz = 0, ituz = 0;
};
enum NeedBit : unsigned {
    NeedCall = 1u << 0,          // never worked
    NeedCallBand = 1u << 1,      // not on this band
    NeedCallSlot = 1u << 2,      // not on this band and mode
    NeedDxcc = 1u << 3,
    NeedDxccBand = 1u << 4,
    NeedDxccClass = 1u << 5,     // the entity not yet in this mode class (CW, PHONE, DIGI)
    NeedCqz = 1u << 6,
    NeedItuz = 1u << 7,
    NeedState = 1u << 8,
    NeedGrid = 1u << 9,
    NeedGridBand = 1u << 10,
    NeedPrefix = 1u << 11,
    NeedDxccQsl = 1u << 12,      // the entity is worked but no QSO with it is confirmed
};
struct NeedResult {
    unsigned bits = 0;
    int level = 0;               // 6 new DXCC, 5 DXCC new band, 4 DXCC new mode class, 3 new zone / state, 2 new grid / prefix, 1 new call or slot, 0 worked
    std::string label;           // "NEW DXCC", "new band", "new mode", "new zone", "new state", "new grid", "new prefix", "new call", "new slot", "B4"
    bool has(NeedBit b) const { return (bits & b) != 0; }
};

class NeedIndex {
public:
    // dxccKey of a logged QSO: its DXCC number as text, else its country name (what awardKey(q, Award::Dxcc) gives)
    void build(const std::vector<LogQso>& log);
    void add(const LogQso& q);
    NeedResult check(const NeedQuery& q) const;
    size_t calls() const { return call_.size(); }
private:
    std::set<std::string> call_, callBand_, callSlot_, dxcc_, dxccBand_, dxccClass_, dxccConf_, cqz_, ituz_, state_, grid_, gridBand_, prefix_;
};

// ---- alerts ----
bool wildMatch(const std::string& pattern, const std::string& text);   // * any run, ? one character; case-insensitive
struct AlertRules {
    bool newDxcc = true, newDxccBand = true, newDxccClass = false, newZone = false, newState = false, newGrid = false, newPrefix = false;
    std::vector<std::string> watch;      // calls or patterns ("VP8*", "3Y0J", "*/MM")
    double quietSec = 900;               // the same call on the same band alerts once in this time
};
class NeedAlerter {
public:
    AlertRules rules;
    // true when this one should alert now; why: "New DXCC: 3Y0J" / "Watched call: VP8PJ"
    bool check(const NeedQuery& q, const NeedResult& r, double nowUtc, std::string& why);
    void reset() { last_.clear(); }
private:
    std::map<std::string, double> last_;
};

}  // namespace dect2::ham
