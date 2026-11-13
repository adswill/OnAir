// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham contest helpers for the keyboard modes (RTTY, CW, PSK): the exchange of the common contests, serial numbers, cut numbers,
// the received exchange read from the decoded text, the dupe check per band and mode, and the Cabrillo 3.0 QSO line.
// The exchanges follow the contests' published rules (CQ WW RTTY / CW: RST + CQ zone, plus the state or province from W / VE in RTTY;
// CQ WPX: RST + serial; ARRL RTTY Roundup: RST + state / province or serial; NAQP: name + state; ARRL Field Day: class + section;
// BARTG: RST + serial + time; generic serial). Pure logic: no audio, no radio.
#pragma once
#include <string>
#include <vector>

namespace dect2::ham {

enum class ContestField { Rst, Serial, CqZone, ItuZone, State, StateOrSerial, Name, Class, Section, Time, Power, Grid };
// State: a US state or Canadian province (optional in CQ WW RTTY, where only W / VE stations send it); StateOrSerial: that, or a serial from DX
struct KeyerContestDef {
    const char* id;               // "cqww-rtty"
    const char* name;             // "CQ WW RTTY"
    const char* cabrillo;         // Cabrillo CONTEST: tag
    std::vector<ContestField> fields;   // the exchange in the order sent
    bool cwCut;                   // CW: 599 as 5NN, zeros as T in numbers
};
const std::vector<KeyerContestDef>& keyerContestDefs();
const KeyerContestDef* contestById(const std::string& id);
const char* contestFieldName(ContestField f);     // "RST", "Serial", ...

// Cut numbers (CW): 0 -> T, 9 -> N (and with `all`, 1 -> A, 5 -> E). The reverse reads T O -> 0, N -> 9, A -> 1, E -> 5 in a number.
std::string cutNumbers(const std::string& s, bool all = false);
std::string uncutNumbers(const std::string& s);

struct ContestStation {
    std::string myCall, myName, myState, mySection, myClass, myGrid;
    int cqZone = 0, ituZone = 0;
    std::string power;            // "100" / "KW" (contests that send power)
};
struct ContestExchange {          // one side of the exchange, field by field (as text, "" not given)
    std::vector<std::pair<ContestField, std::string>> v;
    std::string get(ContestField f) const;
};

// Our exchange for the next QSO: "599 14 14" style text (each field twice when `twice`, as RTTY operators send it).
std::string contestExchangeText(const KeyerContestDef& d, const ContestStation& s, int serial, const std::string& utcHHMM, bool twice);
// The macros of a contest (CQ, exchange, TU, again?): {CALL} {EXCH} {MYCALL} filled from s and the exchange text.
struct ContestMacro { const char* name; std::string text; };
std::vector<ContestMacro> contestMacros(const KeyerContestDef& d, const ContestStation& s, const std::string& call, const std::string& exch);

// The received exchange, read from decoded text that follows the other station's call: the fields in order, each validated (RST 1..5 1..9 1..9,
// zones 1..40 / 1..90, a state / province / section from the lists, a number); repeated copies agree or the first good one wins. False when a
// required field is missing.
bool contestParseExchange(const KeyerContestDef& d, const std::string& text, ContestExchange& out, std::string* why = nullptr);

// ---- the log of a contest ----
struct ContestQso {
    std::string call, band, mode;          // band "20m", mode "RY" "CW" "PH" "DG" (Cabrillo)
    double freqHz = 0, utc = 0;
    std::string sent, rcvd;                // the exchange texts (Cabrillo order)
};
class ContestLog {
public:
    explicit ContestLog(const KeyerContestDef& d) : d_(d) {}
    int nextSerial() const { return (int)q_.size() + 1; }
    bool dupe(const std::string& call, const std::string& band, const std::string& mode) const;   // worked before on this band and mode
    void add(const ContestQso& q) { q_.push_back(q); }
    const std::vector<ContestQso>& qsos() const { return q_; }
    // "QSO: 14080 RY 2026-10-22 1200 LY1ABC        599 15     W1AW          599 MA" (Cabrillo 3.0)
    std::string cabrilloLine(const ContestQso& q, const std::string& myCall) const;
    std::string cabrillo(const ContestStation& s) const;   // the whole file: header, every QSO line, END-OF-LOG:
private:
    const KeyerContestDef& d_;
    std::vector<ContestQso> q_;
};
bool isUsState(const std::string& s);     // the 50 states + DC
bool isVeProvince(const std::string& s);  // the Canadian provinces / territories (RTTY Roundup / CQ WW RTTY)
bool isArrlSection(const std::string& s); // ARRL / RAC sections (Field Day)

}  // namespace dect2::ham
