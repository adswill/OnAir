// Dolby broadcast audio: what a stream is (AC-3, E-AC-3, E-AC-3 with Dolby Atmos JOC, AC-4 immersive), how its channel bed is laid out, the
// layout the player sends to the sound device, and the IEC 61937 packer that passes an AC-3 / E-AC-3 stream untouched to an AV receiver.
//
// Nothing here decodes sound. The headers are read from the bitstream (ETSI TS 102 366 annex E for the E-AC-3 bsi, ETSI TS 103 420
// clause 8.3 for the Atmos flag in addbsi, ETSI TS 103 190-2 annex E.6 / E.10 for the AC-4 DSI, ETSI EN 300 468 annex D.7 for the DVB AC-4
// descriptor). The Dolby Atmos objects themselves cannot be rendered by OnAir: the object renderer is Dolby's licensed technology. FFmpeg
// decodes the channel bed of an E-AC-3 JOC stream (5.1 or 7.1) and ignores the objects; a receiver that gets the bitstream renders them.
// See docs/ATMOS-3.9.md.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

// ---- AC-3 / E-AC-3 sync frames -----------------------------------------------------------------------------------------------------
struct DolbyFrame {
    bool valid = false;
    bool eac3 = false;          // bsid 11..16 (E-AC-3); 0..10 is AC-3
    int bsid = -1;
    int frameType = 0;          // E-AC-3 strmtyp: 0 independent, 1 dependent, 2 AC-3 converted; AC-3 frames: 0
    int substreamId = 0;
    int frameBytes = 0;         // the whole sync frame
    int sampleRate = 0;
    int blocks = 6;             // audio blocks of 256 samples (1, 2, 3 or 6)
    int acmod = 0, lfe = 0, bsmod = 0;
    int chanmap = -1;           // dependent substream channel map (16 bits, TS 102 366 table E.1.4), -1 when none
    bool joc = false;           // flag_ec3_extension_type_a in addbsi: Dolby Atmos (JOC), TS 103 420 clause 8.3
    int complexity = 0;         // complexity_index_type_a: bed + ISF + dynamic objects, at most 16
    int addbsiBit = -1;         // E-AC-3: bit offset of the addbsie flag from the start of the frame (tests)
};
// Reads the header of the sync frame at p (starts with 0x0B 0x77). false when it is not a valid AC-3 / E-AC-3 header.
bool parseDolbyFrame(const uint8_t* p, size_t n, DolbyFrame& out);

// Speaker positions of a channel bed, as bits (one bit per loudspeaker)
enum DolbySpk : uint32_t {
    kSpkL = 1u << 0, kSpkR = 1u << 1, kSpkC = 1u << 2, kSpkLfe = 1u << 3, kSpkLs = 1u << 4, kSpkRs = 1u << 5,
    kSpkLrs = 1u << 6, kSpkRrs = 1u << 7, kSpkCs = 1u << 8, kSpkLc = 1u << 9, kSpkRc = 1u << 10, kSpkTs = 1u << 11,
    kSpkLsd = 1u << 12, kSpkRsd = 1u << 13, kSpkLw = 1u << 14, kSpkRw = 1u << 15, kSpkVhl = 1u << 16, kSpkVhr = 1u << 17,
    kSpkVhc = 1u << 18, kSpkLts = 1u << 19, kSpkRts = 1u << 20, kSpkLfe2 = 1u << 21,
};
uint32_t dolbyAcmodSpeakers(int acmod, int lfe);       // the independent substream's speakers
uint32_t dolbyChanmapSpeakers(int chanmap);            // the speakers a dependent substream's chanmap names
int dolbySpeakerCount(uint32_t spk);
// "2.0", "5.1", "7.1", "1.0", or "<n>.<lfe>" for other beds
std::string dolbyBedName(uint32_t spk);

// What a stream carries, gathered over its frames. feed() takes PES payloads or demuxed packets (whole or split sync frames).
struct DolbyStreamSummary {
    bool valid = false;
    bool eac3 = false;
    bool atmos = false;         // JOC flagged in at least one frame
    int complexity = 0;         // highest complexity_index_type_a seen
    int sampleRate = 0;
    uint32_t speakers = 0;      // the bed: independent substream 0 plus its dependent substreams
    int channels = 0;           // dolbySpeakerCount(speakers)
    std::string bed;            // dolbyBedName(speakers)
    int bitrateKbps = 0;        // of the whole program (independent + dependent frames of one block period)
    uint64_t frames = 0;
};
class DolbyStreamInfo {
public:
    void reset() { *this = DolbyStreamInfo(); }
    void feed(const uint8_t* p, size_t n);
    const DolbyStreamSummary& summary() const { return s_; }
private:
    std::vector<uint8_t> pend_;
    DolbyStreamSummary s_;
    uint32_t curSpk_ = 0;       // speakers of the program being collected (from its independent frame to the next one)
    uint32_t prevSpk_ = 0;      // of the program before: the bed changes only when two programs in a row agree (one damaged frame does not flip it)
    int curBytes_ = 0, curBlocks_ = 0;
};

// The one line a user sees: "Dolby Atmos (E-AC-3 JOC), 5.1 bed", "Dolby Digital Plus (E-AC-3) 5.1", "Dolby Digital (AC-3) 2.0".
// English; the app builds its own translated line from the summary.
std::string dolbyFormatLine(const DolbyStreamSummary& s);

