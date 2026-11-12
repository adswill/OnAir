// Logbook tools (4.0): Cabrillo export -> import round trip (RST and serial, a two-field exchange without RST, a transmitter id, X-QSO, bad
// lines), LoTW / card / eQSL confirmations merged into the log, QSL counts, dated backups with pruning, POTA / SOTA / WWFF / IOTA references,
// the activation count, the POTA file name and the SOTA CSV.
#include "dect2/ham/ham_logtools.h"
#include "dect2/ham/ham_ops.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    const double day = 1794441600.0;   // 2026-11-12 00:00 UTC

    // ---- Cabrillo round trip ----
    {
        std::vector<LogQso> log(3);
        log[0].call = "K1ABC"; log[0].freqHz = 14025000; log[0].mode = "CW"; log[0].startUtc = day + 12 * 3600; log[0].rstSent = "599"; log[0].stx = "001";
        log[0].rstRcvd = "599"; log[0].srx = "123"; log[0].contestId = "CQ-WPX-CW"; log[0].myCall = "LY1ABC";
        log[1] = log[0]; log[1].call = "JA1XYZ"; log[1].freqHz = 21025000; log[1].startUtc = day + 12 * 3600 + 120; log[1].stx = "002"; log[1].srx = "45";
        log[2] = log[0]; log[2].call = "W9ZZZ"; log[2].contestId = "OTHER";
        CabrilloHeader h; h.call = "LY1ABC"; h.contest = "CQ-WPX-CW"; h.soapbox = "73";
        const std::string cab = cabrilloExport(log, h);
        const CabrilloLog L = cabrilloParse(cab);
        CHECK(L.qsos.size() == 2 && L.bad == 0);
        CHECK(L.header.count("CONTEST") && L.header.at("CONTEST") == "CQ-WPX-CW" && L.header.at("CALLSIGN") == "LY1ABC");
        if (L.qsos.size() == 2) {
            const LogQso& a = L.qsos[0];
            CHECK(a.call == "K1ABC" && std::fabs(a.freqHz - 14025000) < 1 && a.mode == "CW" && a.startUtc == day + 12 * 3600 && a.myCall == "LY1ABC");
            CHECK(a.rstSent == "599" && a.stx == "001" && a.rstRcvd == "599" && a.srx == "123" && a.contestId == "CQ-WPX-CW" && a.band == "20m");
            CHECK(L.qsos[1].call == "JA1XYZ" && L.qsos[1].band == "15m" && L.qsos[1].srx == "45" && L.qsos[1].startUtc == day + 12 * 3600 + 120);
        }
        // Field Day (no RST), a transmitter id, a VHF band, X-QSO, a bad line, RTTY / phone modes
        const std::string fd = "START-OF-LOG: 3.0\r\nCONTEST: ARRL-FD\r\nSOAPBOX: one\r\nSOAPBOX: two\r\n"
                               "QSO:  7040 CW 2026-06-27 1800 LY1ABC        2A EMA     K1ABC         1D MA\r\n"
                               "QSO: 14085 RY 2026-06-27 1801 LY1ABC        599 001    K2ABC         599 017    1\r\n"
                               "QSO:    50 PH 2026-06-27 1802 LY1ABC        59 002     K3ABC         59 018\r\n"
                               "X-QSO: 14085 RY 2026-06-27 1803 LY1ABC      599 003    K4ABC         599 019\r\n"
                               "QSO: abc\r\nQSO: 14085 RY 2026-99-27 1803 LY1ABC 599 003 K5ABC 599 019\r\nEND-OF-LOG:\r\n";
        const CabrilloLog F = cabrilloParse(fd);
        CHECK(F.qsos.size() == 3 && F.bad == 2 && F.header.at("SOAPBOX") == "one\ntwo");
        if (F.qsos.size() == 3) {
            CHECK(F.qsos[0].call == "K1ABC" && F.qsos[0].rstSent.empty() && F.qsos[0].stx == "2A EMA" && F.qsos[0].srx == "1D MA" && F.qsos[0].contestId == "ARRL-FD");
            CHECK(F.qsos[0].startUtc == 1782583200.0);
            CHECK(F.qsos[1].call == "K2ABC" && F.qsos[1].mode == "RTTY" && F.qsos[1].srx == "017");
            CHECK(F.qsos[2].call == "K3ABC" && F.qsos[2].mode == "SSB" && F.qsos[2].band == "6m" && F.qsos[2].rstRcvd == "59");
        }
        CHECK(cabrilloToAdifMode("DG") == "DIGI" && cabrilloToAdifMode("fm") == "FM");
    }

    // ---- confirmations ----
    {
        Logbook b;
        LogQso q; q.call = "K1ABC"; q.freqHz = 14074000; q.mode = "FT8"; q.startUtc = day + 3600;
        const long long k1 = b.add(q);
        q.call = "DL1XYZ"; q.freqHz = 7025000; q.mode = "CW"; q.startUtc = day + 7200; q.qslSent = true;
        const long long d1 = b.add(q);
        const std::string lotw = "ARRL Logbook of the World Status Report<EOH>\n"
                                 "<CALL:5>K1ABC<BAND:3>20M<MODE:3>FT8<QSO_DATE:8>20261112<TIME_ON:6>010100<QSL_RCVD:1>Y<DXCC:3>291<CQZ:1>5<STATE:2>MA<GRIDSQUARE:6>FN42ab<EOR>\n"
                                 "<CALL:5>W9ZZZ<BAND:3>20M<MODE:3>FT8<QSO_DATE:8>20261112<TIME_ON:6>010100<QSL_RCVD:1>Y<EOR>\n"
                                 "<CALL:5>K1ABC<BAND:3>40M<MODE:3>FT8<QSO_DATE:8>20261112<TIME_ON:6>010100<QSL_RCVD:1>Y<EOR>\n";
        const MergeResult m = mergeConfirmations(b, lotw, QslSource::Lotw);
        CHECK(m.matched == 1 && m.newlyConfirmed == 1 && m.unmatched == 2);
        const LogQso* k = b.find(k1);
        CHECK(k && k->lotwRcvd && k->dxcc == 291 && k->cqz == 5 && k->state == "MA" && k->grid == "FN42AB");
        const MergeResult again = mergeConfirmations(b, lotw, QslSource::Lotw);
        CHECK(again.matched == 1 && again.newlyConfirmed == 0);
        const std::string card = "<CALL:6>DL1XYZ<FREQ:5>7.026<MODE:2>CW<QSO_DATE:8>20261112<TIME_ON:4>0210<EOR>";   // 10 minutes off: still the QSO
        CHECK(mergeConfirmations(b, card, QslSource::Card).newlyConfirmed == 1 && b.find(d1)->qslRcvd);
        const std::string late = "<CALL:6>DL1XYZ<FREQ:5>7.026<MODE:2>CW<QSO_DATE:8>20261112<TIME_ON:4>0300<EOR>";    // an hour off: not this QSO
        CHECK(mergeConfirmations(b, late, QslSource::Eqsl).unmatched == 1);
        const std::string eq = "<CALL:6>DL1XYZ<BAND:3>40m<MODE:2>CW<QSO_DATE:8>20261112<TIME_ON:4>0200<EOR>";
        CHECK(mergeConfirmations(b, eq, QslSource::Eqsl).matched == 1 && b.find(d1)->extra.at("EQSL_QSL_RCVD") == "Y");
        const QslSummary s = qslSummary(b.qsos());
        CHECK(s.qsos == 2 && s.lotw == 1 && s.rcvd == 1 && s.eqsl == 1 && s.confirmed == 2 && s.sent == 1 && s.pending == 0);
        CHECK(b.canUndo());
    }

    // ---- backups ----
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "onair_ham_backup_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string logPath = (dir / "log.adi").string(), bk = (dir / "backups").string();
        std::string err;
        CHECK(logBackup(logPath, bk, day, 2, err).empty() && !err.empty());   // no log yet
        { std::ofstream o(logPath); o << "<CALL:5>K1ABC<EOR>\n"; }
        CHECK(logBackupDue(bk, day));
        const std::string p1 = logBackup(logPath, bk, day, 2, err);
        CHECK(!p1.empty() && fs::exists(p1) && p1.find("onair-log-20261112-000000.adi") != std::string::npos);
        CHECK(!logBackupDue(bk, day + 3600) && logBackupDue(bk, day + 86400));
        logBackup(logPath, bk, day + 86400, 2, err);
        logBackup(logPath, bk, day + 2 * 86400, 2, err);
        const auto all = logBackups(bk);
        CHECK(all.size() == 2 && all[0].find("20261114") != std::string::npos && all[1].find("20261113") != std::string::npos);
        fs::remove_all(dir);
    }

    // ---- activations ----
    {
        CHECK(refValid(Program::Pota, "US-0001") && refValid(Program::Pota, "k-0001") && refValid(Program::Pota, "DE-01234") && !refValid(Program::Pota, "US0001"));
        CHECK(refValid(Program::Sota, "W7W/LC-001") && refValid(Program::Sota, "G/LD-001") && !refValid(Program::Sota, "W7W-LC-001") && !refValid(Program::Sota, "W7W/LC-01"));
        CHECK(refValid(Program::Wwff, "ONFF-0001") && !refValid(Program::Wwff, "ON-0001"));
        CHECK(refValid(Program::Iota, "EU-005") && !refValid(Program::Iota, "XX-005") && !refValid(Program::None, "US-0001"));
        CHECK(programFromName("pota") == Program::Pota && std::string(programName(Program::Sota)) == "SOTA" && programFromName("x") == Program::None);
        Activation a; a.prog = Program::Pota; a.refs = {"us-0001", " US-0002 "};
        LogQso q; applyActivation(q, a);
        CHECK(q.extra["MY_POTA_REF"] == "US-0001,US-0002" && q.extra["MY_SIG"] == "POTA" && q.extra["MY_SIG_INFO"] == "US-0001,US-0002");
        LogQso h; applyHunted(h, Program::Sota, "w7w/lc-001");
        CHECK(h.extra["SOTA_REF"] == "W7W/LC-001" && h.extra["SIG"] == "SOTA");
        LogQso io; applyHunted(io, Program::Iota, "EU-005");
        CHECK(io.extra["IOTA"] == "EU-005" && !io.extra.count("SIG"));

        std::vector<LogQso> log;
        auto add = [&](const char* call, const char* band, const char* mode, double t, const char* ref) {
            LogQso x; x.call = call; x.band = band; x.mode = mode; x.startUtc = t; x.myCall = "K1ABC/P";
            if (ref) x.extra["MY_POTA_REF"] = ref;
            log.push_back(x);
        };
        const char* calls[] = {"W1AA", "W1AB", "W1AC", "W1AD", "W1AE", "W1AF", "W1AG", "W1AH", "W1AI"};
        for (int i = 0; i < 9; i++) add(calls[i], "20m", "SSB", day + 3600 + i * 60, "US-0001,US-0002");
        add("W1AA", "20m", "SSB", day + 4300, "US-0001,US-0002");      // a dupe: not counted
        add("W1AJ", "20m", "SSB", day + 4400, "US-9999");              // another park
        add("W1AK", "20m", "SSB", day - 3600, "US-0001");              // yesterday
        add("W1AL", "20m", "SSB", day + 5000, nullptr);                // no reference
        ActivationProgress p = activationProgress(log, a, day + 6000);
        CHECK(p.qsos == 9 && p.needed == 10 && !p.done && p.perBand["20m"] == 9);
        add("W1AA", "40m", "SSB", day + 5100, "US-0001@US-ME");        // the same call on another band counts (POTA)
        p = activationProgress(log, a, day + 6000);
        CHECK(p.qsos == 10 && p.done);
        CHECK(activationQsos(log, Program::Pota, "US-0001", day + 100).size() == 11);
        CHECK(potaFileName("K1ABC/P", "us-0001", day + 100) == "K1ABC_P@US-0001-20261112.adi");

        Activation s; s.prog = Program::Sota; s.refs = {"W7W/LC-001"}; s.startUtc = day + 1000;
        std::vector<LogQso> sl;
        LogQso x; x.call = "K1ABC"; x.freqHz = 14062000; x.mode = "CW"; x.startUtc = day + 12 * 3600 + 5 * 60; x.myCall = "N7ABC/P"; x.extra["MY_SOTA_REF"] = "W7W/LC-001";
        sl.push_back(x);
        x.call = "G4XYZ"; x.freqHz = 7032500; x.extra["SOTA_REF"] = "G/LD-001"; x.startUtc += 60; sl.push_back(x);
        x.call = "K1ABC"; x.freqHz = 7032500; x.startUtc += 60; sl.push_back(x);        // SOTA counts calls, not band slots
        x.call = "W1EARLY"; x.startUtc = day + 500; sl.push_back(x);                     // before the activation started
        CHECK(activationProgress(sl, s, day + 13 * 3600).qsos == 2 && activationProgress(sl, s, day + 13 * 3600).needed == 4);
        const std::string csv = sotaCsv(sl, "W7W/LC-001", "N7ABC/P", day + 1000);
        CHECK(csv.find("V2,N7ABC/P,W7W/LC-001,12/11/26,1205,14.062MHz,CW,K1ABC,,\n") == 0);
        CHECK(csv.find("V2,N7ABC/P,W7W/LC-001,12/11/26,1206,7.0325MHz,CW,G4XYZ,G/LD-001,\n") != std::string::npos);
        CHECK(csv.find("W1EARLY") == std::string::npos);
    }

    printf(fails ? "ham_logtools: %d FAILED\n" : "ham_logtools: all passed\n", fails);
    return fails ? 1 : 0;
}
