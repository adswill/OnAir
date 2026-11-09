// Digital video generator (Tools > Digital video generator): the encoding settings, the encoders this FFmpeg really has, and the
// transport-stream bitrate estimate. No interface and no I/O here, so the tests can check all of it.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AVCodec;
struct AVCodecContext;
struct AVDictionary;
struct AVFrame;
struct AVPacket;

namespace dect2::vgen {

enum class Codec { Mpeg2, H264, Hevc };
enum class AudioCodec { None, Aac, Mp2, Ac3 };
enum class RateControl { Cbr, ConstrainedVbr, Crf };
enum class FrameTypes { IPB, IP, IOnly };
enum class TsContent { VideoOnly, VideoAudio, VideoMultiAudio };

// An encoder of the FFmpeg OnAir is linked with that opened when it was tried (videoEncoders() / audioEncoders())
struct EncoderInfo {
    std::string name;    // FFmpeg's name: libx264, h264_nvenc, mpeg2video, aac ...
    std::string label;   // for the list: "H.264 (libx264)"
    Codec codec = Codec::Mpeg2;
    AudioCodec audio = AudioCodec::None;
    bool hardware = false;   // h264_mf / hevc_mf: true when the hardware encoder of the graphics card opened, false for Windows' own
};

// An encoder this FFmpeg has that did not work on this computer when it was tried, and why (shown greyed out in the list)
struct EncoderProblem {
    std::string name;     // "h264_nvenc", "h264_mf (hardware)" ...
    std::string reason;   // "does not open: Function not implemented"
};

// What an encoder understands; the window greys out the rest. Preset, tune and CRF are x264 / x265 names (hardware encoders have their own).
struct EncoderCaps {
    bool preset = false, zeroLatency = false, profile = false, bFrames = false, crf = false;
};

constexpr const char* kPresets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow"};
constexpr int kNumPresets = 7;
constexpr const char* kProfiles[] = {"baseline", "main", "high"};
constexpr int kNumProfiles = 3;

struct Settings {
    TsContent content = TsContent::VideoAudio;
    // video
    std::string videoEncoder = "libx264";   // falls back to mpeg2video when this FFmpeg does not have it
    RateControl rc = RateControl::Cbr;
    bool autoBitrate = true;                // the video bitrate follows suggestVideoKbps() when the picture settings change
    int videoKbps = 1000;
    int crf = 23;
    double fps = 15;
    int width = 720, height = 576;
    double gopSeconds = 2;
    FrameTypes frames = FrameTypes::IPB;
    int profile = 1;                        // kProfiles
    int preset = 2;                         // kPresets: veryfast
    bool zeroLatency = false;
    // audio
    std::string audioEncoder = "aac";
    int audioTracks = 2;                    // VideoMultiAudio only (2..4); every track carries the same sound
    int audioKbps = 96, sampleRate = 48000, channels = 1;
    // transport stream
    bool autoMux = true;                    // mux rate = suggested total; otherwise muxKbps
    int muxKbps = 1300;
    int serviceId = 1;
    bool autoPids = true;
    int pmtPid = 0x1000, videoPid = 0x100, audioPid = 0x101;   // audio track n is on audioPid + n
    std::string serviceName = "OnAir video", provider = "OnAir";
};

// The encoders, tried once (opening hardware encoders takes a moment: call from a worker thread). MPEG-2 video is always offered first
// when present; the bundled LGPL FFmpeg has mpeg2video, mp2, aac and ac3 only.
const std::vector<EncoderInfo>& videoEncoders();
const std::vector<EncoderInfo>& audioEncoders();
const std::vector<EncoderProblem>& videoEncoderProblems();   // tried together with videoEncoders()
bool encoderHardware(const std::string& encoder);       // as videoEncoders() found it (h264_mf: hardware or software)
Codec codecOf(const std::string& encoder);              // by name, for any name (the estimate works without trying the encoder)
AudioCodec audioCodecOf(const std::string& encoder);
EncoderCaps capsOf(const std::string& encoder);
// The encoder to use for `wanted`: itself when it is in the list, else one for the same codec, else mpeg2video
std::string pickVideoEncoder(const std::string& wanted, const std::vector<EncoderInfo>& have);
const char* codecName(Codec c);                         // "MPEG-2", "H.264", "H.265"
const char* audioCodecName(AudioCodec c);               // "AAC", "MPEG-1 Layer II", "AC-3"
int audioTrackCount(const Settings& s);                 // 0 for video only

// The text of the encoder's own "profile" option for kProfiles[p] ("constrained_baseline" for AMF's Baseline), or empty when the
// encoder has no such option and takes the number in AVCodecContext::profile instead (h264_mf, libopenh264): FFmpeg's generic
// "profile" option does not know the names and makes avcodec_open2 fail with "Invalid argument".
std::string profileName(const std::string& encoder, int p);
// Sets up an encoder context (allocated for `c`) and its options for the settings at W x H and fpsNum / fpsDen, the same for the trial
// when the encoders are listed and for the start. full: with the profile, B-frames and the encoder's own rate-control mode; without
// them is the second try for an encoder that refuses them (hardware HEVC without B-frames, an older card). hwMode: the hardware
// variant of h264_mf / hevc_mf. False when the encoder takes neither YUV 4:2:0 nor NV12 pictures from memory (VAAPI wants GPU frames).
bool setupVideoEncoder(AVCodecContext* cx, AVDictionary** opts, const AVCodec* c, const std::string& encoder, bool hwMode, const Settings& s,
                       int W, int H, int fpsNum, int fpsDen, bool full);
// avcodec_send_frame for an encoder that can be busy: a hardware encoder answers EAGAIN until its packets are taken, so `take` gets
// them until the frame goes in (at most about 2 s). frame nullptr ends the stream and hands over every packet up to the end.
// 0 or FFmpeg's error.
int sendToEncoder(AVCodecContext* cx, const AVFrame* frame, AVPacket* pk, const std::function<void(AVPacket*)>& take);

// Bits per pixel for a watchable picture: a rule of thumb per codec and frame-type set, not a measurement
double bitsPerPixel(Codec c, FrameTypes f);
// MPEG-2 sends with each picture how long the decoder waits for it (vbv_delay: 16 bits of 90 kHz, at most about 0.73 s), and in CBR
// FFmpeg's mpeg2video aborts the program when a picture takes longer than that at the bitrate. Even at its lowest quality a camera or
// still picture needs about 160 bit/s per macroblock (16x16 pixels) to stay within it, pure noise about 900 (measured with FFmpeg 8).
constexpr int kMpeg2MinBitsPerMb = 160;   // the lowest MPEG-2 video bitrate the generator allows
constexpr int kMpeg2CbrBitsPerMb = 900;   // below this, MPEG-2 CBR writes VBR headers (vbv_delay 0xFFFF), which no picture can overflow
int macroblocks(int width, int height);
// The lowest video bitrate in kbit/s: kMpeg2MinBitsPerMb for MPEG-2 (rounded up to 10), 10 for the other codecs
int minVideoKbps(const Settings& s);
bool mpeg2CbrHeaders(int width, int height, int64_t bitrate);   // bitrate in bit/s
// The video bitrate the picture settings need, in kbit/s (rounded to 10)
int suggestVideoKbps(const Settings& s);
int effectiveVideoKbps(const Settings& s);              // autoBitrate ? suggestion : videoKbps, never below minVideoKbps

// The transport-stream budget in kbit/s: what each part adds and the mux rate it needs
struct Budget {
    double video = 0;        // mean video rate
    double videoPeak = 0;    // what the mux must carry at the worst moment (constrained VBR / CRF go above the mean)
    double audio = 0;        // all tracks
    double pesTs = 0;        // PES headers, TS headers, adaptation fields and PCR
    double psiSi = 0;        // PAT, PMT and SDT repeated
    double total = 0;        // video + audio + pesTs + psiSi
    double peak = 0;         // the same with videoPeak
    int suggestedMux = 0;    // peak plus 3 % for null packets, rounded up to 10 kbit/s
    int mux = 0;             // the mux rate used: suggestedMux or the user's
};
Budget estimate(const Settings& s);
// Plain warnings for settings that will not work as asked (empty when fine)
std::vector<std::string> checkSettings(const Settings& s, const Budget& b);

// The parameters an overlay block can show (the rows of the encoding table)
enum class Param { VideoCodec, VideoBitrate, Resolution, FrameRate, Gop, FrameTypes, Profile, RateControl, PixelFormat, Preset, Latency,
                   AudioCodec, AudioBitrate, SampleRate, Channels, TsContent, MuxRate, TotalRate, Service, Count };
const char* paramLabel(Param p);                        // English, marked for translation where it is drawn
std::string paramValue(Param p, const Settings& s);     // "720x576", "15 fps", "1000 kbit/s" ...

// The settings as "key=value" lines and back (unknown keys are skipped, missing ones keep their default)
std::string saveSettings(const Settings& s);
void loadSettings(const std::string& text, Settings& s);

} // namespace dect2::vgen
