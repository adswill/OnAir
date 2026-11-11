// Dolby AC-4 audio (ATSC 3.0 in the US and South Korea, some DVB) through an OPTIONAL FFmpeg that has an "ac4" decoder.
// The FFmpeg inside OnAir has no AC-4 decoder (nobody has merged one: only third-party patches exist, see docs/CODECS.md), and OnAir ships
// no patented codec. So, as with mbelib and codec2: the user may put libavcodec + libavutil, built from a FFmpeg that includes an AC-4
// decoder, into the OnAir codecs folder. This file loads them at run time (dlopen / LoadLibrary); without them everything else works and
// the AC-4 audio of a service is dropped, with a note in the receiver tab.
//
// The loaded libraries are used through a handful of functions only, and the structures are touched only in their stable first fields
// (AVPacket data/size, AVFrame data/extended_data/nb_samples/format); the rest goes through AVOptions. That is what lets a FFmpeg of a
// different major version than the one OnAir was built with be used (a 6.1 build with the patch, for example).
//
// Search: DECT2_AC4_DIR (a folder; development and tests), then the OnAir codecs folders. In each folder the newest libavcodec[.N].dylib /
// libavcodec.so[.N] / avcodec-N.dll is tried, with the libavutil of the same folder loaded first.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// ---- signalling -------------------------------------------------------------------------------------------------------------------

enum class AudioCodecKind { Unknown, Aac, Ac3, Eac3, Ac4, MpegH, Opus };
// The kind of audio an RFC 6381 "codecs" value (a DASH Representation@codecs, ATSC A/331 USD/MPD) names: "mp4a.40.2", "ac-3", "ec-3",
// "ac-4.02.01.01", "mhm1.0x0D". Several codecs separated by commas: the first audio one wins.
AudioCodecKind audioCodecFromCodecs(const std::string& codecs);
const char* audioCodecName(AudioCodecKind k);   // "AAC", "AC-3", "E-AC-3", "AC-4", "MPEG-H", "Opus", "unknown"

// "ac-4.<bitstream_version>.<presentation_version>.<mdcompat>" (ETSI TS 103 190-2 annex E.13, as used in ATSC A/342-2 and DASH-IF).
struct Ac4Codecs { bool valid = false; int bitstreamVersion = -1, presentationVersion = -1, mdcompat = -1; };
Ac4Codecs parseAc4Codecs(const std::string& codecs);
// Every codecs="..." value of an MPD (DASH manifest) that names AC-4, in document order
std::vector<std::string> mpdAc4Codecs(const std::string& mpd);
// Every audio codec named by an MPD (AdaptationSet / Representation codecs and mimeType audio), as kinds, without repeats
std::vector<AudioCodecKind> mpdAudioKinds(const std::string& mpd);

// The AC-4 decoder specific info of the ISO-BMFF 'dac4' box (ETSI TS 103 190-2 E.6, ac4_dsi_v1)
struct Ac4Dsi {
    bool valid = false;
    int dsiVersion = -1, bitstreamVersion = -1, fsIndex = -1, frameRateIndex = -1, presentations = 0;
    std::vector<uint8_t> raw;   // the box payload, for parseAc4DsiV1() (dolby_audio.h: presentations, immersive / Atmos)
    int sampleRate() const { return fsIndex == 1 ? 48000 : fsIndex == 0 ? 44100 : 0; }
};
// `payload` is the content of the box (after its 8 byte header)
Ac4Dsi parseDac4(const uint8_t* payload, size_t size);
// Walks an ISO-BMFF initialization segment (ftyp+moov) and reports the AC-4 sample entry ('ac-4') of a sound track, if there is one.
// `channels` is the channel count of the sample entry (a hint only), `dsi` the 'dac4' box when present.
bool findAc4InInit(const uint8_t* init, size_t size, Ac4Dsi* dsi = nullptr, int* channels = nullptr);

