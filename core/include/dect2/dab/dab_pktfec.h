// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB packet mode forward error correction (ETSI EN 300 401 clause 5.3.5): a 188 x 12 table of packets, RS(204,188) over the rows, the parity carried in nine FEC packets (address
// 1022) after each packet set. A receiver with this takes the set out of the stream, corrects it and hands on the packets; without FEC the address 1022 packets are simply dropped.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {

class DabPacketFec {
public:
    // The bytes of the sub-channel in order. Corrected packets (with the FEC packets removed) are appended to out. Until the first FEC set is found, nothing comes out.
    void feed(const uint8_t* p, size_t n, std::vector<uint8_t>& out);
    void reset();
    uint64_t sets() const { return sets_; }
    uint64_t rowsFixed() const { return fixed_; }
    uint64_t rowsBad() const { return bad_; }
    bool synced() const { return synced_; }
private:
    void takeSet(const uint8_t* data2256, const uint8_t* fec216, std::vector<uint8_t>& out);
    std::vector<uint8_t> buf_;
    bool synced_ = false;
    uint64_t sets_ = 0, fixed_ = 0, bad_ = 0;
};

// the writer, for tests and the test transmitter: one packet set of 2256 bytes -> the set followed by its nine FEC packets
std::vector<uint8_t> dabPacketFecEncode(const uint8_t* data2256);

}  // namespace dect2
