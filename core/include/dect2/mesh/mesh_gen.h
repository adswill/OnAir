// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh (LoRa) test signal: a small Meshtastic network and a small MeshCore network around Dubai, built packet by packet as the
// firmware builds them (mesh_proto.h) and modulated as LoRa frames (mesh_lora.h), time-compressed so that a minute shows everything.
//
// EU (default): 6 Meshtastic nodes on LongFast (SF11, 250 kHz, 4/5) at 869.525 MHz and 3 MeshCore nodes (SF8, 62.5 kHz, 4/8) at
// 869.618 MHz. Every 90 s: NODEINFO, POSITION, device TELEMETRY and environment telemetry of every Meshtastic node, a chat on the
// default channel with replies (some relayed: hop start 3, hop limit 2), a traceroute and its reply, a routing ack; MeshCore adverts
// with names and positions, messages on the Public channel, one relayed by the repeater (path = the repeater's hash).
// Frames on one frequency never overlap; the two frequencies do. Each node has its own crystal offset (up to +-1.5 kHz) and an SNR
// that varies by +-1.5 dB from packet to packet.
// The scene is placed for a receiver tuned to the region's Meshtastic LongFast frequency (meshTuning().defMhz in EU): LongFast at
// -tuneOffsetHz from 0 Hz in the samples, MeshCore 93 kHz above it. US: LongFast at 906.875 MHz, MeshCore at 910.525 MHz (SF7,
// 62.5 kHz), 3.65 MHz above: it is left out when the sample rate cannot hold it (it needs about 8 Msps).
//
// SynthConfig: snrDb = SNR of the strongest node (in its LoRa bandwidth); cfoHz added to every node; sroPpm = the transmitters'
// clock error (their frames last that much longer).
//   modeOpt[0]  protocols: 1 Meshtastic, 2 MeshCore, 4 LoRa APRS, 8 MeshCom, 16 LoRaWAN (0 = all)
//   modeOpt[1]  region: 0 EU, 1 US, 2 the 70 cm scene: MeshCom EU (433.175 MHz, SF11, 250 kHz, 4/6, sync 0x2B) at -tuneOffsetHz and
//               LoRa APRS EU (433.775 MHz, SF12, 125 kHz, 4/5, sync 0x12) 600 kHz above it, for a receiver tuned to 433.175 MHz:
//               3 LoRa APRS stations (position, iGate position, weather via a digipeater, message, object, status with UTF-8,
//               telemetry) and 2 MeshCom nodes (positions with battery, one relayed; texts to all, to a call and to group 262)
//               3 LoRaWAN EU868, 4 LoRaWAN US915: six end devices (The Things Network, Helium, Orange, a private network, two join
//               requests from Dragino and Milesight DevEUIs) on 868.1 / 868.3 / 868.5 MHz (US: 903.9 / 904.1 / 904.3) at SF7 - SF12
//               (US: SF7 - SF10), 125 kHz, sync 0x34, for a receiver tuned to the first channel; frame counters go on from cycle to cycle
//               5 ANZ: the EU / US scene on Meshtastic LongFast 919.875 MHz, MeshCore AU (915.8 MHz, SF10, 250 kHz) 4.075 MHz below
//   modeOpt[2]  seed of the packet ids, offsets and SNR variation (0 = 1)
//   modeOpt[3]  1: start with a quiet second (default: the first frame starts after 0.2 s); 2 (70 cm scene): no SNR variation
//   modeOpt[4]  1: the scene is placed for a receiver tuned to MeshCore (868 / 915) or to LoRa APRS EU 433.775 MHz (70 cm) instead
//   modeVal[0]  SNR of the weakest node in dB (0 = snrDb - 20); the others are spread evenly between it and snrDb
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeMeshSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
