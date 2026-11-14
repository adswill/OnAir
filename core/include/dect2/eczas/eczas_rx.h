// e-Czas PCSK225 receiver: the symbol stream (int16) at 500 Hz, ten samples per bit at 50 bit/s, where a symbol is the phase change over a bit (+-16384 for a full
// 36 degree step; the first version took it from an FM discriminator), or the same at any integer multiple of 500 Hz. Sync search and bit slicer ported from sp5wwp/e-Czas main.c.
#pragma once
#include "eczas_proto.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2::eczas {

constexpr double kDiscRateHz = 500.0; // samples/s expected by the bit slicer (10 per bit @ 50 baud)

struct EczasRxPacket {
    TimePacket time;
    uint8_t frame[kFrameBytes]{};
    uint64_t endIdx = 0;      // index (500 Hz stream, counting from configure()) of the last bit of the frame: the frame began 96 bits (1.92 s) earlier
    float syncQuality = 0;    // the sync correlation against its threshold: 1 = just found, larger = cleaner
};

// What the bit slicer saw since configure(): every time the 16-bit sync pattern was found, and how the frames behind it ended
struct EczasRxStats {
    uint64_t syncs = 0;       // sync patterns found
    uint64_t frames = 0;      // frames cut after a sync (the sync word 0x5555 came out right)
    uint64_t crcOk = 0;       // time packets whose CRC passed
    uint64_t crcBad = 0;      // time packets whose CRC failed
    uint64_t otherType = 0;   // frames that are not the time packet (other slots of the minute frame)
    uint64_t falseSync = 0;   // sync pattern found but the sync word did not follow
};

// Streaming decoder: configure() with the PCM sample rate, then feed() int16 discriminator samples.
class EczasReceiver {
public:
    EczasReceiver();
    ~EczasReceiver();
    EczasReceiver(const EczasReceiver&) = delete;
    EczasReceiver& operator=(const EczasReceiver&) = delete;

    void reset();
    void configure(double pcmRateHz, int decimPhase = 0);
    void setPeakHint(int32_t peakAbsSample);
    // 4.1: the symbols are the keying state (+-16384 about the midpoint of the two phases) from a coherent detector, not the phase change over a bit
    void setCoherent(bool on);
    void feed(const int16_t* x, size_t n);
    bool pop(EczasRxPacket& out);   // every frame behind a sync word, oldest first: time.status is CrcOk for a time packet, BadCrc, WrongType (another slot of the minute frame)
    const EczasRxStats& stats() const { return stats_; }

private:
    double pcmRate_ = kDiscRateHz;
    int decim_ = 1;
    int decimPhase_ = 0;
    uint64_t inCount_ = 0;
    uint64_t outCount_ = 0;
    struct Impl;
    std::unique_ptr<Impl> demod_;
    std::vector<EczasRxPacket> queue_;
    EczasRxStats stats_;
};

// Decode all time packets found in a buffer (useful for tests and offline files).
std::vector<EczasRxPacket> decodeDiscPcm(const int16_t* x, size_t n, double pcmRateHz, int decimPhase = 0);

// BFO recording (8 kHz / 48 kHz mono) to 500 Hz discriminator samples (for tests/tools).
std::vector<int16_t> eczasAudioToDisc(const int16_t* pcm, size_t n, double pcmRateHz);

// BFO recording (8 kHz / 48 kHz mono) or 500 Hz discriminator PCM.
std::vector<EczasRxPacket> eczasDecodeRecording(const int16_t* pcm, size_t n, double pcmRateHz);

} // namespace dect2::eczas
