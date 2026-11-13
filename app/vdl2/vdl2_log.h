// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL2 session log (#309): every decoded frame of this session, not only the 400 the screen keeps, each with its UTC date and
// time, and the export of it as CSV, JSON or plain text. Pure data and text (tests/vdl2/test_vdl2_log.cpp), no GUI.
#pragma once
#include "dect2/vdl2/vdl2_tel.h"
#include <cstdint>
#include <deque>
#include <string>

namespace vdl2log {

constexpr size_t kMax = 200000;   // frames kept; the oldest go first

enum class Fmt { Csv, Json, Text };

class Log {
public:
    // Adds the frames of a telemetry snapshot (newest first, as the receiver keeps them) that are newer than the last one seen.
    // Returns how many were added.
    size_t feed(const dect2::Vdl2Telemetry& t);
    size_t size() const { return rows_.size(); }
    void clear() { rows_.clear(); }
    // Every frame, oldest first, one record each; the time is always UTC "YYYY-MM-DD HH:MM:SS"
    std::string render(Fmt f) const;
    const std::deque<dect2::Vdl2Message>& rows() const { return rows_; }
private:
    std::deque<dect2::Vdl2Message> rows_;
    uint64_t last_ = 0;
};

std::string utc(int64_t wall);              // "2026-10-25 14:03:09" (UTC), "" when the time is not set
const char* fmtExt(Fmt f);                  // ".csv", ".json", ".txt"

}  // namespace vdl2log
