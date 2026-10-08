// Iridium frame layer (no radio): classify and decode the bits of one demodulated burst, assemble pager messages,
// and build burst bits for the test signal.
// Facts: iridium-toolkit (github.com/muccc/iridium-toolkit: bitsparser.py, bch.py, FORMAT.md, tests/test_parser.py) and gr-iridium
// (lib/iridium_qpsk_demod_impl.cc for the symbol to bit mapping).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// One demodulated burst. bits: one bit per byte (0/1) after DQPSK demodulation, beginning with the first bit AFTER the
// unique word, in the order iridium-toolkit's input lists them (the bit string of its "RAW:" lines after the unique word).
// That is gr-iridium's order: per symbol the delta code d = (s - s_prev) mod 4 (s = counter-clockwise quadrant, Q1 = 0)
// goes through {0->0, 1->2, 2->3, 3->1} and is written as (code bit 1, code bit 0); see iridiumStepsToBits. The decoder swaps
// the two bits of every symbol first, as the toolkit does, so everything below works in the toolkit's own order.
struct IridiumBurstBits {
    std::vector<uint8_t> bits;
    bool downlink = true;
    double freqHz = 0, timeSec = 0, levelDb = 0, confidence = 0;
    double refUnixTime = 0;             // wall clock the Iridium time of IBC frames is read against; 0 = this computer's clock
};

enum class IridiumType { Unknown, IRA, IBC, ISY, ITL, MSG, IDA, IIP, IIQ, IIR, IIU, IMS, Voice };

struct IridiumFrame {
    IridiumType type = IridiumType::Unknown;
    std::string typeName;               // "IRA", "IBC", ... ("VOC" for voice: counted, never decoded)
    bool ok = false;                    // the error-correcting codes of the frame checked out
    int corrected = 0;                  // bits corrected by the BCH codes
    int satId = -1, beamId = -1;
    bool hasPosition = false;           // IRA: position as the ring alert carries it (geocentric latitude, like the toolkit)
    double lat = 0, lon = 0, altKm = 0;
    int paged = 0;                      // IRA: number of TMSIs paged (the identities themselves are not kept)
    bool hasTime = false;               // IBC/ITL: time as UTC seconds since 1970
    double unixTime = 0;
    int ric = -1, msgSeq = -1, block = -1, blocks = -1;   // MSG (pager): block = part number (from 0), blocks = number of parts
    std::string msgText;                // MSG: text of this part (control characters shown as '?', ETX padding removed)
    std::string hex;                    // payload as hex for data/unknown frames (empty for voice); MSG: the toolkit's "msg:" field
    // added by the frame layer
    int lcwFt = -1;                     // frame type of the link control word (0 voice, 1 IP, 2 data, 3.., 7 sync), -1 without LCW
    bool crcOk = false;                 // IDA/IIP: the frame check sequence matches
    int idaCtr = -1, idaLen = -1;       // IDA: counter and number of payload bytes
    bool idaCont = false;               // IDA: another part follows
    int msgFmt = -1;                    // MSG: 5 ASCII, 3 BCD
    int msgChecksum = -1;               // MSG: 7-bit checksum of the whole text, as sent in every part
    std::string msgRaw;                 // MSG: the 7-bit characters of this part as sent
    int lbfc = -1;                      // IBC: raw frame counter ("Iridium time") when this frame carries it
    bool uplink = false;
};

IridiumFrame decodeIridiumBurst(const IridiumBurstBits& b);

struct IridiumPagerMessage {
    int ric = -1, seq = -1;
    std::string text;
    bool complete = false;              // all parts received and the checksum of the whole text matches
    double timeSec = 0;
};

