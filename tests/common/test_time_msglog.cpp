// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Time display helper (dect2/common/time_fmt.h) and the message log writer (dect2/common/msg_log.h).
#include "dect2/common/msg_log.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
using namespace dect2;
using namespace dect2::timefmt;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main() {
    // 2026-10-22 14:03:11 UTC
    const int64_t t = 1792677791;
    CHECK(format(t, Zone::Utc, Style::DateClock, false) == "2026-10-22 14:03:11", "utc %s", format(t, Zone::Utc, Style::DateClock, false).c_str());
    CHECK(format(t, Zone::Utc, Style::Clock, true) == "14:03:11 (UTC)", "label");
    CHECK(format(t, Zone::Utc, Style::ClockShort, false) == "14:03", "short");
    CHECK(format(t, Zone::Utc, Style::DateClockMin, false) == "2026-10-22 14:03", "min");
    CHECK(format(0, Zone::Utc, Style::Clock, true) == "-", "unset");
    CHECK(format(-5, Zone::Local, Style::Clock, false) == "-", "negative");
    CHECK(showUtc(t) == "2026-10-22 14:03:11 (UTC)", "ham");
#ifndef _WIN32
    setenv("TZ", "Etc/GMT-3", 1);   // UTC+3, no daylight saving
    tzset();
    CHECK(format(t, Zone::Local, Style::DateClock, true) == "2026-10-22 17:03:11 (local)", "local %s", format(t, Zone::Local, Style::DateClock, true).c_str());
    // across midnight: 22:30 UTC is 01:30 next day
    CHECK(format(1792708200, Zone::Local, Style::DateClock, false) == "2026-10-23 01:30:00", "midnight %s", format(1792708200, Zone::Local, Style::DateClock, false).c_str());
    setZone(Zone::Local); setLabel(false);
    CHECK(clock(t) == "17:03:11", "global local");
    CHECK(show(t, Style::ClockShort) == "17:03", "show local");
#endif
    setZone(Zone::Utc); setLabel(true);
    CHECK(clock(t) == "14:03:11", "clock has no label");
    CHECK(show(t) == "14:03:11 (UTC)", "show label");
    setLabel(false);

    // message log lines
    msglog::Row r; r.epoch = t; r.mode = "POCSAG 1200"; r.freqHz = 466230000; r.address = "1234567"; r.text = "line1\nline2\ttab";
    CHECK(msglog::formatLine(r, Zone::Utc, false) == "2026-10-22 14:03:11 | POCSAG 1200 | 466.2300 MHz | 1234567 | line1 line2 tab", "line %s", msglog::formatLine(r, Zone::Utc, false).c_str());
    CHECK(msglog::formatLine(r, Zone::Utc, true).find("(UTC)") != std::string::npos, "line label");
    msglog::Row e;
    CHECK(msglog::formatLine(e, Zone::Utc, false) == "- | - | - | - | ", "empty row %s", msglog::formatLine(e, Zone::Utc, false).c_str());
    const std::string all = msglog::formatAll({r, r}, Zone::Utc, false);
    CHECK(all.rfind("# OnAir message log: time (UTC)", 0) == 0, "header");
    size_t nl = 0; for (char c : all) nl += c == '\n';
    CHECK(nl == 3, "3 lines, got %zu", nl);

    // day rotation names
    CHECK(msglog::dayPath("/a/b/log.txt", t, Zone::Utc) == "/a/b/log-2026-10-22.txt", "dayPath %s", msglog::dayPath("/a/b/log.txt", t, Zone::Utc).c_str());
    CHECK(msglog::dayPath("/a.d/log", t, Zone::Utc) == "/a.d/log-2026-10-22.log", "dayPath no ext %s", msglog::dayPath("/a.d/log", t, Zone::Utc).c_str());

    // writer: save all + auto-save with rotation
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "onair_msglog_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::string err;
    const std::string one = (dir / "sub" / "all.txt").string();
    std::filesystem::create_directories(dir / "sub", ec);
    CHECK(msglog::saveAll(one, {r}, Zone::Utc, false, err), "saveAll: %s", err.c_str());
    CHECK(slurp(one) == msglog::formatAll({r}, Zone::Utc, false), "saveAll content");
    CHECK(!msglog::saveAll((dir / "nope" / "x" / "y.txt").string(), {r}, Zone::Utc, false, err) && !err.empty(), "saveAll into missing folder fails");

    msglog::AutoSaver as;
    CHECK(!as.append(r, Zone::Utc, false, err), "no base fails");
    as.setBase((dir / "auto" / "pager.log").string());
    msglog::Row r2 = r; r2.epoch = t + 86400; r2.address = "42";
    CHECK(as.append(r, Zone::Utc, false, err), "append1 %s", err.c_str());
    CHECK(as.append(r, Zone::Utc, false, err), "append2");
    CHECK(as.append(r2, Zone::Utc, false, err), "append3");
    const std::string d1 = slurp((dir / "auto" / "pager-2026-10-22.log").string());
    const std::string d2 = slurp((dir / "auto" / "pager-2026-10-23.log").string());
    CHECK(d1 == msglog::headerLine(Zone::Utc) + "\n" + msglog::formatLine(r, Zone::Utc, false) + "\n" + msglog::formatLine(r, Zone::Utc, false) + "\n", "day 1 file: header once, two lines");
    CHECK(d2 == msglog::headerLine(Zone::Utc) + "\n" + msglog::formatLine(r2, Zone::Utc, false) + "\n", "day 2 file");
    std::filesystem::remove_all(dir, ec);
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
