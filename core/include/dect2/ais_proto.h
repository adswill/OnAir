// AIS (ITU-R M.1371-5) message layer: bit fields, the HDLC frame (flags, stuffing, NRZI, FCS), message decoding and !AIVDM sentences.
// Field layouts follow the gpsd AIVDM document (gpsd.gitlab.io/gpsd/AIVDM.html); the frame is plain HDLC (ISO 13239 / ITU-T X.25 FCS).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace ais {

using Bits = std::vector<uint8_t>;   // one bit per element, in transmission order

// ---- bit fields: multi-bit fields are sent most significant bit first
uint32_t getU(const Bits& b, size_t pos, size_t len);          // bits past the end read as 0
int32_t getS(const Bits& b, size_t pos, size_t len);           // two's complement
std::string getText(const Bits& b, size_t pos, size_t len);    // 6-bit text, trailing '@' and spaces removed
void putU(Bits& b, size_t pos, size_t len, uint32_t v);        // grows b with zeros when needed
void putS(Bits& b, size_t pos, size_t len, int32_t v);
void putText(Bits& b, size_t pos, size_t len, const std::string& s);   // upper case, padded with '@'

// ---- frame
// FCS: CRC-16-CCITT run bit by bit in transmission order (the register is the reflected one, as in HDLC), start value 0xFFFF.
uint16_t fcsRegister(const uint8_t* bits, size_t n);
constexpr uint16_t kFcsGood = 0xF0B8;        // register after payload + FCS when nothing is wrong
void appendFcs(Bits& payload);               // adds the 16 FCS bits (one's complement of the register, low bit first)
bool fcsOk(const Bits& frame);               // frame = payload + 16 FCS bits
Bits stuff(const Bits& in);                  // a 0 after every five 1s
bool destuff(const Bits& in, Bits& out);     // false when six 1s in a row appear (flag or abort inside the frame)
Bits nrziEncode(const Bits& data, int startLevel = 1);   // 0 = level change, 1 = no change (the level is the line bit that is modulated)
Bits nrziDecode(const Bits& line, int prevLevel = 1);
// The bits a transmitter puts on the air for one message: 24 bits of training sequence (alternating 0 and 1), start flag, stuffed payload + FCS,
// end flag, 8 bits while the transmitter ramps down. The training sequence alternates on the line; everything after it is NRZI coded. 'payload' is the message without FCS.
Bits burstLineBits(const Bits& payload);

// Frames found in a bit stream that is already NRZI decoded: every stretch between two flags that is long enough, destuffed.
// A frame is reported good when its FCS is right. 'bad' counts stretches of plausible length whose FCS failed.
struct HdlcResult { std::vector<Bits> good; int bad = 0; };
HdlcResult hdlcFrames(const Bits& decoded);

// ---- messages
struct AisMsg {
    bool valid = false;
    int type = 0, repeat = 0;
    uint32_t mmsi = 0;
    int cls = 5;                              // AisClass of the sender
    // position
    bool hasPos = false; double lat = 0, lon = 0;
    float sog = -1, cog = -1; int heading = -1;
    bool hasRot = false; float rotDegMin = 0;
    int navStatus = -1;
    int second = 60;                          // UTC second of the position report, 60 = not available
    // static and voyage
    std::string name, callsign, destination;
    uint32_t imo = 0;
    int shipType = -1;
    int dimA = 0, dimB = 0, dimC = 0, dimD = 0;
    int etaMonth = 0, etaDay = 0, etaHour = 24, etaMin = 60;
    float draughtM = 0;
    int epfd = 0;
    int partNo = -1;                          // message 24: 0 = A, 1 = B
    std::string vendor;
    // aid to navigation
    int aidType = -1; bool offPosition = false, virtualAid = false;
    // search and rescue aircraft
    int altitudeM = -1;
    // base station time
    int year = 0, month = 0, day = 0, hour = 24, minute = 60, sec = 60;
    // text and binary
    std::string text;
    int dac = -1, fi = -1;
    uint32_t destMmsi = 0;
    size_t bits = 0;                          // payload length
};

// Decodes a message from its payload bits (without FCS). valid is false when the type is not one of the decoded ones; the header
// (type, repeat, mmsi) is still filled in. Messages whose length does not fit their type are not valid.
bool decodeMessage(const Bits& payload, AisMsg& m);

// ---- NMEA 0183 !AIVDM sentences
std::string armour(const Bits& payload, int& fillBits);            // 6-bit ASCII, fill bits to the next multiple of six
Bits unarmour(const std::string& chars, int fillBits);
uint8_t nmeaChecksum(const std::string& body);                      // XOR of the characters between '!' (or '$') and '*'
// One or more sentences (60 characters of payload per sentence, as in the gpsd examples). 'seqId' is the sequential message id of multi-sentence
// messages (0 - 9); channel is 'A' or 'B'.
std::vector<std::string> toNmea(const Bits& payload, char channel, int seqId = 0);
// A sentence: payload characters, fill bits, fragment count and number. False when the checksum or the layout is wrong.
bool parseNmea(const std::string& sentence, std::string& chars, int& fillBits, int& fragCount, int& fragNo, char* channel = nullptr);

// ---- small helpers shared with the generator
double rateOfTurn(int raw);                                         // degrees per minute from the 8-bit field (127 = more than 720), NaN when not available
const char* shipTypeText(int t);
int classOfMmsi(uint32_t mmsi, int type);

} // namespace ais
} // namespace dect2
