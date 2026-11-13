// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Subtitles saved as text next to a recording: an SRT / WebVTT writer, and a collector that turns the text-based teletext subtitle page of a
// service into timed cues (time = PCR of the stream, from the first PCR seen). Bitmap DVB subtitles (EN 300 743) are shown by the player and
// recorded inside the .ts; exporting them as PNG overlays is not done.
#pragma once
#include "dect2/tv/teletext.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct SubCue { double start = 0, end = 0; std::string text; };   // text: lines separated by '\n'

std::string srtTimestamp(double sec);   // 00:00:01,500
std::string vttTimestamp(double sec);   // 00:00:01.500
std::string formatSrt(const std::vector<SubCue>& cues);   // cues with empty text or end <= start are left out; numbered from 1
std::string formatVtt(const std::vector<SubCue>& cues);   // "WEBVTT" header; & < > escaped
bool writeTextFile(const std::string& path, const std::string& text);

// The visible text of a teletext subtitle page: the boxed cells of rows 1..23 (row 0 is the header), one line per row, trailing spaces removed.
std::string ttxSubtitleText(const TtxPage& p);

class TeletextSubtitleCollector {
public:
    void configure(int ttxPid, int page, int pcrPid);   // page 100..899; ttxPid -1 off
    void feed(const uint8_t* pkt188);                   // every packet of the service
    void finish();                                      // closes the cue that is showing
    std::vector<SubCue> cues() const;
    double seconds() const { return t_; }               // stream time since the first PCR
    bool active() const { return pid_ >= 0; }
private:
    int pid_ = -1, page_ = 0, pcrPid_ = -1;
    TeletextDecoder dec_;
    double t_ = 0, firstPcr_ = -1, lastPcr_ = 0, wrap_ = 0;
    uint64_t lastVersion_ = ~0ull;
    std::string shown_;
    double shownAt_ = 0;
    std::vector<SubCue> cues_;
};

} // namespace dect2
