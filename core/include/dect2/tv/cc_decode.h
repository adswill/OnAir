// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CEA-608 and CEA-708 closed captions, from the cc_data triplets of the video user data (ATSC A/53, the same data FFmpeg gives as AV_FRAME_DATA_A53_CC).
// CEA-608: channel CC1 (field 1), pop-on, roll-up and paint-on. CEA-708: service 1, as a text stream (window positions and styles are ignored). Cues are in the form the
// subtitle export takes.
#pragma once
#include "dect2/tv/subtitle_export.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

class CaptionDecoder {
public:
    void feed(const uint8_t* ccData, int nTriplets, double ptsSec);   // triplets: marker/valid/type byte, then two data bytes
    void finish();                                                    // closes what is showing
    const std::vector<SubCue>& cues() const { return cues_; }
    // What is on screen at time t (the picture's presentation time, as fed): a pop-on page from its display until it is replaced or erased;
    // roll-up: the row being typed or the last row, with the row above it (a row closed by a carriage return stays, moved up, until the
    // next one closes, at most 10 s); 708: the line covering t. One format only: CEA-608 once the stream has sent any, else 708.
    // Lines separated by '\n'; empty when nothing shows. Cues more than 30 s older than t are not looked at.
    std::string at(double t) const;
    bool any() const { return pairs_ > 0 || pkts_ > 0; }   // caption data has been seen (even if nothing has been shown yet)
    uint64_t cc608Pairs() const { return pairs_; }
    uint64_t cc708Packets() const { return pkts_; }
    bool has708() const { return text708_; }
    void reset() { *this = CaptionDecoder(); }

private:
    void pair608(uint8_t b1, uint8_t b2, double t);
    void emit(const std::string& text, double start, double end, bool is708 = false);
    void flushRow(double t);
    void packet708(const std::vector<uint8_t>& p, double t);
    void text708(const std::string& s, double t);
    // 608
    std::string nd_, disp_, cur_;                   // non-displayed, displayed (pop-on), current row (roll-up / paint-on)
    enum { Pop, Roll, Paint } mode_ = Pop;
    double dispStart_ = -1, rowStart_ = -1;
    uint8_t last1_ = 0, last2_ = 0;
    // 708
    std::vector<uint8_t> pkt_;
    int pktNeed_ = 0;
    std::string line708_;
    double line708Start_ = -1;
    bool text708_ = false;
    std::vector<SubCue> cues_;
    std::vector<SubCue> shown608_, shown708_;      // the same cues by format, for at()
    std::vector<char> rolled608_;                    // per shown608_ cue: a roll-up row closed by a carriage return (it stays on screen, moved up)
    bool rollEnd_ = false;
    uint64_t pairs_ = 0, pkts_ = 0;
};

}  // namespace dect2
