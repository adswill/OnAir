# RDS-TMC in plain words (3.9, issue #476)

OnAir already decoded RDS-TMC (groups 3A and 8A, ISO 14819-1) with a built-in English table of about 170 frequent events. Location codes were shown as numbers. 3.9 adds these:

- **Import of your own TMC files.** FM > Traffic (TMC) > *TMC tables and log* has *Import file...* and *Import folder...*. The files are checked, then copied to the settings folder (`<data folder>/tmc`) and read again at every start. *Remove all* deletes that folder.
- **Messages in plain words.** Each message shows the event text, the road (number and name), the location name with its junction number, the direction ("towards Basel"), the extent ("from Frankfurt-Süd (2 locations)"), the duration and diversion advice. The additional events of multi-group messages are translated as well, and "diversion via" / "destination" use location names. When an event list gives the duration type (D or L), the duration text follows it.
- **Log.** Every new message (and every change to one) is written once to `<path>.csv` and `<path>.txt`. Repeats are not written again. The log is on by default once a file is chosen with *Log file...*. A checkbox switches it off. CSV columns: time_utc, freq_mhz, pi, ps, ltn, sid, event_code, event, location_code, location, road, direction, extent, extent_text, duration, diversion, details.
- **No country database ships with OnAir.** Location tables are licensed national data. Without files, messages keep their codes, and a "(?)" tooltip explains how to import them. If the imported table number differs from the station's LTN, a warning is shown.

## Formats read (columns and delimiter detected automatically)

- **Delimiters:** `;`, `,`, tab and `|`, with quoted fields. Whether the first line is a header is detected.
- **Encodings:** UTF-8, UTF-8 with BOM, UTF-16 with BOM (a spreadsheet's "Unicode text"), and Latin-1 / Windows-1252 (anything that is not valid UTF-8).
- **Event lists:**
  - A code column, by name (CODE, ECD, Event code, Nr, Nummer, ...) or as the first column of numbers from 1 to 2047.
  - One or more text columns (Text, Description, Beschreibung, Meldung, English, Deutsch, TEXT_DE, ...).
  - The language of each text column comes from its name, then the file name (`_de`, `deu`, `english`, ...), then its words. German and English are recognised; any other language is kept as "xx" and can still be selected.
  - Quantifier variants ("Text (Q)") are used only when there is no plain column. A "(Q)" placeholder is dropped from the text.
- **Location table as one list** (the LCL layout of the German BASt and others): LOCATION CODE, TYPE / SUBTYPE, ROAD_NUMBER, ROADNAME, FIRST_NAME, SECOND_NAME, AREA_REFERENCE, LINEAR_REFERENCE, NEGATIVE_OFFSET, POSITIVE_OFFSET, EXIT_NUMBER, CID, TABCD.
- **TISA exchange files:** POINTS, ROADS, SEGMENTS, POFFSETS / SOFFSETS, NAMES, ADMINISTRATIVEAREA / OTHERAREAS. The kind of each file comes from its name or its columns. Name ids are resolved across the files.

The column names are those of the commonly published files, taken from memory of ISO 14819-2/-3 and the TISA exchange format. They are not checked against the paid standards, so matching is loose (case, spaces and punctuation are ignored), and the content decides when the names say nothing. Direction and extent follow ISO 14819-1 as commonly implemented:

- The positive direction runs towards the second name of the road or segment.
- The secondary location is found by counting extent steps from the primary location against the direction of travel. For traffic in the positive direction, that means following the negative offsets.

If the official German files use other column names, the importer reports "No column ... found". Send the header line and it will be added.

## Tests (simulated only)

`tmc_db` (tests/fm/test_tmc_db.cpp) uses small synthetic files only:

- German (Latin-1, `;`), English (UTF-8 BOM, `,`, quoted commas), a two-language tab list, a list without a header, and UTF-16.
- An LCL table and a set of five TISA files.
- describe() with and without files, broken offset chains, unknown events and the list's duration type.
- The CSV and text log: no repeats, appending, quoting.
- The folder loader.
- A full path: generated RDS groups (3A + 8A) through the FM generator and receiver, then turned into the expected German line.

No real national file was available. The format detection is checked only against the layouts described above.
