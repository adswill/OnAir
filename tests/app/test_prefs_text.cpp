// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The values of settings.conf on Linux and Windows (app/platform/prefs_text.h, #360): a value with line breaks comes back whole,
// and a file from before the escapes is read as it is.
#include "../../app/platform/prefs_text.h"
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { failures++; std::printf("FAIL: %s\n", msg); } } while (0)

// what one line of the file holds after a save and a read
static std::string roundTrip(const std::string& v) { return prefstext::fromFile(prefstext::escape(v), prefstext::kFormat); }

int main() {
    using namespace prefstext;
    // the mesh channel keys: protocol<TAB>name<TAB>secret, one a line (app/mesh/mesh_settings_ui.cpp)
    const std::string keys = "2\t#norniron\t8b3387e9c5cdea6ac9e5edbaa115cd72\n2\t#test\tabcdef0123456789abcdef0123456789\n1\tSC\tAQ==\n";
    CHECK(escape(keys).find('\n') == std::string::npos, "an escaped value is one line");
    CHECK(roundTrip(keys) == keys, "three keys come back as three keys");
    // the FM scan list: fields split by '|', stations by line breaks (app/state/prefs.cpp)
    const std::string fm = "100.900|Radio CBN|News|31.2|1|1|C2A1|0|Ola|\n104.500|Atlantida|Pop|28|1|1|C3B2|1||Atl\n";
    CHECK(roundTrip(fm) == fm, "the FM stations keep their '|' and their lines");
    // Windows paths and backslashes
    const std::string path = "C:\\Users\\me\\Recordings\\new\\rtl";
    CHECK(roundTrip(path) == path, "a Windows path comes back as it was");
    CHECK(escape(path).find("\\\\n") != std::string::npos, "its \\n is not taken for a line break");
    CHECK(roundTrip("ends with \\") == "ends with \\", "a trailing backslash");
    CHECK(roundTrip("\r\n\\\\") == "\r\n\\\\", "CR, LF and two backslashes");
    CHECK(roundTrip("") == "", "an empty value");
    CHECK(escape("plain text; a=b|c") == "plain text; a=b|c", "a plain value is written as it is");
    // a file from before the escapes: no settingsFormat line
    CHECK(fromFile(path, 0) == path, "an old file's Windows path keeps its single backslashes");
    CHECK(fromFile("a\\nb", 0) == "a\\nb", "an old file is not unescaped");
    // a hand edit with an escape that is not one of ours
    CHECK(unescape("tab\\there") == "tab\\there", "an unknown escape is kept");
    std::puts(failures ? "FAILED" : "prefs_text ok");
    return failures ? 1 : 0;
}
