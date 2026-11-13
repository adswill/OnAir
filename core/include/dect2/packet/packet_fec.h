// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Packet radio with forward error correction, alongside plain AX.25 on the same demodulator: FX.25 (TAPR, Stensby) and IL2P (N. Carrillo KK4HEJ, draft 0.6). Tags, parity sizes and the
// FX.25 block layout were checked against Dire Wolf's source (GPL-2+), IL2P against the v0.6 specification; the scrambler initial values come from Dire Wolf.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {

class PacketFecRx {
public:
    using Sink = std::function<void(const std::vector<uint8_t>& ax25NoFcs, int repaired, bool il2p)>;
    PacketFecRx();
    ~PacketFecRx();
    void setSink(Sink s);
    void reset();
    // one line bit (before NRZI decoding): 1200 baud AFSK gives levels directly, IL2P is not NRZI
    void lineBit(int level, bool il2pActive);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

namespace packetfec {
// the codeblocks, for the test signal
bool fx25Tag(int ctag, uint64_t& value, int& nBytes, int& kBytes);       // false: undefined tag
int rsEncodeParity(const uint8_t* data, int n, int nroots, int fcr, uint8_t* parityOut);   // the Karn field 0x11D, prim 1
std::vector<uint8_t> il2pDescramble(const uint8_t* in, size_t n);
std::vector<uint8_t> il2pScramble(const uint8_t* in, size_t n);
}

} // namespace dect2
