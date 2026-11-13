// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT session layer: a pure state machine without a radio. OPEN / ACCEPT / START, data bursts with acknowledgement bitmaps, FINISH / VERIFIED,
// resume from the receiver's block map, whole-file hash (BLAKE2s-256 or SHA-256), optional compression with the 64 kB skip rule and the profile stepper.
//
// The sender and the receiver exchange byte messages (control messages, ack) and logical bursts (blocks of file data); frames <-> bursts / messages
// glue for the physical layer is at the bottom. A radio driver does:  action = sender.next();  send it;  feed the reply to sender.onReply() or call
// sender.onTimeout().  The receiver answers receiver.onCtrl(message) / receiver.onBurst(burst) with the bytes to send back (empty = nothing).
//
// Compression: zstd is not linked in this repository (no zstd or zlib in CMakeLists.txt / third_party), so no codec is built in. The plumbing is
// complete: the sender takes a CompressFn, the receiver a DecompressFn per compression id (id 1 is reserved for zstd), and the skip rule is in
// compressionWorthwhile(). Until a codec is registered every file is sent raw.
//
// Block map: the spec's fixed 256-byte chunks cannot be used at P0 / P1 (see kaft_frame.h), so the map is a set of byte ranges (RangeSet), which is
// also the natural RLE coding for the ACCEPT message.
#pragma once
#include "dect2/kaft/kaft_frame.h"
#include "dect2/kaft/kaft_rx.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dect2::kaft {

// ---------------------------------------------------------------- hashes
using Hash32 = std::array<uint8_t, 32>;
Hash32 blake2s256(const uint8_t* p, size_t n);   // RFC 7693
Hash32 sha256Hash(const uint8_t* p, size_t n);   // FIPS 180-4 (the repository's dect2::meshcrypto::sha256)
Hash32 hashBytes(int code, const uint8_t* p, size_t n);   // 0 = BLAKE2s-256, 1 = SHA-256

// ---------------------------------------------------------------- byte ranges
class RangeSet {
public:
    using Range = std::pair<uint64_t, uint64_t>;   // [first, second)
    void add(uint64_t a, uint64_t b);
    void remove(uint64_t a, uint64_t b);
    bool covers(uint64_t a, uint64_t b) const;
    uint64_t total() const;
    bool empty() const { return r_.empty(); }
    void clear() { r_.clear(); }
    const std::vector<Range>& ranges() const { return r_; }
private:
    std::vector<Range> r_;   // sorted, disjoint, not touching
};

// ---------------------------------------------------------------- messages
enum class MsgType : uint8_t { Open = 1, Accept = 2, Start = 3, Finish = 4, Verified = 5, Reject = 6, Abort = 7, Poll = 8, Ack = 9 };
constexpr size_t kMaxCtrlBytes = (size_t)kCtrlMaxBlocks * 236 - 2;   // a control message must fit kCtrlMaxBlocks blocks of P1
constexpr size_t kMaxMapRanges = 100;                                 // ranges in ACCEPT / REJECT

struct OpenMsg {
    uint16_t session = 0;
    std::string callTx, callRx = "CQ";
    uint8_t width = 0;                // width code
    std::string fileName;             // UTF-8, no path
    uint64_t payloadSize = 0;         // bytes that are sent (after compression)
    uint64_t origSize = 0;            // original file size
    uint8_t hashCode = 0;
    Hash32 hash{};                    // hash of the payload bytes
    uint8_t compression = 0;          // 0 = none, 1 = zstd (reserved)
    uint8_t startProfile = 0;
    uint32_t resumeOffset = 0;        // sender's hint, the real map comes back in ACCEPT
    uint16_t blockSize = 0;           // data bytes per block at the starting profile
};
struct AcceptMsg {
    uint16_t session = 0;
    std::string callRx;
    int16_t esN0x10 = 0;              // measured Es/N0 in tenths of dB
    uint8_t profile = 0;              // chosen starting profile
    std::vector<RangeSet::Range> have;
};
struct StartMsg { uint16_t session = 0; uint8_t profile = 0; };
struct FinishMsg {
    uint16_t session = 0;
    uint64_t payloadSize = 0;
    uint8_t hashCode = 0;
    Hash32 hash{};                    // of the payload
    Hash32 origHash{};                // of the original file (equal to hash when not compressed)
};
struct VerifiedMsg { uint16_t session = 0; bool ok = true; };
struct RejectMsg { uint16_t session = 0; uint8_t reason = 0; std::vector<RangeSet::Range> have; };   // 0 = missing blocks, 1 = hash mismatch, 2 = decompression failed
struct AbortMsg { uint16_t session = 0; uint8_t reason = 0; };                                          // 1 = busy, 2 = unsupported, 3 = too big
struct PollMsg { uint16_t session = 0; uint16_t burstNo = 0; };
struct AckMsg {
    uint16_t session = 0;
    uint16_t burstNo = 0;
    std::vector<uint8_t> seen;                      // per frame of the burst (0 / 1): header decoded
    std::vector<std::vector<uint8_t>> blockOk;      // per frame: per block 0 / 1 (empty when not seen)
    int16_t esN0x10 = 0;
    uint8_t lossPct = 0;
    uint8_t requestedProfile = 0;
};

