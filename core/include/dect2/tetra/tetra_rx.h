// TETRA network information receiver: the TETRA base stations around you, from what every base station broadcasts in the clear to all
// terminals on its downlink (ETSI EN 300 392-2): the synchronisation channel (BSCH: colour code, MCC, MNC, frame numbers, sharing mode),
// the broadcast network channel (BNCH SYSINFO: main carrier, band, duplex spacing, location area, cell services and security
// parameters), the access assignment channel (what each timeslot carries) and the neighbour cell broadcasts on the main control channel.
// Receive only. Nothing is decrypted and nothing addressed to a terminal is decoded: no short data, no subscriber identities. Voice: only unencrypted traffic bursts, as coded frames to a user-installed codec (tetra_voice.h); encrypted or unknown is never decoded.
// Signalling sent with air interface encryption is reported as encrypted and counted, never read. The user is responsible for following
// the rules on receiving radio signals that apply where they are.
//
// The input is taken in blocks of 2.3 s (more than two multiframes, so every carrier's synchronisation burst is in it twice). Each block's
// spectrum is searched for 25 kHz carriers; each one is brought to 4 samples per symbol, matched filtered (root raised cosine 0.35),
// frequency corrected (fourth power of the differential symbols), slot synchronised on the training sequences, and its broadcast
// channels decoded. A block is captured only while the previous one is not being searched, so a slow computer just searches less often.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/tetra/tetra_slots.h"
#include "dect2/tetra/tetra_voice.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "tetra_pdu.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {
using tetra::SlotSelect;
using tetra::SpeechFrameSink;   // tetra_voice.h declares it in dect2::tetra

struct TetraTelemetry;   // tetra_tel.h

class TetraReceiver {
public:
    static constexpr double kCaptureSec = 2.3;   // the air time of one capture: the carriers are searched and decoded a capture at a time

    TetraReceiver();
    ~TetraReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);   // where the user's (dial) frequency sits relative to 0 Hz in the input
    void setCenterHz(double hz);       // the radio's centre frequency (0: unknown, for files)
    bool ready() const;                // false when the input rate is too low for this mode
    void reset();                      // after a retune: drop the block being captured (the cell table is kept)
    void clearCells();                 // the user's Clear button
    void feed(const cf32* x, size_t n);
    bool telemetry(TetraTelemetry& out, uint64_t lastSeq);
    // The cell table and the event log stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    // Where unencrypted traffic bursts go (tetra_voice_codec.h). The sink must outlive the receiver; null: voice frames are dropped.
    void setVoiceSink(SpeechFrameSink* s);
    // What is listened to (issue #267). Timeslot 1..4 (0: the first one with clear voice); a carrier in Hz (0: the strongest with clear voice,
    // followed as before); single carrier: only the 25 kHz channel at the dial frequency is decoded, the rest of the span is not.
    void setVoiceTimeslot(int tn);
    void setVoiceCarrier(double hz);
    void setSingleCarrier(bool on);
    bool singleCarrier() const;
    // The stream whose clear voice reached the sink in the last capture: its carrier (Hz, 0 none) and timeslot (0 none). Any thread.
    double voiceCarrierHz() const;
    int voiceTimeslot() const;
    // Which traffic slot is listened to (tetra_slots.h): Auto (the first clear voice), a slot 1..4, or the slot carrying a talkgroup / ISSI.
    // Encrypted and unknown-state slots stay undecoded whatever is chosen.
    void setVoiceSlot(const SlotSelect& s);
    // The part of the capture the carrier search looks at, as a fraction of the sample rate centred on the capture (default tetra::kUsableBand =
    // 0.72, the flat part of a radio's decimation filter). A wide capture on a radio whose passband is flatter can search more of it (0.9);
    // nothing is refused above or below. Any thread.
    void setSearchBand(double fraction);
    double searchBand() const;
    // Single carrier: only the 25 kHz channel at the dial frequency (setSignalOffset) is filtered out and decoded; the span is not searched.
    // Off (default): the whole span is searched for carriers. The same call seen on two carriers of the span is played once either way.
    bool busy() const;                // a block is captured or being searched (tools: feed a file only as fast as it is searched)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning tetraTuning();

