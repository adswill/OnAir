// Radiosonde frame decoders that work on demodulated FSK symbols (no radio): one per sonde type, and the symbol
// streams of single frames for the test signal. The receiver's channels demodulate to hard symbols at symbolRate()
// and push them into every decoder whose rate matches, until one reports frames.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SondeFix {
    std::string type;                   // "RS41", "DFM", "M10", "M20", ...
    std::string subtype;                // e.g. "DFM-17", "RS41-SGP"
    std::string serial;
    int frame = -1;
    bool crcOk = false;
    int corrected = 0;                  // symbols/bytes corrected by the frame's codes
    bool hasPos = false;
    double lat = 0, lon = 0, altM = 0;
    bool hasVel = false;
    double vSpeed = 0, hSpeed = 0, headingDeg = 0;
    int sats = -1;
    bool hasTime = false;
    double unixTime = 0;                // UTC seconds since 1970 from the sonde
    bool hasTemp = false, hasHumidity = false, hasPressure = false;
    double tempC = 0, humidity = 0, pressureHpa = 0;
    double batteryV = -1;
    int burstKillS = -1;                // seconds, -1 = not sent
    std::string note;                   // e.g. "calibrating 12/51"
};

class SondeBitDecoder {
public:
    virtual ~SondeBitDecoder() = default;
    virtual const char* type() const = 0;
    virtual double symbolRate() const = 0;                 // on-air FSK symbols per second (Manchester-coded types: 2 symbols per data bit)
    // hard symbols (0/1) at symbolRate(); the decoder handles Manchester coding, both polarities and frame sync itself
    virtual void push(const uint8_t* sym, size_t n, double timeSec, std::vector<SondeFix>& out) = 0;
    virtual void reset() = 0;
};

std::unique_ptr<SondeBitDecoder> makeDfmDecoder();
std::unique_ptr<SondeBitDecoder> makeM10Decoder();
std::unique_ptr<SondeBitDecoder> makeM20Decoder();

// What the test signal wants a sonde to send
struct SondeTruth {
    std::string serial;
    int frame = 0;
    double lat = 0, lon = 0, altM = 0, vSpeed = 0, hSpeed = 0, headingDeg = 0;
    int sats = 9;
    double unixTime = 0;
    double tempC = -40, humidity = 30, pressureHpa = 200;
    double batteryV = 3.0;
};
// on-air symbols of one frame (0/1 at the type's symbolRate()), and the time between frames
std::vector<uint8_t> dfmSymbols(const SondeTruth& t, int variant);     // variant: 6, 9 or 17
std::vector<uint8_t> m10Symbols(const SondeTruth& t);
std::vector<uint8_t> m20Symbols(const SondeTruth& t);
double sondeFramePeriodS(const std::string& type);

// ---- Added by the decoder owner (the declarations above are unchanged) ----
//
// How the symbol builders use SondeTruth (the decoders return the same quantities in SondeFix):
//  - serial: DFM-09/17 a decimal number ("23038743"); DFM-06 six hex digits ("A1B2C3"); M10 "%1X%02u-%1X-%1u%04u" (e.g. "310-2-11329");
//    M20 "%u%02u-%u-%u%04u" (e.g. "211-4-01234"). A string that does not fit its type is replaced by a number made from it, so every
//    truth produces a valid frame; the decoder then reports that number.
//  - frame: DFM: the index of the 280-bit frame (one per sondeFramePeriodS("DFM") = 0.224 s); the caller counts it up and each call
//    sends the 3 blocks that belong to that index (9 data blocks per second, a configuration cycle of 7 to 12 frames). M10, M20: the
//    counter byte of the frame.
//  - unixTime: UTC. M10 sends GPS time plus its own GPS-UTC offset (18 s); DFM sends UTC; M20 sends a GPS time of week, and
//    18 s are subtracted as dxlAPRS does (a real M20 may differ by the leap second count of its day).
//  - altM: M10 and M20 height above the WGS-84 ellipsoid as the sonde sends it; DFM mode 2 sends the ellipsoid height and the
//    geoid difference (we report mean sea level), DFM mode 3 and 4 send mean sea level.
//  - tempC, humidity, batteryV: DFM temperature, M10 temperature, humidity and battery voltage, M20 temperature and battery voltage.
//    M20 humidity and all pressure values are not sent/decoded (see the unsupported list in the report).
//  - sats: DFM, M10.
std::vector<uint8_t> dfmFrameBits(const SondeTruth& t, int variant);           // the 280 data bits (header 0x45CF, config, 2 data blocks)
std::vector<uint8_t> m10FrameBytes(const SondeTruth& t);                       // 101 bytes with checksum
std::vector<uint8_t> m20FrameBytes(const SondeTruth& t);                       // 70 bytes with checksum
// Frame bytes to a fix. Return true when the checksum is right (out is then filled).
bool m10ParseFrame(const uint8_t* f, size_t n, SondeFix& out);
bool m20ParseFrame(const uint8_t* f, size_t n, SondeFix& out);
uint16_t sondeM10Checksum(const uint8_t* msg, size_t len);
// Hamming(8,4) of the DFM frames: codeword = d0 d1 d2 d3 p0 p1 p2 p3 (d0 first on air), nibble bit 3 = d0.
uint8_t sondeHamming84Encode(uint8_t nibble);
int sondeHamming84Decode(uint8_t code, uint8_t& nibble);                        // 0 clean, 1 one bit corrected, -1 two bits wrong (not correctable)

} // namespace dect2
