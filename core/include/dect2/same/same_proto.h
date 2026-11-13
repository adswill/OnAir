// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NOAA Weather Radio SAME / EAS alert messages (47 CFR 11.31): parsing and the plain-English tables. Receive only: the text of an alert
// is a public broadcast.
//
//   ZCZC-ORG-EEE-PSSCCC[-PSSCCC...]+TTTT-JJJHHMM-LLLLLLLL-
//
// ORG originator, EEE event code, P the Partial County Alerting digit (0 the whole county, 1 - 9 a part of it), SS the state FIPS code,
// CCC the county FIPS code (000 the whole state), TTTT the purge time (hours and minutes the alert is valid), JJJHHMM the issue time (day of
// the year and UTC hour and minute), LLLLLLLL the sender's call sign ('/' replaces '-'). "NNNN" ends a message.
//
// County names are NOT built in (a wrong name is worse than none): a location shows "county FIPS 029" unless the user loads a county table
// (loadCountyCsv). The state / territory table, the Canadian province table (Statistics Canada codes, see the note in same_proto.cpp)
// and the state centres used to place an alert on a map (approximate, a whole state or province) are built in.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace same {

enum Country { CountryUs = 0, CountryCanada = 1 };

struct Location {
    std::string code;           // "PSSCCC", as sent
    int pca = 0;                // the leading digit: 0 whole county, 1 - 9 a part of it
    int state = 0;              // SS
    int county = 0;             // CCC (0 = the whole state)
    std::string pcaName;        // "" for 0, else "northwest" ... "southeast"
    std::string stateName;      // "" when the code is not in the table
    std::string countyName;     // "" when no county table is loaded or the county is not in it
    std::string text;           // "Northwest part of county FIPS 201, Texas": ready to show
    bool hasPos = false;        // the state's centre (approximate: it is not the place of the alert)
    double lat = 0, lon = 0;
};

struct Message {
    bool valid = false;         // every field passed its check
    bool eom = false;           // "NNNN"
    std::string raw;
    std::string err;            // why not valid
    std::string org, orgName;
    std::string event, eventName;
    std::string eventKind;      // "warning", "watch", "emergency", "statement", "test", "message", ""
    std::vector<Location> locs;
    int purgeMin = 0;           // TTTT in minutes
    int issueDay = 0, issueHour = 0, issueMin = 0;   // JJJ HH MM (UTC)
    std::string callsign;
};

const char* originatorName(const std::string& org);          // "" when unknown
const char* eventName(const std::string& code);              // "" when unknown
const char* pcaName(int digit);                              // "" for 0 and out of range
const char* stateName(int fips, Country c = CountryUs);      // "" when not in the table
std::string stateAbbr(int fips, Country c = CountryUs);
bool stateCentre(int fips, Country c, double& lat, double& lon);

// A character that may appear in a SAME message (ASCII 0x20 .. 0x7E: the format uses digits, capitals, + - / and space)
bool validChar(uint8_t c);
// The message cut to what the format says (from "ZCZC" or "NNNN" to the last '-'); "" when it does not start so
std::string trimMessage(const std::string& bytes);
// Parse and check ("ZCZC-..." or "NNNN"); m.valid says whether every field is right. County names come from the loaded table, if any.
Message parse(const std::string& text, Country c = CountryUs);
// One location code in words, with the county table as loaded now (so a table loaded later names the history too)
Location locate(const std::string& code, Country c);
// Day of the year (1 - 366) and UTC time -> "May 03 14:30 UTC" for the given year (leap years follow the year); "" when out of range
std::string issueText(int year, int day, int hour, int minute);
// The issue time as an epoch (UTC seconds) for the given year; 0 when the date is out of range
int64_t issueEpoch(int year, int day, int hour, int minute);
std::string purgeText(int minutes);                          // "1 h 30 min"

// The user's county table. Lines are "SSCCC,Name", "SS,CCC,Name" or the Census file national_county.txt ("AL,01,001,Autauga County,H1");
// a header line and blank lines are skipped. Replaces the table; returns the number of counties read (0: nothing changed, err says why).
size_t loadCountyCsv(const std::string& text, std::string* err = nullptr);
size_t loadCountyFile(const std::string& path, std::string* err = nullptr);
size_t countyCount();
void clearCounties();
std::string countyNameOf(int state, int county);             // "" when not loaded

} // namespace same
} // namespace dect2