// ---- AC-4 (ATSC 3.0, DVB) ------------------------------------------------------------------------------------------------------------
struct Ac4Presentation {
    int version = -1;           // presentation_version
    int config = -1;            // presentation_config_v1
    bool channelCoded = false;  // false: the presentation has object or A-JOC substreams (pres_ch_mode -1)
    int chMode = -1;            // dsi_presentation_ch_mode (TS 103 190-2 table 56), -1 when not channel coded
    bool immersive() const { return (!channelCoded && config >= 0 && config != 6) || (chMode >= 9 && chMode <= 15); }
};
struct Ac4DsiInfo {
    bool valid = false;
    int bitstreamVersion = -1, sampleRate = 0;
    std::vector<Ac4Presentation> presentations;
    bool immersive = false;     // any presentation with objects, A-JOC or height channels: Dolby Atmos in AC-4
    std::string bed;            // the channel configuration of the first immersive (else the first) presentation: "7.1.4", "5.1", "objects"
};
// `p` is the payload of the 'dac4' box: ac4_dsi_v1() (TS 103 190-2 E.6). Stops safely at the end of the data.
Ac4DsiInfo parseAc4DsiV1(const uint8_t* p, size_t n);
const char* ac4ChModeName(int chMode);   // table 56: "mono", "stereo", "3.0", "5.0", "5.1", "7.0", "7.1", "7.0.4", "7.1.4", "9.0.4", "9.1.4", "22.2"

// The DVB AC-4 descriptor (extension descriptor 0x7F, descriptor_tag_extension 0x15, EN 300 468 annex D.7). `body` follows the extension tag.
struct Ac4Descriptor {
    bool valid = false;
    bool haveConfig = false, dialogueEnhancement = false;
    int channelMode = -1;       // 0 mono, 1 stereo, 2 multichannel
    std::vector<uint8_t> dsi;   // the ac4_dsi bytes (the descriptor's "TOC"), when present
    Ac4DsiInfo dsiInfo;         // parsed, when the bytes are an ac4_dsi_v1()
};
Ac4Descriptor parseAc4Descriptor(const uint8_t* body, size_t n);

// What the receiver knows about the AC-4 sound of the ATSC 3.0 service (set by the remuxer from the init segment, read by the UI)
void ac4NoteDsi(const Ac4DsiInfo& d);
Ac4DsiInfo ac4LastDsi();

// ---- what the player sends to the sound device ------------------------------------------------------------------------------------
// User settings (Settings > Audio). Read by the player whenever it opens a sound track.
struct DolbyOutputConfig {
    // 0 automatic (the device's speakers, at most the stream's), 1 stereo, 2 stereo matrix-encoded (Dolby Surround Lt/Rt), 3 5.1, 4 7.1
    std::atomic<int> layout{0};
    std::atomic<bool> passthrough{false};   // send AC-3 / E-AC-3 untouched as IEC 61937 (experimental, untested on hardware)
    std::string device() const { std::lock_guard<std::mutex> g(mu_); return device_; }   // the bitstream device ("" = default output)
    void setDevice(const std::string& d) { std::lock_guard<std::mutex> g(mu_); device_ = d; }
    static DolbyOutputConfig& global();
private:
    mutable std::mutex mu_;
    std::string device_;
};
// The channel count to play: layout setting, the stream's channels and the device's (0 unknown = stereo). Always 2, 6 or 8.
int dolbyOutputChannels(int layoutSetting, int streamChannels, int deviceChannels);

// ---- IEC 61937 (IEC 61937-1 / -3): AC-3 and E-AC-3 inside an IEC 60958 stereo 16-bit PCM stream ---------------------------------------
// The burst: Pa = 0xF872, Pb = 0x4E1F, Pc = data type (AC-3: 0x01 | bsmod << 8; E-AC-3: 0x15), Pd = payload length (AC-3: bits,
// E-AC-3: bytes), then the payload as 16-bit words (the first byte of the frame in the high half), then zeros to the repetition period:
// AC-3 1536 IEC 60958 frames (link at the stream rate), E-AC-3 6144 frames (link at four times the stream rate, six blocks per burst).
// Output: interleaved stereo int16 words, i.e. word k of the burst is sample k (left, right, left, ...). A device writes them as S16 LE.
struct IecBurst { std::vector<int16_t> words; int dataType = 0; int lengthCode = 0; };
class Iec61937Packer {
public:
    enum Kind { None = 0, Ac3 = 1, Eac3 = 2 };
    static constexpr uint16_t kPa = 0xF872, kPb = 0x4E1F;
    // Takes whole sync frames (one or several in a row, e.g. a demuxed packet). Appends the finished bursts.
    void push(const uint8_t* p, size_t n, std::vector<IecBurst>& out);
    void flush(std::vector<IecBurst>& out);   // the E-AC-3 frames still collected, as a burst (the end of a stream)
    void reset() { acc_.clear(); accBlocks_ = 0; kind_ = None; rate_ = 0; }
    Kind kind() const { return kind_; }
    int streamRate() const { return rate_; }
    int linkRate() const { return kind_ == Eac3 ? rate_ * 4 : rate_; }   // IEC 60958 frame rate of the link
    static int repetitionPeriod(Kind k) { return k == Eac3 ? 6144 : k == Ac3 ? 1536 : 0; }   // IEC 60958 frames per burst
    // Packs one payload into a burst of `period` frames. false when it does not fit (bit rate too high).
    static bool pack(const uint8_t* payload, size_t n, int dataType, int lengthCode, int period, IecBurst& b);
    uint64_t dropped() const { return dropped_; }
private:
    std::vector<uint8_t> acc_;   // E-AC-3: frames of the current six-block period
    int accBlocks_ = 0;
    Kind kind_ = None;
    int rate_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace dect2
