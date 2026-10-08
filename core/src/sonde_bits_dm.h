// Helpers shared by the M10 and M20 decoders (Meteomodem): the frame checksum, the differential Manchester line code
// with its sync pattern, GPS time conversions, and a decoder base class that finds the sync and cuts frames out of the
// symbol stream. Internal: not part of the interface in sonde_bits.h.
//
// Sources (facts only, the code is our own):
//  - rs1729/RS demod/mod/m10m20mod.c (frame layout, update_checkM10, header comment with the sync symbols)
//  - rs1729/RS demod/mod/demod_mod.c (read_softbit2p: the line code is read two symbols at a time)
//  - oe5hpm/dxlAPRS src/sondeudp.c (independent: the M10 sync word 0x649F20)
#pragma once
#include "dect2/sonde_bits.h"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace dect2 {
namespace sondebits {

// ---- checksum (rs1729 update_checkM10 / checkM10 / blk_checkM10) ----
uint16_t m10Update(uint16_t c, uint8_t b);
uint16_t m10Check(const uint8_t* msg, size_t len);          // over msg[0..len-1]
uint16_t m10BlockCheck(int len, const uint8_t* msg);        // length byte first, then msg[0..len-3] (M20 essential block)

// ---- time ----
int64_t daysFromCivil(int y, int m, int d);                  // days since 1970-01-01
double civilToUnix(int y, int m, int d, int hh, int mm, double ss);
constexpr double kGpsEpochUnix = 315964800.0;                // 1980-01-06 00:00:00 UTC
void unixToGps(double unixUtc, int leapS, int& week, double& towS);   // GPS week and time of week of a UTC time
double gpsToUnix(int week, double towS, int leapS);
int fixTrimbleWeek(int week);                                // week number roll-over repair, see m10 notes

// ---- NTC thermistor: 1/T = p0 + p1 ln R + p2 ln^2 R + p3 ln^3 R (Steinhart-Hart polyfit used by rs1729) ----
double steinhartTempC(double rOhm, const double p[4]);          // < -270 when R <= 0
double steinhartResistance(double tempC, const double p[4]);    // inverse, by Newton iteration
// M10 and M20 share the thermistor, its three bias ranges and the polyfit
extern const double kM10Poly[4];
extern const double kM10Rs[3], kM10Rp[3];
bool m10NtcTemp(int scale, int adc, double& tempC);            // scale 0..2, 12-bit ADC value; false when out of range or implausible
bool m10NtcAdc(double tempC, int& scale, int& adc);            // inverse: picks a scale whose ADC value is well inside the range

// ---- differential Manchester ("diff-M") line code of M10/M20 ----
// 2 symbols per bit at 9600 symbols/s. A level change at every bit boundary; a bit 1 has a second change in the middle, a bit 0 has none.
// The sync is 8 zero bits followed by 11, three bits that break the rule, and 000 (rs1729: "Sync-Header"), 32 symbols.
extern const char kDmSync[33];                               // '1' = high frequency, as in rs1729's header comment
class DmWriter {
public:
    explicit DmWriter(std::vector<uint8_t>& out) : out_(out) {}
    void bit(int b);
    void byte(uint8_t v) { for (int i = 7; i >= 0; i--) bit((v >> i) & 1); }
    void sync();                                             // the 32 sync symbols (the level is 0 afterwards)
    uint8_t level() const { return level_; }
    void setLevel(uint8_t l) { level_ = l; }
private:
    std::vector<uint8_t>& out_;
    uint8_t level_ = 0;
};

// Finds the sync in a hard symbol stream (either polarity), then reads bytes (2 symbols per bit, bit = the two symbols differ)
// and hands every complete frame to the subclass. Frame start: frame[0] = length byte, frame[1] = type byte, total length[0]+1.
class DmFrameDecoder : public SondeBitDecoder {
public:
    void push(const uint8_t* sym, size_t n, double timeSec, std::vector<SondeFix>& out) override;
    void reset() override;
protected:
    // after the first two bytes: total frame length in bytes (>= 2), or 0 to reject
    virtual size_t frameLength(uint8_t b0, uint8_t b1) const = 0;
    // a complete frame: fill out and return true to report it (also with crcOk false, for the bad-frame count)
    virtual bool handleFrame(const uint8_t* f, size_t n, double timeSec, SondeFix& out) = 0;
    int maxSyncErrors_ = 4;
private:
    struct Cand { uint64_t start; int err; bool inv; size_t need; bool checked; };
    std::vector<uint8_t> buf_;                  // symbols since base_
    uint64_t base_ = 0;                         // absolute index of buf_[0]
    uint64_t total_ = 0;                        // symbols seen
    uint64_t reg_ = 0;                          // last 64 symbols, newest in bit 0
    uint64_t goodEnd_ = 0;                      // end of the last frame that passed its check
    std::vector<Cand> pend_;
    void service(double timeSec, std::vector<SondeFix>& out);
    bool readBytes(const Cand& c, size_t nBytes, std::vector<uint8_t>& dst) const;
};

} // namespace sondebits
} // namespace dect2
