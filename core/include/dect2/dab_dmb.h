// DMB video services in a DAB ensemble (T-DMB): ETSI TS 102 427 (the outer code of a transport stream in a stream mode sub-channel) and
// TS 102 428 (the DMB video service: MPEG-4 Systems in an MPEG-2 transport stream, H.264 video with HE-AAC or BSAC sound).
//
// The chain after the sub-channel's Viterbi decoder and energy dispersal: the byte stream -> the packet sync (0x47 every 204 bytes, the sync bytes
// pass the interleaver undelayed) -> the convolutional de-interleaver (Forney, I = 12, M = 17, as DVB-T) -> Reed-Solomon (204,188) -> the DMB
// transport stream. Its elementary streams are MPEG-4 sync layer (SL) packets in PES packets (stream type 0x12) with the object descriptors in
// ISO/IEC 14496 sections (stream type 0x13), which players do not take reliably. DmbRemux turns them into a plain transport stream: one program
// with H.264 (Annex B, stream type 0x1B) and AAC in ADTS (0x0F), with the SL time stamps as PES time stamps. That stream goes to the player like
// a DVB service.
#pragma once
#include "dab_tel.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// MPEG-4 Systems (ISO/IEC 14496-1) to a plain transport stream. Feed it the DMB transport stream packet by packet.
class DmbRemux {
public:
    static constexpr int kProgram = 1;                       // the program number of the output
    static constexpr int kPmtPid = 0x100, kVideoPid = 0x101, kAudioPid = 0x102, kPcrPid = 0x1F0;
    DmbRemux();
    ~DmbRemux();
    void reset();
    void feed(const uint8_t* pkt188);
    void setSink(std::function<void(const uint8_t* pkt188)> cb);
    // The BSAC hook: ER BSAC access units (audio object type 22, Korean DMB) arrive here, with the AudioSpecificConfig and the presentation
    // time (90 kHz). BSAC is not supported (its specification is not freely available): the video plays alone and the service info says so.
    // Should a BSAC decoder ever exist, it plugs in at the single call of this.
    void setBsacSink(std::function<void(const std::vector<uint8_t>& asc, const uint8_t* au, int n, int64_t pts90k)> cb);
    void stats(DmbStats& s) const;                          // fills video, audio, sampleRate, channels, note, tsOut
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// The outer decoder of a DMB sub-channel and the remux behind it.
class DmbDecoder {
public:
    DmbDecoder();
    ~DmbDecoder();
    void reset();                                           // a new sub-channel or a break in the stream
    void push(const uint8_t* frame, int n);                 // one logical frame of the sub-channel (bytes after the energy dispersal)
    // The plain transport stream of the service: all packets that one logical frame produced (after Reed-Solomon and the remux)
    void setPacketSink(std::function<void(const uint8_t* pk, size_t n)> cb);
    // Tests: every packet of the DMB transport stream as it was sent (after Reed-Solomon, before the remux)
    void setRawTap(std::function<void(const uint8_t* pkt188)> cb);
    DmbRemux& remux();
    DmbStats stats() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
