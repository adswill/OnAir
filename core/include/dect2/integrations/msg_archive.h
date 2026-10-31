// "Save decoded messages": every decoded message of every mode, written to a folder you choose, one file per day per mode, as JSON Lines (default),
// JSON (one array per file, valid at all times) or CSV. It is a sink of the Integrations hub (one hook, Engine::setPublisher), so every mode that raises
// an automation event is covered. Off until the user ticks it and picks a folder. Docs: docs/INTEGRATIONS.md.
#pragma once
#include "dect2/automation/event.h"
#include <cstdint>
#include <string>

namespace dect2::integrations {

enum class ArchiveFormat { JsonLines = 0, Json = 1, Csv = 2 };

struct ArchiveSettings {
    bool enabled = false;
    std::string folder;                       // created when missing
    ArchiveFormat format = ArchiveFormat::JsonLines;
};

const char* archiveExt(ArchiveFormat f);                          // "jsonl", "json", "csv"
std::string archiveIso(double unixSeconds);                        // 2026-10-31T12:34:56Z
// File name for an event: onair-<mode>-<YYYY-MM-DD>.<ext>; the mode is made filename-safe, the day is the UTC day of the event.
std::string archiveFileName(const automation::Event& ev, ArchiveFormat f);
// One record as JSON: timestamp, mode, type, source, freq_hz, quality, quality_field, text, fields{...}
std::string archiveRecordJson(const automation::Event& ev);
// The CSV header line and one CSV row (RFC 4180 quoting; no trailing newline).
std::string archiveCsvHeader();
std::string archiveCsvRow(const automation::Event& ev);
// Appends the event to its day file in settings.folder. Returns an empty string when written, else why not.
std::string archiveWrite(const ArchiveSettings& s, const automation::Event& ev);

}  // namespace dect2::integrations