// ---- the optional library -----------------------------------------------------------------------------------------------------------

struct Ac4LibInfo {
    bool found = false;        // a libavcodec and a libavutil were loaded
    bool hasDecoder = false;   // and it has an AC-4 decoder
    std::string path, utilPath;
    int avcodecVersion = 0;    // libavcodec_version(): major << 16 | minor << 8 | micro
    std::string message;       // what is wrong, in plain words; empty when all is well
};
// Looks once (later calls give the same answer unless rescan is true: "Look again"). Thread-safe.
Ac4LibInfo ac4Library(bool rescan = false);
inline bool ac4Available() { return ac4Library().hasDecoder; }
// For tests: look only in this folder (and not in the codecs folders); empty = back to the normal search. Forces a new search.
void ac4SetLibraryDir(const std::string& dir);
// For tests: the name of the decoder looked up in the library ("ac4" by default). A test can borrow another decoder (ac3) of a stock FFmpeg.
void ac4SetDecoderName(const std::string& name);
// Folders and file names searched (for the help card)
std::vector<std::string> ac4SearchFolders();

// Decodes AC-4 access units (ISO-BMFF samples or sync frames 0xAC40/0xAC41) with the loaded library.
class Ac4Decoder {
public:
    Ac4Decoder();
    ~Ac4Decoder();
    Ac4Decoder(const Ac4Decoder&) = delete;
    Ac4Decoder& operator=(const Ac4Decoder&) = delete;
    // false: no library or no decoder (error() says why)
    bool open();
    bool isOpen() const;
    // Decodes one access unit. Returns the number of sample frames appended to `pcm` (interleaved float, -1..1) or 0 when the decoder
    // wanted more data (or waits for an I-frame), or -1 on error. channels/sampleRate describe the output.
    int decode(const uint8_t* data, size_t size, std::vector<float>& pcm, int& channels, int& sampleRate);
    void flush();
    std::string error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---- AC-4 -> AC-3 for the transport stream ---------------------------------------------------------------------------------------
// The transport stream that feeds the player must carry something the built-in FFmpeg can decode. The AC-4 audio is decoded with the
// optional library, mixed to stereo or 5.1 (by the channel count of the sample entry), and encoded as AC-3 by the FFmpeg inside OnAir.
struct Ac4TsPacket { std::vector<uint8_t> data; int64_t pts90 = 0; int samples = 0; };

class Ac4ToAc3 {
public:
    Ac4ToAc3();
    ~Ac4ToAc3();
    Ac4ToAc3(const Ac4ToAc3&) = delete;
    Ac4ToAc3& operator=(const Ac4ToAc3&) = delete;
    // channelsHint: the sample entry's channel count (>= 6 gives a 5.1 output, otherwise stereo)
    bool open(int channelsHint);
    int outChannels() const;
    int outSampleRate() const;   // 48000
    int outBitRate() const;
    // `pts90` is the time of the access unit in 90 kHz units. Appends the AC-3 frames that became complete.
    void push(const uint8_t* data, size_t size, int64_t pts90, std::vector<Ac4TsPacket>& out);
    std::string error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---- what the receiver tab shows -------------------------------------------------------------------------------------------------------
struct Ac4Status {
    bool seen = false;            // a service with AC-4 audio was received in this run
    std::string detail;           // "AC-4, 48 kHz, 2 presentations, 6 channels" (from the init segment / MPD)
    bool libFound = false, hasDecoder = false;
    std::string libPath, libMessage;
    uint64_t accessUnits = 0, decoded = 0, failed = 0;
    int channels = 0, sampleRate = 0;
    bool dropped = false;         // the audio was dropped for want of the library
};
Ac4Status ac4Status();
void ac4NoteSeen(const std::string& detail);       // called by the remuxer / receiver
void ac4NoteDropped();
void ac4NoteDecode(bool ok, int channels, int rate);
void ac4StatusReset();

} // namespace dect2
