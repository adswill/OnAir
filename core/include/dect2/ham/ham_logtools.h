// Logbook tools for the Ham screen (4.0): Cabrillo import (the export is ham_ops.h cabrilloExport, so a contest log round-trips), QSL
// confirmations merged from a LoTW / eQSL / Club Log ADIF download, QSL tracking counts, dated backups of the log file, and the portable
// activation programs (POTA, SOTA, WWFF, IOTA): the ADIF reference fields, the activation count, the POTA upload file name and the SOTA CSV.
// Pure logic and local files only: nothing is sent anywhere.
#pragma once
#include "dect2/ham/ham_log.h"
#include <map>
#include <string>
#include <vector>

namespace dect2::ham {

// ---- Cabrillo 3.0 import ----
struct CabrilloLog {
    std::map<std::string, std::string> header;   // "CONTEST" -> "CQ-WW-CW", the last value of each tag (SOAPBOX lines joined)
    std::vector<LogQso> qsos;                    // call, freq, mode, start, myCall, rstSent / stx, rstRcvd / srx, contestId
    int bad = 0;                                 // QSO: lines that did not read
};
// The QSO line is "QSO: freq mo date time mycall <sent exchange> call <received exchange> [t]": the exchange halves have the same number of
// fields (a final transmitter id 0 / 1 is dropped). An RST at the start of a half goes to rstSent / rstRcvd, the rest to stx / srx.
CabrilloLog cabrilloParse(const std::string& text);
std::string cabrilloToAdifMode(const std::string& mo);   // CW -> CW, RY -> RTTY, PH -> SSB, FM -> FM, DG -> DIGI (no ADIF mode is implied)

// ---- QSL confirmations ----
enum class QslSource { Lotw, Eqsl, Card, ClubLog };
struct MergeResult { int matched = 0, newlyConfirmed = 0, unmatched = 0; };
// Each record of the download (QSO_DATE, TIME_ON, CALL, BAND or FREQ, MODE) is matched to a QSO with the same base call, band and mode class
// started within 30 minutes; that QSO gets the confirmation (LoTW: LOTW_QSL_RCVD, eQSL: EQSL_QSL_RCVD, card: QSL_RCVD, Club Log: CLUBLOG_QSO_UPLOAD_STATUS
// stays, QSL_RCVD is not set) and the fields the service knows that the log lacks (DXCC, zones, state, grid, county). One undo step per changed QSO.
MergeResult mergeConfirmations(Logbook& log, const std::string& adifText, QslSource src);
struct QslSummary { int qsos = 0, sent = 0, rcvd = 0, lotw = 0, eqsl = 0, confirmed = 0, pending = 0; };   // pending: QSL sent, nothing received
QslSummary qslSummary(const std::vector<LogQso>& log);

// ---- backups ----
// Copies the log to dir/onair-log-YYYYMMDD-HHMMSS.adi and deletes the oldest such files beyond `keep`. The path of the copy, "" + err on failure.
std::string logBackup(const std::string& logPath, const std::string& dir, double nowUtc, int keep, std::string& err);
// true when dir holds no backup newer than minGapSec (the app backs up once a day when the Ham screen opens)
bool logBackupDue(const std::string& dir, double nowUtc, double minGapSec = 86400);
std::vector<std::string> logBackups(const std::string& dir);   // newest first

// ---- LoTW through the user's own TQSL (OnAir never holds the certificate) ----
// The command line that signs and uploads an ADIF file with TQSL: quiet, no date dialog, upload, duplicates skipped ("compliant"), exit after.
// tqsl: the program ("tqsl" when it is on the PATH); location: the TQSL station location name. Quotes in the arguments are removed.
std::string tqslCommand(const std::string& tqsl, const std::string& location, const std::string& adifFile);
bool tqslOk(int exitCode);                               // 0 done, 9 done with some duplicates skipped

// ---- portable activations ----
enum class Program { None = 0, Pota = 1, Sota = 2, Wwff = 3, Iota = 4 };
const char* programName(Program p);                      // "POTA", "SOTA", "WWFF", "IOTA", ""
Program programFromName(const std::string& s);
// POTA "US-0001" / "K-0001" / "DE-0123"; SOTA "W7W/LC-001", "G/LD-001"; WWFF "ONFF-0001"; IOTA "EU-005"
bool refValid(Program p, const std::string& ref);
std::string refNormal(const std::string& ref);           // upper case, spaces removed
struct Activation {
    Program prog = Program::None;
    std::vector<std::string> refs;                       // more than one: a two-fer (POTA counts the QSOs for every park)
    double startUtc = 0;
    bool active() const { return prog != Program::None && !refs.empty(); }
};
// the ADIF fields of my own reference: MY_SIG, MY_SIG_INFO and MY_POTA_REF / MY_SOTA_REF / MY_WWFF_REF / MY_IOTA
void applyActivation(LogQso& q, const Activation& a);
// the other station's reference (hunting a spot): SIG, SIG_INFO and POTA_REF / SOTA_REF / WWFF_REF / IOTA
void applyHunted(LogQso& q, Program p, const std::string& ref);
struct ActivationProgress { int qsos = 0, needed = 0; bool done = false; std::map<std::string, int> perBand; };
// POTA: distinct call / band / mode on the UTC day of nowUtc, 10 needed; SOTA: distinct calls since startUtc on that day, 4 needed;
// WWFF: distinct call / band / mode, 44 for the award (10 for the log); IOTA: no minimum. The QSOs counted are those carrying the reference.
ActivationProgress activationProgress(const std::vector<LogQso>& log, const Activation& a, double nowUtc);
std::string potaFileName(const std::string& myCall, const std::string& ref, double utc);   // "K1ABC@K-0001-20261112.adi" ('/' -> '_')
// the indices of the QSOs of one reference on the UTC day of utc (POTA: one file per park and day)
std::vector<size_t> activationQsos(const std::vector<LogQso>& log, Program p, const std::string& ref, double utc);
// SOTA CSV v2: "V2,MYCALL/P,W7W/LC-001,12/11/26,1205,14MHz,CW,K1ABC,,"
std::string sotaCsv(const std::vector<LogQso>& log, const std::string& summit, const std::string& myCall, double fromUtc = 0);

}  // namespace dect2::ham
