// DRM signalling and message formats: FAC (clause 6.3), SDC data entities (6.4), the text message (6.5) and the audio super frame (5.2 to 5.4).
// Bits are one per byte (0 or 1), most significant first, like in drm_fec.h.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 { namespace drm {

// ---------------------------------------------------------------- FAC

struct FacService {
    uint32_t id = 0;            // 24 bit service identifier
    int shortId = 0;
    bool audioCa = false, dataCa = false;
    int language = 0;           // Table 18
    bool data = false;          // Audio/Data flag
    int descriptor = 0;         // programme type (audio, Table 19) or application identifier (data); 30 = warning/alarm active, 31 = skip this multiplex
};

struct FacInfo {
    bool enhancement = false;   // Base/Enhancement flag
    int identity = 0;           // 0 first frame of the super frame (SDC AFS index valid), 1 intermediate, 2 last, 3 first (AFS index invalid)
    bool modeE = false;         // RM flag: robustness mode E, two sets of service parameters
    int occupancy = 0;          // Table 48
    int interleaver = 0;        // 0: long (2 s, mode E 600 ms), 1: short (400 ms)
    int mscMode = 0;            // RM flag 0: 0 = 64-QAM, 3 = 16-QAM; RM flag 1: 0 = 16-QAM, 3 = 4-QAM
    int sdcMode = 0;            // RM flag 0: 0 = 16-QAM, 1 = 4-QAM; RM flag 1: 0 = 4-QAM rate 0.5, 1 = 4-QAM rate 0.25
    int numServicesCode = 0;    // Table in clause 6.3.3
    int reconfig = 0;
    int toggle = 0;
    FacService svc[2];
    int nSvc = 1;               // sets of service parameters carried (2 in mode E)
};

int facInfoBits(bool modeE);                                  // 72 or 116
void facBuild(const FacInfo& f, uint8_t* bits);               // fills facInfoBits() bits, CRC included
bool facParse(const uint8_t* bits, bool modeE, FacInfo& f);   // false when the CRC is wrong or the length disagrees; bits are the descrambled FAC block
int facAudioServices(int numServicesCode);                    // -1 for the reserved codes
int facDataServices(int numServicesCode);
const char* languageName(int code);                           // Table 18
const char* programmeTypeName(int code);                      // Table 19
const char* mscModeName(bool modeE, int mscMode);
const char* occupancyName(bool modeE, int occ);

// ---------------------------------------------------------------- SDC

struct SdcStream { int lenA = 0, lenB = 0; };                 // bytes of the logical frame in part A and part B
struct SdcMux { bool present = false; int protA = 0, protB = 0; int nStreams = 0; SdcStream stream[4]; };
struct SdcAudio {
    bool present = false;
    int shortId = 0, streamId = 0;
    int coding = 0;             // 0 AAC, 3 xHE-AAC
    int sbr = 0;                // AAC only
    int mode = 0;               // AAC: 0 mono, 1 parametric stereo, 2 stereo; xHE-AAC: 0 mono, 2 stereo
    int rateCode = 0;           // AAC: 1 12 kHz, 3 24 kHz, 5 48 kHz; xHE-AAC: 2 16, 3 19.2, 4 24, 5 32, 6 38.4, 7 48 kHz
    bool text = false;
    bool enhancement = false;
    int mps = 0;                // MPEG Surround mode
    std::vector<uint8_t> config;   // xHE-AAC static config
    int rateHz() const;
};
struct SdcLabel { bool present = false; int shortId = 0; int textControl = 0; std::string text; };
struct SdcLang { bool present = false; int shortId = 0; std::string language, country; };   // ISO 639-2 and ISO 3166 codes
struct SdcTime { bool present = false; int mjd = 0, hour = 0, minute = 0; bool hasOffset = false; int offsetHalfHours = 0; };
struct SdcApp { bool present = false; int shortId = 0, streamId = 0; bool packet = false; int domain = 0; };

struct SdcInfo {
    int afsIndex = 0;
    SdcMux mux;                 // the current configuration (version flag 0)
    SdcMux muxNext;             // a configuration announced for later (version flag 1)
    SdcAudio audio[4];          // by short id
    SdcLabel label[4];
    SdcLang lang[4];
    SdcApp app[4];
    SdcTime time;
    std::vector<int> otherTypes;   // data entity types that are present but not interpreted
    int entities = 0;
};