namespace tetra {

// One carrier brought to 72 kHz (4 samples per symbol): x at fs mixed down by shiftHz and filtered to the TETRA channel
std::vector<cf32> channelize(const cf32* x, size_t n, double fs, double shiftHz);

// The carriers of a capture at fs, strongest first (all of them; maxCarriers > 0 keeps only that many): 25 kHz wide rises of the spectrum within
// the middle `usableBand` (default kUsableBand) of the span, where a radio's decimation filter is flat (the span the application steps by). The floor the rises are measured against is local,
// the 20th percentile of the spectrum within kFloorWindowHz around each bin: a floor taken over the whole span sat in the roll-off of the
// passband at its edges, and the whole passband then stood above the threshold as one rise too wide to be carriers, or was cut into
// carriers only where the noise happened to dip, so the carriers of a capture came and went from one capture to the next.
struct CarrierPeak {
    double off = 0;        // Hz from the centre of the capture
    float levelDb = 0;     // peak over the local floor
    float powerDb = 0;     // power of the rise
};
constexpr double kUsableBand = 0.72;
constexpr double kFloorWindowHz = 600e3;
std::vector<CarrierPeak> findCarriers(const cf32* x, size_t n, double fs, double usableBand = kUsableBand, size_t maxCarriers = 0);

// Several carriers of one wide capture, each brought to 72 kHz, through ONE shared forward FFT (lora::FdChannelizer, the channeliser of the LoRa and
// Inmarsat banks): the cost is the FFT of the capture plus a small inverse FFT per carrier, not a mixer and a filter over the whole capture for
// every carrier. offsets in Hz from the centre of the capture; the result has one entry per offset (empty when too short). Same samples as
// channelize() within the carrier's own channel.
std::vector<std::vector<cf32>> channelizeMany(const cf32* x, size_t n, double fs, const std::vector<double>& offsets);

// What one carrier's capture held
struct CarrierDecode {
    bool tetra = false;        // slot synchronised on the training sequences
    bool synced = false;       // a synchronisation burst (BSCH) decoded
    Sync sync;                 // the last one
    int syncs = 0;
    uint32_t scrambler = 0;    // the cell's scrambling code used for the other channels (0: unknown)
    double cfoHz = 0;          // the frequency error found, relative to the carrier's assumed centre
    float snrDb = -99;
    int slots = 0, trafficSlots = 0, blocksOk = 0, blocksBad = 0;
    int voicePassed = 0, voiceNotDecoded = 0;   // traffic bursts that passed the encryption gate / refused (encrypted or state unknown);
                                                // a passed burst of a timeslot other than voiceTn asked for is not handed to the sink
    const char* voiceStatus = "";                // text of the last traffic burst verdict (tetra_voice.h)
    int voiceTn = 0;                             // the timeslot whose clear traffic reached the sink (0: none)
    int voiceDup = 0;                            // frames of the followed timeslot dropped because that air time had already been given to the sink
    int voiceGaps = 0;                           // frames of the followed timeslot given to the sink as silence (no clear burst, or lost between captures)
    std::vector<MacBlock> macs;
    int tsUsage[4] = {-1, -1, -1, -1};
    uint32_t slotTg[4] = {0, 0, 0, 0}, slotIssi[4] = {0, 0, 0, 0};   // IDs of a slot with clear voice, from its call set-up; 0 unknown, always 0 for encrypted/unknown calls (local only)
    SlotState slot[4] = {SlotState::Idle, SlotState::Idle, SlotState::Idle, SlotState::Idle};   // what each timeslot held in this capture (tetra_slots.h)
};
// knownMcc etc.: the identity of a cell already heard on this carrier, used when this capture has no synchronisation burst (-1: none)
// calls: the per-call encryption state of this carrier, kept across captures by the caller (null: no call state, so no traffic burst is ever decoded);
// nowSec: the clock of its expiry (any monotonic seconds).
// voiceTn: the timeslot (1..4) whose clear traffic goes to the sink; -1 the first timeslot with clear traffic, 0 every one (no silence then).
// timeline: the followed timeslot across captures (tetra_voice.h), kept by the caller; the frames of the slot lost between this capture and the
// last one go to the sink as silence first (null: none).
CarrierDecode decodeCarrier(const cf32* x72k, size_t n, int knownMcc = -1, int knownMnc = -1, int knownCc = -1,
                              SpeechFrameSink* sink = nullptr, CallTracker* calls = nullptr, double nowSec = 0, int voiceTn = 0,
                              VoiceTimeline* timeline = nullptr);

} // namespace tetra

} // namespace dect2