std::vector<uint8_t> encode(const OpenMsg&);     bool decode(const std::vector<uint8_t>&, OpenMsg&);
std::vector<uint8_t> encode(const AcceptMsg&);   bool decode(const std::vector<uint8_t>&, AcceptMsg&);
std::vector<uint8_t> encode(const StartMsg&);    bool decode(const std::vector<uint8_t>&, StartMsg&);
std::vector<uint8_t> encode(const FinishMsg&);   bool decode(const std::vector<uint8_t>&, FinishMsg&);
std::vector<uint8_t> encode(const VerifiedMsg&); bool decode(const std::vector<uint8_t>&, VerifiedMsg&);
std::vector<uint8_t> encode(const RejectMsg&);   bool decode(const std::vector<uint8_t>&, RejectMsg&);
std::vector<uint8_t> encode(const AbortMsg&);    bool decode(const std::vector<uint8_t>&, AbortMsg&);
std::vector<uint8_t> encode(const PollMsg&);     bool decode(const std::vector<uint8_t>&, PollMsg&);
std::vector<uint8_t> encode(const AckMsg&);      bool decode(const std::vector<uint8_t>&, AckMsg&);
int msgType(const std::vector<uint8_t>& m);      // MsgType value or -1

// ---------------------------------------------------------------- compression
using CompressFn = std::function<std::vector<uint8_t>(const uint8_t*, size_t)>;
using DecompressFn = std::function<bool(const uint8_t*, size_t, std::vector<uint8_t>& out)>;
bool isCompressedExtension(const std::string& fileName);   // jpg png mp4 mp3 zip 7z gz xz zst flac ogg docx apk ...
// the spec's rule: compress a sample of the first 64 kB, skip when it shrinks by less than 5 percent or the extension is a known compressed type
bool compressionWorthwhile(const uint8_t* data, size_t n, const std::string& fileName, const CompressFn& compress);

// ---------------------------------------------------------------- profile stepper
struct StepperConfig {
    double upMarginDb = 2.0;      // step up when Es/N0 is this far above the next profile's requirement ...
    int upBursts = 3;             // ... for this many bursts in a row
    double upLoss = 0.02;         // ... and the block loss is below this
    double downMarginDb = 0.5;    // step down at once below requirement + this
    double downLoss = 0.2;        // or above this block loss
    double downLoss2 = 0.5;       // two steps above this
    int hysteresis = 10;          // bursts without a step up after a step down
    int averageBursts = 4;        // Es/N0 is averaged over the last bursts
    int minProfile = 0;
    int maxProfile = kNumProfiles - 1;
};
class ProfileStepper {
public:
    explicit ProfileStepper(const StepperConfig& c = StepperConfig()) : c_(c) {}
    void reset() { hist_.clear(); good_ = 0; hold_ = 0; }
    // after a burst sent at profile `cur`: the profile to use next
    int update(int cur, double esN0Db, double blockLoss);
    int holdoff() const { return hold_; }
private:
    StepperConfig c_;
    std::vector<double> hist_;
    int good_ = 0, hold_ = 0;
};
int startingProfile(double esN0Db, double marginDb = 2.0, int maxProfile = kNumProfiles - 1);

// ---------------------------------------------------------------- logical bursts
struct DataBlock {
    uint32_t offset = 0;
    std::vector<uint8_t> data;
};
struct TxBurst {
    uint16_t session = 0;
    uint16_t burstNo = 0;                              // 12 bits, wraps (it is the high bits of the frame number)
    int profile = 0;
    Width width = Width::W1M;
    std::vector<std::vector<DataBlock>> frames;        // up to kFramesPerBurst frames
    size_t blockCount() const { size_t n = 0; for (const auto& f : frames) n += f.size(); return n; }
    size_t dataBytes() const { size_t n = 0; for (const auto& f : frames) for (const auto& b : f) n += b.data.size(); return n; }
    double seconds() const;                            // airtime of the data frames
};
struct RxDataBlock {
    bool ok = false;
    DataBlock blk;
};
struct RxBurstFrame {
    bool seen = false;                                 // header decoded
    std::vector<RxDataBlock> blocks;
};
struct RxBurst {
    uint16_t session = 0;
    uint16_t burstNo = 0;
    int profile = 0;
    float esN0Db = 0;
    std::vector<RxBurstFrame> frames;                  // one entry per frame of the burst, in order (not-seen ones included)
};

