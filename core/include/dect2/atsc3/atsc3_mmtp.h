// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MMTP (ISO/IEC 23008-1, used by ATSC 3.0 as the second service transport, A/331 section 8): MMTP packets over UDP carry MPUs (one ISO BMFF
// file of about two seconds per asset: an MPU metadata part ftyp + mmpu + moov, a movie fragment moof, and the media samples as MFUs) and
// signalling messages (the MP table lists the assets and the presentation time of each MPU). This reads the packets, puts the MPUs of
// each asset together and rebuilds them as plain fragmented MP4 (an init segment once, then moof + mdat per MPU, with a running timeline),
// which is what the remuxer already takes from ROUTE. An MPU may hold several movie fragments (on air it holds one); each goes out as its own
// moof + mdat. A movie fragment that lost packets goes out with the samples that can still be played, in their place on the timeline.
#pragma once
#include "atsc3_ip.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {
namespace atsc3 {

// ---- packets (ISO/IEC 23008-1 9.2)
enum MmtpPayloadType { MmtpMpu = 0, MmtpGenericObject = 1, MmtpSignalling = 2, MmtpRepair = 3 };

struct MmtpPacket {
    int version = 0;                      // 0 or 1; on air version 1 has a two-byte field after the sequence number
    bool counterFlag = false, extensionFlag = false, rap = false;
    int fecType = 0, type = 0;
    uint16_t packetId = 0;
    uint32_t timestamp = 0, sequence = 0, counter = 0;
    const uint8_t* payload = nullptr;     // points into the buffer that was parsed
    size_t payloadSize = 0;
};
// Reads the header of an MMTP packet (the UDP payload); false when it is too short or a length in it runs past the end.
bool parseMmtpPacket(const uint8_t* data, size_t size, MmtpPacket& p);
// Builds an MMTP packet (for tests): version, counter, extension flag (with `ext` as the extension body), type, id, timestamp, sequence.
std::vector<uint8_t> makeMmtpPacket(const MmtpPacket& p, const std::vector<uint8_t>& payload, const std::vector<uint8_t>& ext = {});

// ---- MPU mode payload (9.3.2)
enum MpuFragmentType { MpuMetadata = 0, MpuMovieFragment = 1, MpuMfu = 2 };
struct MpuPayload {
    int fragmentType = 0;
    bool timed = true, aggregated = false;
    int fragmentation = 0;                // 0 whole, 1 first, 2 middle, 3 last fragment of a data unit
    int fragmentCounter = 0;              // fragments of the same data unit that follow this one
    uint32_t mpuSequence = 0;
    std::vector<std::pair<const uint8_t*, size_t>> units;   // data units (one unless aggregated); point into the parsed buffer
};
bool parseMpuPayload(const uint8_t* data, size_t size, MpuPayload& m);
// The 14-byte header in front of each fragment of a timed MFU: the sample it belongs to and the byte offset in the sample's media data.
constexpr size_t kMfuHeaderBytes = 14;
struct MfuHeader { uint32_t fragmentSequence = 0, sample = 0, offset = 0; int priority = 0, dependencyCounter = 0; };
bool parseMfuHeader(const uint8_t* data, size_t size, MfuHeader& h);

// ---- signalling (9.3.4, 10)
struct SignallingMessage { int id = 0, version = 0; std::vector<uint8_t> payload; };
// The messages of a signalling payload whose data is whole (fragmentation 0) or put back together; false when a length does not fit.
bool parseSignallingMessages(const uint8_t* data, size_t size, bool aggregated, bool longLength, std::vector<SignallingMessage>& out);

struct MmtpAsset {
    std::string id;                       // asset_id bytes
    std::string type;                     // asset_type, four characters (hvc1, hev1, ac-4, mp4a, stpp, ...)
    std::vector<uint16_t> packetIds;      // locations of type 0 (the same MMTP flow)
    std::vector<std::pair<uint32_t, uint64_t>> mpuTimes;   // MPU_timestamp_descriptor: MPU sequence number -> 64-bit NTP presentation time
};
struct MpTable {
    int tableId = 0, version = 0, mode = 0;   // 0x20 complete table, 0x11..0x1F subsets
    std::string packageId;
    std::vector<MmtpAsset> assets;
};
// The MP table in the payload of an MPT message (message ids 0x11 to 0x20); false when it is cut short.
bool parseMpTable(const uint8_t* data, size_t size, MpTable& t);
// "video", "audio" or "subtitles" for the asset types ATSC 3.0 uses (hvc1, ac-4, stpp, ...); the type itself otherwise.
std::string assetKind(const std::string& type);

// ---- MPU -> fragmented MP4
// The parts of an MPU metadata (ftyp + mmpu + moov) needed to rebuild it: the media track (handler vide or soun, never the MMT hint track,
// which may come first) and an init segment of that track alone, without the mmpu box (it holds the MPU sequence number and would make
// every init segment look new).
struct MpuInit {
    std::string handler;                  // "vide", "soun", or the handler of the first other track
    uint32_t trackId = 0, timescale = 0;
    uint32_t trexDuration = 0, trexSize = 0;
    uint32_t hintTrackId = 0, hintTrexSize = 0;
    uint32_t trexFlags = 0;               // the media track's default sample flags
    // HEVC video (hvcC): the length field size in front of each NAL unit, and the highest temporal sub-layer (-1: not HEVC or not given)
    int nalLengthSize = 0, maxTemporalId = -1;
    std::vector<uint8_t> init;            // ftyp + moov of the media track
};
bool readMpuMetadata(const uint8_t* data, size_t size, MpuInit& out);

// The movie fragment of an MPU (moof, followed on air by the 8-byte header of the mdat): the media track's samples, the size of the hint
// samples that sit in front of the media in the first fragment of each MFU, and what is needed to write a moof for the media alone.
struct MpuFragment {
    std::vector<uint32_t> sizes, durations;   // per media sample
    std::vector<int64_t> compositionOffsets;
    std::vector<uint32_t> flags;          // per media sample, from the trun, the tfhd or the trex (bit 16 set: not a sync sample)
    uint32_t hintSize = 0;
    uint64_t mediaBytes = 0;
    std::vector<uint8_t> trafOther;       // the media traf's boxes other than tfhd, tfdt and trun (kept as they are)
    std::vector<uint8_t> tfhd;            // its tfhd, rewritten to count data from the moof (no explicit base offset)
    uint32_t trunFlags = 0, firstSampleFlags = 0;
    int trunVersion = 0;
    std::vector<uint8_t> trunEntries;     // the per-sample entries of its trun as sent
};
// Bounds: a table that announces more samples or bigger samples than these is refused whole, never clamped.
constexpr uint32_t kMpuMaxSamples = 4096;
constexpr uint32_t kMpuMaxSampleBytes = 8u << 20;
constexpr size_t kMpuMaxBytes = 32u << 20;
bool readMpuFragment(const uint8_t* data, size_t size, const MpuInit& init, MpuFragment& out);
// moof + mdat for the media samples (in trun order, `media` their bytes back to back), with the decode time and fragment number given.
std::vector<uint8_t> buildMediaSegment(const MpuFragment& f, uint32_t fragmentNumber, uint64_t baseTime, const std::vector<uint8_t>& media);

// ---- a movie fragment with samples that did not arrive whole
// Whether a picture can be left out without harming any other (HEVC: a sub-layer non-reference picture of the highest sub-layer): 1 yes,
// 0 no, -1 not known (not HEVC, or too little of the sample arrived to read its first picture NAL unit). `data` is the start of the sample.
int sampleDisposable(const uint8_t* data, size_t size, const MpuInit& init);
// Which samples of `f` are played when only those marked in `whole` arrived whole, in decode order: a missing sample is left out; after a
// missing sample that others may depend on (`disposable` not 1), nothing is played up to the next sync sample, so the decoder is never handed
// a picture whose reference is gone. A track whose table marks no sample as non-sync is taken as all sync for sound, and as sync at its first
// sample only for video.
std::vector<char> playableSamples(const MpuFragment& f, const std::string& handler, const std::vector<char>& whole, const std::vector<char>& disposable);
// `f` cut down to the samples marked in `keep`, the time of each sample left out given to the sample played before it, so every sample left
// keeps its decode time; `lead` is the time of the samples before the first one kept (to add to the decode time of the fragment). The trun
// is written with explicit durations, sizes and flags; an sdtp keeps the entries of the samples kept, sample groups and subsample
// information are not kept. False when no sample is kept.
bool keepSamples(const MpuFragment& f, const std::vector<char>& keep, MpuFragment& out, uint64_t& lead);

// ---- one service: its MMTP flow, the assets of the MP table, and the rebuilt fragmented MP4 of each video and audio asset
struct MmtpStats {
    long packets = 0, bad = 0, signalling = 0, mpts = 0, nonTimed = 0, late = 0, seqRejected = 0;
    long mpusDelivered = 0, mpusDropped = 0, initsDelivered = 0, assetsSkipped = 0;
    long fragmentsDelivered = 0, fragmentsLost = 0;   // movie fragments (one per MPU on air; ISO/IEC 23008-1 allows several)
    long seqRestarts = 0;                             // the sender's MPU numbers jumped or went back and the asset carried on from them
    // MPUs (and movie fragments) with samples missing that went out with the samples that can be played, and the samples left out of them
    long mpusRepaired = 0, fragmentsRepaired = 0, samplesLost = 0, samplesCut = 0;
    long moofsRebuilt = 0;                            // movie fragments whose moof was lost, rebuilt from the asset's last one (sound only)
};

class MmtpService {
public:
    // `dstIp`, `dstPort`: the MMTP session announced by the SLT for this service
    MmtpService(uint32_t srcIp, uint32_t dstIp, int dstPort);
    ~MmtpService();
    // Feeds a UDP datagram of any session; returns true if it belonged to this service. Called from one thread at a time.
    bool push(const UdpDatagram& d);

    bool ready() const;                    // the MP table has been read and an init segment of a video or audio asset delivered
    std::vector<MmtpAsset> assets() const; // from the last complete MP table
    MmtpStats stats() const;
    // Lines the assets up by MPU sequence number instead of the MPU_timestamp descriptor (the default uses the descriptor when it is there).
    void setAlignBySequence(bool on);

    // Called for each init segment (when an asset starts or its moov changes) and each media segment, in order per asset.
    // `packetId` names the asset, `handler` is "vide" or "soun".
    std::function<void(uint16_t packetId, const std::string& handler, bool init, const std::vector<uint8_t>& data)> onObject;
    // Called once for each video or audio asset of the MP table, before its first object: the output can then wait for all of them
    // instead of starting with the asset whose first MPU happened to be whole first.
    std::function<void(uint16_t packetId, const MmtpAsset& asset)> onAsset;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace atsc3
} // namespace dect2
