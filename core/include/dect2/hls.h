// HTTP Live Streaming for one service: takes an MPEG transport stream, copies the video, turns the audio into AAC where needed and
// cuts the result into short segments with a rolling playlist. Browsers, iPhones, Apple TV and AirPlay play this directly.
// Everything stays in memory; the network tuner serves the files.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class HlsPipeline {
public:
    // Supplies transport stream bytes. It may block for a short while; it returns the number of bytes written to `buf` (at least 1) or
    // a value <= 0 when the stream has ended or the pipeline is being stopped.
    using Reader = std::function<int(uint8_t* buf, int size)>;

    // `token` becomes part of the segment names, so that segments can only be fetched by whoever got the playlist.
    HlsPipeline(Reader reader, const std::string& token = std::string());
    ~HlsPipeline();
    void start();
    void stop();

    // The playlist ("index.m3u8") or a segment ("seg00001.ts"); false when it does not exist (yet).
    bool get(const std::string& name, std::string& body, std::string& contentType);
    bool ready() const;           // the first playlist is available
    std::string error() const;   // why the stream cannot be made (for example an unsupported picture format)
    double idleSeconds() const;   // time since the last get(), to stop streams nobody watches

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
