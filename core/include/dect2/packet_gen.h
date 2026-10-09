// APRS / Packet test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makePacketSynth(): a few made-up APRS stations near 25.2 N 55.3 E send AX.25 frames on one 25 kHz FM channel, in an endless cycle:
// positions (plain, compressed, Mic-E), an object, an item, a message and its acknowledgement, a weather report and a status,
// at 1200 baud (Bell 202 AFSK), and some frames at 9600 baud (G3RUH). The carrier sits at the mode's tune offset below the radio's
// centre (plus cfoHz); the cycle is packetGenCycle() with its length packetGenCycleSec().
//
// SynthConfig: snrDb = carrier to noise in 25 kHz; cfoHz = carrier offset; modeOpt[0] = 0 both speeds one after the other, 1 only
// 1200 baud, 2 only 9600 baud.
#pragma once
#include "mode_synth.h"
#include "packet_ax25.h"
#include <memory>
#include <vector>

namespace dect2 {

struct PacketGenFrame {
    ax25::Frame frame;
    int baud = 1200;
};

// The frames of one cycle in the order they are sent.
std::vector<PacketGenFrame> packetGenCycle(int mode);
// Length of one cycle in seconds, silence before the first frame included.
double packetGenCycleSec(int mode);

std::unique_ptr<ModeSynth> makePacketSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
