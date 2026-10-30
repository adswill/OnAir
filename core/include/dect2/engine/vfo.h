// Multi-VFO: several independent channels placed on ONE capture. Every channel has its own frequency (an offset from the capture centre),
// bandwidth, mode (a demodulator that plays, or a decoder), squelch and mixer settings. The channels of the demodulators that are above
// their squelch are mixed to one stereo stream (volume, pan, mute per channel); a decoder channel feeds its own receiver (POCSAG / FLEX
// pagers, APRS / AX.25 packet) and keeps its own results. Nothing here limits the number of channels or their width: when the CPU cannot
// keep up, load() goes above 1 and the interface warns; no channel is refused.
//
//   feed(span) -> per channel: ChannelFilter (mix to 0, resample) -> band filter -> demodulator -> squelch -> mixer -> sink(stereo 48 kHz)
//                                                                  \-> decoder receiver (192 ksps, as the whole-span banks do)
// The engine feeds the bank in addition to the mode's own receiver, and only when channels exist, so the single-receiver path is untouched.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/packet/packet_tel.h"
#include "dect2/pager/pager_tel.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

enum class VfoMode : int { Nfm = 0, Am, Usb, Lsb, Wfm, Pager, Packet, Count };
const char* vfoModeName(VfoMode m);
inline bool vfoIsDecoder(VfoMode m) { return m == VfoMode::Pager || m == VfoMode::Packet; }
double vfoDefaultBandwidthHz(VfoMode m);

struct VfoConfig {
    int id = 0;                    // chosen by the caller, unique
    double offsetHz = 0;           // from the capture centre
    double bandwidthHz = 0;        // 0: the mode's default; no upper limit
    VfoMode mode = VfoMode::Nfm;
    double squelchDb = -150;       // channel power in dBFS; <= -150: always open
    float volume = 1.0f;           // 0 .. 2
    float pan = 0.0f;              // -1 left .. +1 right
    bool mute = false;
    bool enabled = true;
};

struct VfoStatus {
    int id = 0;
    float levelDb = -200;          // channel power, dBFS
    bool open = false;             // above the squelch (a decoder channel: has a carrier)
    uint64_t items = 0;            // decoder: pages / frames so far
    std::string info;              // decoder: a one-line state
};

class VfoBank {
public:
    using Sink = std::function<void(const float* stereo, size_t frames)>;   // 48 kHz interleaved; called from feed()
    static constexpr int kAudioRate = 48000;
    VfoBank();
    ~VfoBank();
    VfoBank(const VfoBank&) = delete;
    VfoBank& operator=(const VfoBank&) = delete;

    void configure(double spanRate);                       // the capture's rate; the channels are built again
    double spanRate() const;
    void setChannels(const std::vector<VfoConfig>& c);     // any thread; a channel whose id, frequency, width and mode are unchanged keeps its state
    size_t channelCount() const;
    void setSink(Sink s);
    void feed(const cf32* x, size_t n);                    // the analysis thread
    void reset();                                          // after a retune: filters and receivers forget the signal (results stay)

    std::vector<VfoStatus> status() const;
    bool pagerTelemetry(int id, PagerTelemetry& t) const;
    bool packetTelemetry(int id, PacketTelemetry& t) const;
    void clearResults();

    // CPU: the share of real time the channels' work took in the last second (1 = all of it); above 1 the channels fall behind
    double load() const;
    // the reason a channel cannot work well (the span is narrower than the channel, or than a decoder needs); empty when fine. Never blocks.
    std::string warning(int id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