// Joins the parts of pager messages; returns the messages that completed (or timed out incomplete) with this frame.
class IridiumMsgAssembler {
public:
    IridiumMsgAssembler();
    ~IridiumMsgAssembler();
    std::vector<IridiumPagerMessage> feed(const IridiumFrame& f, double timeSec);
    // Gives up on messages that have waited longer than 2000 s (as the toolkit does) and returns them as incomplete.
    std::vector<IridiumPagerMessage> expire(double timeSec);
    void reset();
private:
    struct State;
    std::unique_ptr<State> s_;
};

// Builders for the test signal: bits after the unique word, same convention as IridiumBurstBits::bits (downlink).
// lat is the geocentric latitude (what the ring alert carries; the decoder returns the same kind).
std::vector<uint8_t> iridiumBuildIra(int satId, int beamId, double lat, double lon, double altKm, const std::vector<uint32_t>& tmsis);
std::vector<uint8_t> iridiumBuildIbc(int satId, int beamId, double unixTime);
std::vector<uint8_t> iridiumBuildIsy();
// One part of a pager message with the checksum computed over textPart: right for a one-part message (block 0 of 1).
// For several parts use iridiumBuildMsgParts, which splits the text and puts the checksum of the whole text in every part.
std::vector<uint8_t> iridiumBuildMsg(int ric, int seq, int block, int blocks, const std::string& textPart);
std::vector<uint8_t> iridiumBuildVoiceLike(uint32_t seed);   // random payload that classifies as voice

// ---- additions of the frame layer ----
// All parts of one pager message (at most 3 parts of at most 60 characters; longer text is cut), in the order to send them.
std::vector<std::vector<uint8_t>> iridiumBuildMsgParts(int ric, int seq, const std::string& text);
// Numeric pager message (format 3, BCD); digits 0 to 9, at most about 100.
std::vector<uint8_t> iridiumBuildMsgBcd(int ric, int seq, const std::string& digits);
// Ring alert from the raw fields: position in units of 4 km (12 bit signed each), interval (48 in the live system), the two
// unknown bits, the broadcast sub-band. tmsis are the paged identities, msc_id is carried per page. A ring alert is always
// 864 bits (432 symbols): pages (up to 12) plus an end page plus filler.
struct IridiumPage { uint32_t tmsi = 0; int mscId = 2; };
std::vector<uint8_t> iridiumBuildIraRaw(int satId, int beamId, int x, int y, int z, int interval, int ts, int eip, int bcSubband,
                                        const std::vector<IridiumPage>& pages);
// Short burst data frame (IDA): payload up to 20 bytes, counter 0..7.
std::vector<uint8_t> iridiumBuildIda(const std::vector<uint8_t>& payload, int ctr, bool cont);
// Link control word: 46 bits in the toolkit's order (ft 3 bits, lcw2 6 bits, lcw3 21 bits), then 312 payload bits. Returns the 358 bits.
std::vector<uint8_t> iridiumBuildLcwFrame(int ft, int lcw2, int lcw3, const std::vector<uint8_t>& payload312);

// Iridium time. A frame counter of 32 bits counts 90 ms; the epoch changed twice (1996/2007 first, 2014-05-11 14:23:55 until
// 2026-01-14, 2025-02-14 18:14:17 since). The era closest to refUnix wins (refUnix <= 0: this computer's clock).
double iridiumTimeFromLbfc(uint32_t lbfc, double refUnix = 0);
uint32_t iridiumLbfcFromTime(double unixTime);   // era that was active at unixTime

// Symbol mapping of gr-iridium: delta step 0..3 (counter-clockwise quarter turns between two symbols) <-> the two bits of the RAW order.
void iridiumStepsToBits(const std::vector<uint8_t>& steps, std::vector<uint8_t>& bits);
void iridiumBitsToSteps(const std::vector<uint8_t>& bits, std::vector<uint8_t>& steps);
// The 24 bits (12 symbols) of the unique word as the toolkit lists them (identical in RAW order), downlink or uplink.
const std::vector<uint8_t>& iridiumUniqueWordBits(bool downlink);

} // namespace dect2
