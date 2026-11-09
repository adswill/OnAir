// ATSC PSIP beyond the channel table (A/65): MGT, the source_id of the channels, EIT titles, ETT descriptions, the system time. Text is ATSC multiple-string text in plain
// (compression 0) form: Latin-1 (mode 0) or UTF-16 (mode 0x3F). Huffman-compressed titles (Annex C tables) are reported as compressed and left blank.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dect2 {
namespace psip {

struct Event { int sourceId = 0, eventId = 0; int64_t start = 0; int duration = 0; std::string title; bool compressed = false; };

struct State {
    std::set<int> eitPids, ettPids;
    std::map<int, int> sourceToProgram;      // source_id -> program_number
    int gpsUtcOffset = 18;                   // leap seconds; updated from the STT
    int64_t systemTimeUtc = 0;
    std::map<uint32_t, std::string> ett;     // ETM_id -> extended text
    uint64_t mgt = 0, eit = 0, ettCount = 0, stt = 0, compressedTitles = 0;
};

// each returns true when the section was accepted. CRC checked. len includes the CRC.
bool parseMgt(const uint8_t* s, int len, State& st);
bool parseVctSources(const uint8_t* s, int len, State& st);
bool parseEit(const uint8_t* s, int len, State& st, std::vector<Event>& out);
bool parseEtt(const uint8_t* s, int len, State& st, uint32_t* etmId, std::string* text);
bool parseStt(const uint8_t* s, int len, State& st);
// multiple string text (A/65 clause 6.10): the first string of the first language.  Returns the number of bytes consumed. *compressed set if Huffman coded.
std::string mss(const uint8_t* p, int len, bool* compressed);
inline int64_t gpsToUtc(int64_t gps, int offset) { return gps + 315964800 - offset; }

}  // namespace psip
}  // namespace dect2