// ---------------------------------------------------------------- sender
struct SenderConfig {
    std::string callsign, peer = "CQ";
    Width width = Width::W1M;
    std::string fileName;
    uint16_t session = 1;
    int hashCode = 0;                 // 0 BLAKE2s-256, 1 SHA-256
    int startProfile = 4;
    int minProfile = 0, maxProfile = kNumProfiles - 1;
    bool fixedProfile = false;        // ignore the receiver's requests
    int framesPerBurst = kFramesPerBurst;
    CompressFn compress;              // empty: no compression
    uint8_t compressionId = 1;
    int maxRetries = 6;
};
struct SenderStats {
    uint64_t dataBytesSent = 0;       // file data in all blocks sent (retransmissions included)
    uint64_t blocksSent = 0, blocksLost = 0;
    uint32_t bursts = 0, ctrlSent = 0, polls = 0, timeouts = 0;
    double airtime = 0;               // seconds, transmit plus the replies, turnarounds and timeouts
    double efficiency(uint64_t payload) const { return dataBytesSent ? (double)payload / (double)dataBytesSent : 1.0; }
};
constexpr double kTurnaroundSeconds = 0.025;
constexpr double kTimeoutSeconds = 0.5;
double ctrlFrameSeconds(Width w, size_t msgBytes);   // airtime of a control message (ack included)

struct SendAction {
    enum Kind { None, Ctrl, Burst, Done, Failed } kind = None;
    std::vector<uint8_t> ctrl;        // Ctrl
    bool expectsReply = false;        // Ctrl: call onReply() with the answer or onTimeout()
    TxBurst burst;                    // Burst (always expects an ack)
};

class Sender {
public:
    Sender(const SenderConfig& cfg, const std::vector<uint8_t>& file);
    SendAction next();
    void onReply(const std::vector<uint8_t>& msg);
    void onTimeout();
    bool done() const { return state_ == State::Done; }
    bool failed() const { return state_ == State::Failed; }
    int profile() const { return profile_; }
    uint64_t payloadSize() const { return payload_.size(); }
    uint64_t bytesAcked() const { return acked_.total(); }
    int compression() const { return compression_; }
    const SenderStats& stats() const { return stats_; }
    const Hash32& payloadHash() const { return hash_; }
private:
    enum class State { Open, WaitAccept, Start, Data, WaitAck, Poll, Finish, WaitVerified, Done, Failed };
    TxBurst buildBurst();
    void retry(State back);
    SenderConfig cfg_;
    std::vector<uint8_t> payload_;
    int compression_ = 0;
    uint64_t origSize_ = 0;
    Hash32 hash_{}, origHash_{};
    State state_ = State::Open;
    int profile_ = 0;
    int retries_ = 0;
    uint16_t burstNo_ = 0;
    RangeSet todo_, acked_;
    TxBurst inflight_;
    SenderStats stats_;
};

// ---------------------------------------------------------------- receiver
struct ReceiverConfig {
    std::string callsign;
    std::map<int, DecompressFn> decompressors;
    StepperConfig stepper;
    uint64_t maxPayload = 1ull << 31;
};
class Receiver {
public:
    explicit Receiver(const ReceiverConfig& cfg = ReceiverConfig());
    // a control message (esN0Db: measured on the frame, NaN when unknown); the reply to send back, empty for none
    std::vector<uint8_t> onCtrl(const std::vector<uint8_t>& msg, double esN0Db = std::nan(""));
    std::vector<uint8_t> onBurst(const RxBurst& burst);
    bool complete() const { return complete_; }
    const std::vector<uint8_t>& file() const { return file_; }
    const std::string& fileName() const { return name_; }
    uint64_t bytesHeld() const { return have_.total(); }
    int heldSessions() const { return (int)held_.size(); }
private:
    struct Held {
        std::vector<uint8_t> buf;
        RangeSet have;
    };
    std::vector<uint8_t> reject(uint8_t reason);
    ReceiverConfig cfg_;
    std::map<std::string, Held> held_;      // resumable sessions, key = hash(name, size, file hash)
    std::string key_;
    enum class State { Idle, Accepted, Started, Done } state_ = State::Idle;
    uint16_t session_ = 0;
    std::string name_, peer_;
    OpenMsg open_;
    std::vector<uint8_t> buf_;
    RangeSet have_;
    bool complete_ = false;
    std::vector<uint8_t> file_;
    ProfileStepper stepper_;
    std::vector<uint8_t> lastAck_;
    uint16_t lastBurst_ = 0xFFFF;
};

// ---------------------------------------------------------------- glue to the physical layer
std::vector<FrameSpec> framesForBurst(const TxBurst& b, const std::string& callsign);
bool burstFromFrames(const std::vector<RxFrame>& frames, RxBurst& out);   // data frames of one burst (any order), false when none
FrameSpec frameForCtrl(const std::vector<uint8_t>& msg, bool isAck, uint16_t session, uint16_t frameNo, Width w, const std::string& callsign);
bool msgFromFrame(const RxFrame& f, std::vector<uint8_t>& msg);           // false when a block failed or the length is wrong

}  // namespace dect2::kaft
