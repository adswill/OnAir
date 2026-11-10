// DRM Alternative Frequency Signalling read from the SDC (ES 201 980 clause 6.4.3): type 3 other frequencies of this multiplex, type 4 their schedules, type 11 the same
// programme on another system (DRM, AM, FM, DAB). And a station book: what was heard, kept between sessions.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct DrmAltFreq {
    double khz = 0;              // tune here; 0 when only a DAB channel name is known (dabChannel)
    int kind = 0;                // 0 DRM, 1 AM, 2 FM, 3 DAB
    bool modeE = false;          // DRM+
    bool synchronous = false;    // type 3: the same multiplex with the same timing, a switch can be seamless
    bool enhancement = false;    // type 3: the enhancement layer
    int shortIdMask = 15;        // type 3: which services of the tuned multiplex (bit n: short id n) are there; 15: all
    int regionId = 0, scheduleId = 0;
    bool sameService = true;     // type 11: the same programme (false: an alternative one)
    int shortId = -1;            // type 11: the service of the tuned multiplex it applies to (-1: type 3, the multiplex)
    bool otherIdValid = false; uint32_t otherId = 0;   // type 11: DRM service id, ECC+PI, PI, DAB service id
    std::string dabChannel;      // "12C"
    bool hasSchedule = false;    // a schedule of that id is known
    bool activeNow = true;       // by its schedule and the multiplex time; true when no schedule
};

struct DrmAfsSchedule { int id = 0, dayCode = 0, startMin = 0, durationMin = 0; };

class DrmAfs {
public:
    void clear();
    // the SDC data field of one block: list entities are merged; a change of a type's version flag starts that type's list again
    void feed(const uint8_t* data, int bytes);
    const std::vector<DrmAltFreq>& altFreqs() const { return afs_; }
    const std::vector<DrmAfsSchedule>& schedules() const { return sched_; }
    // updates activeNow, from the multiplex UTC time (year 0: time unknown, everything stays active)
    void applySchedule(int year, int month, int day, int hour, int minute);
private:
    std::vector<DrmAltFreq> afs_;
    std::vector<DrmAfsSchedule> sched_;
    int ver3_ = -1, ver4_ = -1, ver11_ = -1;
};

bool drmScheduleActive(const DrmAfsSchedule& s, int weekday0Monday, int minuteOfDay);   // 0 Monday .. 6 Sunday; handles durations longer than a day or a week
std::string drmDabChannelName(int code);                                                // 64 -> "5A", 101 -> "13F", "" out of range

// Entities for the test transmitter
std::vector<uint8_t> drmAfsEntityBody3(bool synchronous, const std::vector<uint32_t>& kHz, int regionId = 0, int scheduleId = 0, int restrictMask = -1);
std::vector<uint8_t> drmAfsEntityBody4(int scheduleId, int dayCode, int startMin, int durationMin);
std::vector<uint8_t> drmAfsEntityBody11(int shortId, int systemId, bool sameService, uint32_t otherId, int otherBits, const std::vector<uint32_t>& freqCodes, int freqBits);

// A persistent list of the DRM stations heard, one per service id
struct DrmBookEntry {
    uint32_t id = 0;
    std::string label, language, country, programme, text;
    std::vector<double> heardKhz;        // tuned frequencies it was heard on
    std::vector<double> altKhz;          // other frequencies it announced
    bool audio = true;
    float bestSnr = 0;
    int64_t lastHeard = 0;               // wall clock
};

class DrmStationBook {
public:
    void note(const DrmBookEntry& e);    // merge by id; lists are united, the rest takes the newest non-empty value
    const std::map<uint32_t, DrmBookEntry>& entries() const { return e_; }
    std::vector<DrmBookEntry> newestFirst() const;
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void clear() { e_.clear(); }
private:
    std::map<uint32_t, DrmBookEntry> e_;
};

} // namespace dect2
