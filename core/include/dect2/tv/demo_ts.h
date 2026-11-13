// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Built-in test multiplex for the synthetic transmitters: MPEG-2 programmes muxed into a transport stream.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

// One programme in the mux: its own video and audio PIDs (PAT / PMT / SDT). They do not share elementary streams.
struct DemoTsService {
    std::string name;
    int picture = 0;            // 0 test card, 1 colour bars, 2 grey ramp (ignored when file or camera is set)
    std::string file;           // optional video file for this service only
    std::string camera;         // video device id (listVideoInputs); empty = default camera when liveCamera is set
    bool liveCamera = false;    // this programme is a live camera even when `camera` is empty (OS default)
    int generator = 0;          // 1..8: the live stream of Tools > Digital video generator N (Output: Transmitter); the transmitter remuxes these (#492)
};

// Names that go in PAT / SDT / the mpegts muxer. Empty services: one programme called "OnAir".
struct DemoTsIdentity {
    std::string provider = "OnAir";
    std::string network = "OnAir";
    std::vector<std::string> services;     // names only: each becomes its own programme (different picture / beep)
    std::vector<DemoTsService> programmes; // if non-empty, this is the mux (takes priority over `services`)
    int serviceId = 1;                     // first service_id; the rest are consecutive
};

// A packet source for dvbt::Generator / atsc::Generator. The clip is encoded once per identity and padded with null
// packets so that it fills `netBitrate` bits per second of the channel. A camera on any programme switches to a live mux.
std::function<void(uint8_t*)> demoTsSource(double netBitrate);
std::function<void(uint8_t*)> demoTsSource(double netBitrate, const DemoTsIdentity& id);

} // namespace dect2
