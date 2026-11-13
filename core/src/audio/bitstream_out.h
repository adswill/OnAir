// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A sound device opened for an IEC 61937 bitstream (AC-3 / E-AC-3 passthrough to an AV receiver or soundbar): the data goes out as 16-bit
// stereo "PCM" at the link rate, bit for bit, with the non-audio flag set where the platform lets a program set it. EXPERIMENTAL: written
// from the platform documentation, NOT tested on real hardware (no receiver was available). See docs/ATMOS-3.9.md.
//   Windows: WASAPI exclusive mode with a WAVEFORMATEXTENSIBLE_IEC61937 format (KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL / _PLUS)
//   macOS:   CoreAudio hog mode, the output stream switched to a non-mixable physical format (kAudioFormat60958AC3 or 16-bit non-mixable PCM)
//   Linux:   ALSA "hdmi" / "iec958" device with the IEC 60958 channel status AES0=0x06 (non-audio), AES3 the link rate
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

class BitstreamDevice {
public:
    virtual ~BitstreamDevice() = default;
    // Plays `frames` IEC 60958 frames (2 words each). Blocks until the device has room; false when the device failed.
    virtual bool write(const int16_t* words, int frames) = 0;
};

// kind: 1 AC-3, 2 E-AC-3 (Iec61937Packer::Kind). device: "" the default output, else a device name (Windows / macOS: the name the
// output list shows; Linux: an ALSA PCM name such as "hdmi:CARD=0,DEV=0"). Must be called on the thread that will write (Windows COM).
std::unique_ptr<BitstreamDevice> bitstreamOpen(const std::string& device, int linkRate, int kind, std::string& err);

// The ALSA PCM name with the IEC 60958 channel status for a bitstream at `linkRate` (exposed for the test)
std::string alsaIecDeviceName(const std::string& device, int linkRate);

} // namespace dect2
