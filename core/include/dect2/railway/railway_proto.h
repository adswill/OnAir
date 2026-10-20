// Receive-only Chinese railway CIR/FFSK messages, ported from frank-1225/railair-terminal.
// GPL-3.0-or-later. See docs/railway.md for field provenance.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::railway {
enum class Protocol { Klw, Tdcs };
std::string locomotiveModel(unsigned code);
std::string lbjLocomotive(const std::string& raw);
uint16_t crc16(const uint8_t* p, size_t n);

struct Message {
    Protocol protocol = Protocol::Klw;
    int64_t wallTime = 0;
    double timeSec = 0;
    int correctedBits = 0;
    std::vector<uint8_t> raw;
    std::string train, locomotive, locoCode, locoSerial, tailId, action, parameter, clockSync, extra;
    unsigned actionCode = 0, commandReserved = 0;
    int pressureKpa = -1;
    int signalType = 0, semaphoreType = 0, locomotiveRole = 0;
    unsigned speedKmh = 0, weightTonnes = 0, carCount = 0, station = 0, track = 0, actualTrack = 0, driver = 0;
    double length = 0, kilometer = 0;
    bool kilometerValid = false, increasingMileage = false, testFrame = false;
};
bool parseFrame(const std::vector<uint8_t>& frame, Message& out);

// A syndrome can name several <=2-bit errors. Carry all bounded candidates and
// publish only a unique frame passing the application CRC and header checks.
class FrameFinder {
public:
    void feed(bool bit, std::vector<Message>& out);
    uint64_t syncs = 0, crcFailures = 0, rejected = 0, ambiguous = 0;
private:
    struct Candidate { std::vector<uint8_t> bytes; int corrected = 0; };
    uint32_t shift_ = 0, word_ = 0;
    unsigned seen_ = 0, bits_ = 0;
    bool active_ = false, inverted_ = false;
    std::vector<Candidate> candidates_;
};
} // namespace dect2::railway
