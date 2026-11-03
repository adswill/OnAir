// Shared export for every table of heard items: one toolbar row = "Save results..." (CSV / KML / GeoJSON ... by format
// box) + "Copy CSV" (clipboard). Built on resexp (app/state/results_export): CSV with RFC 4180 quoting in UTF-8, KML and
// GeoJSON for rows with lat/lon. The pure half below needs no GUI, so tests link it.
#pragma once
#include "../../state/results_export.h"
#include <string>

namespace kit {

// true when the table has a lat and a lon column and at least one row with both filled (then KML / GeoJSON are useful)
bool hasPositions(const resexp::Table& t);
// the text "Copy CSV" puts on the clipboard: the CSV, with CRLF turned into LF so it pastes cleanly into a sheet
std::string clipboardCsv(const resexp::Table& t);

// GUI half (export_ui.cpp): the toolbar row. kind names the file ("sonde"...). `note` receives a status line.
void exportBar(const resexp::Table& t, const char* kind, std::string& note, const char* id = "kexp");

}  // namespace kit
