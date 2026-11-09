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
    uint64_t cc608Pairs() const { return pairs_; }
    uint64_t cc708Packets() const { return pkts_; }
    bool has708() const { return text708_; }
    void reset() { *this = CaptionDecoder(); }

private:
    void pair608(uint8_t b1, uint8_t b2, double t);
    void emit(const std::string& text, double start, double end);
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
    uint64_t pairs_ = 0, pkts_ = 0;
};

}  // namespace dect2
