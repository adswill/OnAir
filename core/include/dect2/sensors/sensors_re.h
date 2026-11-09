// Working out an unknown sensor from its packets: the checksum (the Protocol Lab's CRC, sum and XOR search) and which bits hold a value, by comparing with what the user reads off the
// sensor (a thermometer's display) or, with no readings, by a slowly changing field. The result is a line to add to a flex decoder.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 { namespace sensors {

struct ReChecksum { bool found = false; std::string name; int start = 0, width = 0, position = 0; bool sum = false; };
struct ReField {
    int bitOffset = 0, bits = 0;
    int kind = 0;           // 0 binary MSB first, 1 binary low byte first (16 bits), 2 BCD
    double scale = 1, offset = 0;
    double r2 = 0;          // how well the line fits the readings (1: exactly)
    bool isSigned = false;
    std::string text;       // "bits 16-27 x 0.1 - 40 (R2 0.9998)"
    std::string flexGet;    // "get=@16:{12}:temperature" as far as a flex decoder can say it
};

ReChecksum reFindChecksum(const std::vector<std::vector<uint8_t>>& packets);
// packets and readings pair up; at least 4 different readings. Best fit first; only clean scale factors are kept unless allowScale.
std::vector<ReField> reFindFields(const std::vector<std::vector<uint8_t>>& packets, const std::vector<double>& readings, const char* name = "value", int maxResults = 5);
// No readings: fields that change slowly and in one direction across the packets in time order (a temperature ramp)
std::vector<ReField> reFindRamps(const std::vector<std::vector<uint8_t>>& packets, int maxResults = 5);
// "a1 b2 ..." or "a1b2c3" rows to bytes (rows that fail are skipped)
std::vector<std::vector<uint8_t>> reParseHexRows(const std::string& text);

} } // namespace dect2::sensors