// data: the SDC data field (bytes). Returns the number of entities read.
int sdcParse(const uint8_t* data, int bytes, SdcInfo& out);
// Writing: append one entity to a data field
void sdcPutEntity(std::vector<uint8_t>& field, int type, int version, const std::vector<uint8_t>& body4plus);   // body: first nibble in the high bits of body[0], then whole bytes
std::vector<uint8_t> sdcEntityMux(const SdcMux& m);
std::vector<uint8_t> sdcEntityAudio(const SdcAudio& a);
std::vector<uint8_t> sdcEntityLabel(int shortId, const std::string& text);
std::vector<uint8_t> sdcEntityLang(int shortId, const std::string& lang, const std::string& country);
std::vector<uint8_t> sdcEntityTime(const SdcTime& t);
// The full SDC block as it is coded: AFS index (4 bits), data field padded with 0x00 to `fieldBytes`, CRC16 over 8 bit AFS index + data field.
// bits: 4 + 8 * fieldBytes + 16 bits.
void sdcBuildBits(int afsIndex, const std::vector<uint8_t>& field, int fieldBytes, std::vector<uint8_t>& bits);
bool sdcCheckBits(const uint8_t* bits, int fieldBytes, int& afsIndex);            // CRC check of the decoded block (descrambled), bits >= 4 + 8 * fieldBytes + 16

// Modified Julian Date <-> calendar date (clause 6.4.3.9 uses MJD)
void mjdToDate(int mjd, int& y, int& m, int& d);
int dateToMjd(int y, int m, int d);

// ---------------------------------------------------------------- text message (clause 6.5): one 4 byte unit per logical frame of an audio stream

class TextDecoder {
public:
    void reset();
    // Feed the 4 bytes of one logical frame. Returns true when the message changed.
    bool feed(const uint8_t* four);
    const std::string& text() const { return text_; }
    uint64_t segmentsOk() const { return ok_; }
    uint64_t segmentsBad() const { return bad_; }
private:
    void finishSegment();
    std::vector<uint8_t> seg_;
    int need_ = 0;                // bytes expected for the segment being received (0: waiting for a start marker)
    bool toggle_ = false, haveToggle_ = false;
    std::string parts_[8];
    uint8_t got_ = 0;             // bit mask of the segments received for the current message
    int last_ = -1;               // index of the last segment, once seen
    std::string text_;
    bool changed_ = false;
    uint64_t ok_ = 0, bad_ = 0;
};

// The units a transmitter sends for a message: one 4 byte unit per logical frame, the message repeated; call next() once per frame.
class TextEncoder {
public:
    void set(const std::string& message);
    void next(uint8_t* four);
private:
    std::vector<uint8_t> stream_;   // markers and pieces of one repetition of the message
    size_t pos_ = 0;
    bool toggle_ = false;
};

// ---------------------------------------------------------------- audio super frame, AAC and AAC + SBR (clause 5.4), EEP and UEP

struct AacSuperFrame {
    int numFrames = 0;
    bool headerOk = false;                     // borders increase and stay inside the payload
    std::vector<std::vector<uint8_t>> frames;  // the audio frames, higher and lower protected bytes joined
    std::vector<uint8_t> crc;                  // the 8 bit CRC of each frame (over the first bits of the frame, see the decoder)
    int hpBytes = 0;                           // bytes of each frame in the higher protected part
};
int aacNumFrames(bool modeE, int rateHz);      // 5 or 10, 0 when the rate is not allowed
int aacHeaderBytes(int numFrames);             // frame borders (12 bits each, 4 padding bits for 10 frames)
// lf: the logical frame of the stream (part A then part B), lenA: bytes in part A. The 4 bytes of the text message are not part of lf here.
bool aacSuperFrameParse(const uint8_t* lf, int len, int lenA, int numFrames, AacSuperFrame& out);
// Build: frames with their CRC bytes (hpBytes bytes of every frame go to the higher protected part). Returns the logical frame (len bytes, zero padded).
bool aacSuperFrameBuild(const std::vector<std::vector<uint8_t>>& frames, const std::vector<uint8_t>& crc, int hpBytes, int len, std::vector<uint8_t>& lf);

}} // namespace dect2::drm
