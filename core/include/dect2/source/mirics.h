// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The register maths of OnAir's own driver for the Mirics MSi2500 USB bridge + MSi001 tuner (SDRplay RSP1, its clones, and the DVB-T / ISDB-T
// sticks built on the same two chips). Pure functions with no USB in them, so the tests check them without a radio; the driver that sends
// them is core/src/source/mirics_usb.cpp. The register meanings come from libmirisdr (Miroslav Slugen, Leif Asbrink SM5BSZ, Edouard Griffiths
// F4EXB; GPL-2.0-or-later, see THIRD_PARTY.md); none of its code is copied. Untested on hardware until a user reports back (issue #503).
#pragma once
#include "dect2/common/ring.h"   // cf32
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace mirics {

// ---- USB requests (vendor, device, OUT; bmRequestType 0x42)
constexpr uint8_t kReqType = 0x42;
constexpr uint8_t kReqWriteReg = 0x41;      // wValue = (value & 0xff) << 8 | register, wIndex = value >> 8
constexpr uint8_t kReqStartStream = 0x43;
constexpr uint8_t kReqStopStream = 0x45;
constexpr uint8_t kEndpoint = 0x81;         // the samples, isochronous (alternate setting 1) or bulk (alternate setting 3)
constexpr int kAltIso = 1, kAltBulk = 3;
constexpr size_t kBlockBytes = 1024;        // the samples come in 1024-byte blocks: 16-byte header (a 32-bit sample counter first), then data

// One register write as it goes over USB
struct Write { uint8_t reg; uint32_t value; };
inline void writeSetup(const Write& w, uint16_t& wValue, uint16_t& wIndex) {
    wValue = (uint16_t)(((w.value & 0xff) << 8) | w.reg);
    wIndex = (uint16_t)((w.value >> 8) & 0xffff);
}

// ---- sample formats of the MSi2500 (what a 1024-byte block carries)
enum class Format { S14 = 0, S12 = 1, S10 = 2, S8 = 3 };
struct FormatInfo {
    Format f;
    const char* key;         // radio setting value
    const char* name;
    int pairs;               // complex samples per block
    double maxRateHz;        // the fastest rate USB 2 carries in this format (24.576 MB/s of blocks)
    uint32_t reg7;           // the bridge's format register
    uint32_t reg3Code;       // the format's code in bits 12..15 of the rate register
};
const FormatInfo& formatInfo(Format f);
// The format for a rate: the user's choice ("14", "12", "10", "8") when the rate fits it, else the finest one that carries the rate ("auto"
// or a choice too fine for the rate).
Format formatFor(double rateHz, const std::string& wish = "auto");

// ---- sample rate (the bridge's ADC clock: registers 4 and 3)
constexpr double kMinRateHz = 1.3e6;        // below this the clock divider leaves its range
constexpr double kMaxRateHz = 12.096e6;     // the 8-bit format's limit on USB 2 (libmirisdr allows 15 Msps with lost samples)
struct RateRegs { uint32_t reg3 = 0, reg4 = 0; double rateHz = 0; int div = 0; uint32_t n = 0, frac21 = 0; };
// The registers for a rate (clamped to kMinRateHz..kMaxRateHz) in a format; rateHz = what they really give.
RateRegs rateRegs(double rateHz, Format f);

// ---- tuner (MSi001 behind the bridge's register 9; the bridge's register 8 drives the board's band-switch GPIOs)
enum class Flavour { Generic = 0, Rsp1 = 1 };   // the band-switch words differ: an SDRplay RSP1 (and clones of it), or a TV stick
enum class Band { Am, Vhf, B3, B45, L };
struct BandPlan { double lowMhz; Band band; int mode; int loDiv; bool upconvert; uint32_t gpioWord; };
const BandPlan& bandFor(double hz, Flavour fl);
// the tuner's IF filter widths (Hz), narrowest first
const std::vector<uint32_t>& filterWidths();
// the filter for a rate: the user's width when it is one of them, else the widest that is not wider than the rate (anti-alias), at
// least the narrowest
uint32_t filterFor(double rateHz, uint32_t wishHz = 0);

struct TuneRegs {
    std::vector<Write> writes;    // in the order they are sent
    double tunedHz = 0;           // the frequency the synthesiser really makes
    Band band = Band::Vhf;
    uint32_t n = 0, thresh = 0, frac = 0, afc = 0;
    int loDiv = 0;
};
// Register 8 (band switch) and the tuner writes for a frequency and filter width (zero IF, 24 MHz crystal)
TuneRegs tuneRegs(double hz, uint32_t filterHz, Flavour fl);

// ---- gain: LNA 24 dB (on the AM inputs the mix buffer, 24 dB on port 2), mixer 19 dB, IF (baseband) 0..59 dB: 0..102 dB in all
constexpr double kMaxGainDb = 102;
struct Gain { bool lna = true, mixer = true; int ifDb = 0; double totalDb() const { return (lna ? 24 : 0) + (mixer ? 19 : 0) + ifDb; } };
// The stages for an overall gain. lna / mixer: "auto" (on from the top down as the gain rises: IF first above 43 dB with both on), "on", "off".
Gain gainFor(double totalDb, const std::string& lna = "auto", const std::string& mixer = "auto");
// The tuner writes for those stages (register 1 gain reductions, then register 6 DC-offset tracking)
std::vector<Write> gainRegs(const Gain& g, Band band);

// ---- the fixed parts of a start / stop
std::vector<Write> initRegs();      // after the stream was stopped: the bridge's ADC and the default band switch
Write adcStopReg();                 // ADC and USB interface asleep

// ---- unpacking the 1024-byte blocks into complex float samples (full scale 1.0)
class Unpacker {
public:
    void reset(Format f) { f_ = f; carry_.clear(); expect_ = 0; have_ = false; lost_ = 0; blocks_ = 0; }
    // A stretch of the stream (bulk: any length, a block split over two calls is joined) or one isochronous packet (packet=true: a part
    // block at its end is dropped). Appends to out; returns the number of samples added.
    size_t feed(const uint8_t* p, size_t n, std::vector<cf32>& out, bool packet = false);
    uint64_t lostSamples() const { return lost_; }   // from gaps in the blocks' sample counter (wraps at 2^32)
    uint64_t blocks() const { return blocks_; }
    Format format() const { return f_; }
    static size_t block(Format f, const uint8_t* b, cf32* out);   // one block (kBlockBytes): its samples
private:
    void take(const uint8_t* b, std::vector<cf32>& out);
    Format f_ = Format::S14;
    std::vector<uint8_t> carry_;
    uint32_t expect_ = 0;
    bool have_ = false;
    uint64_t lost_ = 0, blocks_ = 0;
};

// ---- USB ids
struct UsbModel { uint16_t vid, pid; const char* name; Flavour flavour; };
const std::vector<UsbModel>& usbModels();
const UsbModel* usbModel(uint16_t vid, uint16_t pid);

} // namespace mirics
} // namespace dect2
