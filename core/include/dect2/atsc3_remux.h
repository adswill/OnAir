// Turns the MP4 fragments that ROUTE delivers (one stream of init + media segments per component, e.g. HEVC video and audio) into one MPEG-2
// transport stream, the input of the HLS pipeline. Timestamps of the components keep their common timeline.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class Atsc3Remux {
public:
    Atsc3Remux();
    ~Atsc3Remux();

    // Declares a component before its data arrives (so the output waits for it); `id` is chosen by the caller.
    void addComponent(int id);
    // `data` is an init segment (ftyp + moov) or a media segment (moof + mdat) in arrival order; init segments may be repeated.
    void push(int id, const std::vector<uint8_t>& data);
    // Blocks until transport stream bytes are available; returns the count (>= 1), or <= 0 when stopped and everything has been read.
    int read(uint8_t* buf, int size);
    // The same with a time limit: returns 0 when nothing came within timeoutMs, and -1 when the stream has ended.
    int readTimed(uint8_t* buf, int size, int timeoutMs);
    void stop();

    std::string error() const;
    int streamCount() const;           // streams in the output, 0 until the header is written
    bool started() const;
    // Codecs that could not be put into the output (for example AC-4 audio), by name; those components are dropped.
    std::vector<std::string> droppedCodecs() const;

    // Waits (up to the given time) for components that have not shown an init segment before writing the header.
    void setStartupWaitMs(int ms);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
