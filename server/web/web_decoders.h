// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web decoders: the existing receivers (ADS-B, AIS, APRS, FT8 / FT4 / WSPR, SSTV, weather fax, Meteor-M LRPT, RDS, CW, RTTY) as shared
// web decoders (WebDecoder / DecoderFactory of web_hub.h). Each gets its own slice of the capture at the rate it needs; listeners who ask for
// the same kind on the same kHz share one instance. Results are `event` messages with a versioned schema (docs/ONAIR-WEB-PROTOCOL.md,
// "Decoder events"). Receive only: nothing here transmits, reads keys or writes a file; pictures live in memory and go to the subscribers
// as PNG strips. Digital voice and pager decoders are NOT registered here (policy: off unless the host accepted the legal notice).
// Receive-only decoders (transmit is the separate host setting web_allow_tx).
#pragma once
#include "web_hub.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
struct AdsbTelemetry;
struct AisTelemetry;
struct PacketTelemetry;
struct FmTelemetry;
struct CwTelemetry;
struct FtxDecode;
}

namespace onair_web {

constexpr int kDecoderSchema = 1;   // "v" of every decoder event

// Registers the decoders above with the hub. Returns the kinds registered.
std::vector<std::string> registerWebDecoders(WebHub& hub);

// What a kind needs: the baseband rate of its slice and the band it may be tuned in (Hz). false: not one of ours.
bool webDecoderSpec(const std::string& kind, double& rateHz, double& minHz, double& maxHz);
bool webDecoderFreqOk(const std::string& kind, double hz);
std::vector<std::string> webDecoderKinds();

// ---------------------------------------------------------------- pure helpers (tested in tests/server/test_web_decoders.cpp)
std::string cleanText(const std::string& s, size_t maxBytes = 4096);        // valid UTF-8 only (a bad byte becomes '?'), control characters become spaces, cut at a whole character
std::string base64(const uint8_t* p, size_t n);
uint32_t crc32Of(const uint8_t* p, size_t n, uint32_t crc = 0);
// A PNG with stored (uncompressed) deflate blocks; channels 1 grey, 3 RGB; rows are `stride` bytes apart. Empty: bad arguments.
std::vector<uint8_t> pngEncode(const uint8_t* px, int w, int h, int channels, size_t stride = 0);
// Mean-box downscale by an integer factor (new size ceil(w / f) x ceil(h / f))
std::vector<uint8_t> boxDownscale(const uint8_t* px, int w, int h, int channels, int f, int& ow, int& oh);

// Picture rows as they become available -> `image` events (strips of rows). The decoder supplies the rows; the streamer remembers which
// rows went out (a row is sent once), cuts strips so that an event stays well under 64 KB, and builds a small preview for late listeners.
struct ImageView {
    int w = 0, h = 0, channels = 1;
    std::function<const uint8_t*(int y)> row;   // the pixels of row y, or nullptr when that row has not arrived
};
class ImageStreamer {
public:
    explicit ImageStreamer(std::string kind) : kind_(std::move(kind)) {}
    // A new picture: id must change for every picture. `label` is shown by the client (e.g. the SSTV mode).
    void begin(uint64_t id, int w, int h, int channels, const std::string& label);
    // Strips of the rows that are available and not sent yet; at most maxEvents events. `final` marks the last strip once complete is true.
    void poll(const ImageView& v, bool complete, int maxEvents, std::vector<std::string>& ev);
    // A preview of everything that is available now as one event (for a listener who joins in the middle)
    void snapshot(const ImageView& v, bool complete, std::vector<std::string>& ev) const;
    uint64_t id() const { return id_; }
    int sentRows() const { return sent_; }
    bool active() const { return id_ != 0; }
private:
    std::string stripEvent(const ImageView& v, int y0, int rows, bool preview, int scale, bool fin) const;
    std::string kind_, label_;
    uint64_t id_ = 0;
    int w_ = 0, h_ = 0, ch_ = 1;
    std::vector<char> sent_flags_;
    int sent_ = 0, first_ = 0;
    bool finalSent_ = false;
};

// Event JSON of each table (one complete JSON object, <= 60000 bytes; the biggest tables are cut, newest first)
std::string adsbEventJson(const dect2::AdsbTelemetry& t);
std::string aisEventJson(const dect2::AisTelemetry& t);
std::string aprsEventJson(const dect2::PacketTelemetry& t);
std::string rdsEventJson(const dect2::FmTelemetry& t);
std::string cwEventJson(const dect2::CwTelemetry& t);
std::string rttyEventJson(double markHz, double spaceHz, double baud, double shiftHz, int state, float quality, uint64_t chars, const std::string& text);
// New FT8 / FT4 / WSPR decodes in one event
std::string ftxEventJson(const std::string& kind, double dialHz, const std::vector<dect2::FtxDecode>& d, float clockErrS, int clockN);
std::string statusEventJson(const std::string& kind, const std::string& state, const std::string& why);

// Real USB audio from complex baseband centred on the dial frequency (for the weather fax): keeps 200 .. 3400 Hz above it, slow AGC.
class UsbAudio {
public:
    explicit UsbAudio(double rate);
    void process(const dect2::cf32* x, size_t n, std::vector<float>& out);
private:
    double rate_;
    std::vector<dect2::cf32> taps_;
    std::vector<dect2::cf32> hist_;
    size_t pos_ = 0;
    float env_ = 0.0f;
};

} // namespace onair_web
