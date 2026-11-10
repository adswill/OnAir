// TETRAPOL voice: the encryption gate and the hand-over of clear voice frames to the user-installed speech codec (docs/CODECS.md).
// OnAir ships no TETRAPOL codec (RP-CELP, PAS 0001-7, patented): the plug-in onair-codec-tetrapol is installed by the user and loaded
// through the speech-codec plug-in interface (codec/speech_codec.h) after the user accepted the notice.
//
// The gate: a voice frame goes to the codec only when this call's own signalling, read in the clear on this channel, carried a key
// reference whose key index is 0 (clear call, PAS 0001-3-2). A key index other than 0 is an encrypted call (sticky for the call); no key
// reference at all (a call joined late, a lost D_CALL_START) is "encryption state unknown". Both stay silent. Nothing is ever decrypted,
// no key is used, guessed or stored.
//
// Plug-in frame (the bytes given to onair_speech_decode, one 20 ms voice frame, 16 bytes):
//   bytes 0..14: the 120 codec bits, bit k (k = 0..119: voice1[0..19] then voice2[0..99] of PAS 0001-2 6.1, the PAS 0001-7 numbering)
//                in byte k / 8, mask 0x80 >> (k % 8)
//   byte 15:     bit 0 set when the frame is damaged (the CRC-3 of the protected part failed): the codec should conceal it; other bits 0
// The plug-in answers with 160 samples of 8 kHz PCM (20 ms); another rate is resampled by the app.
#pragma once
#include "dect2/tetrapol/tetrapol_codec.h"
#include "dect2/tetrapol/tetrapol_link.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {
namespace tetrapol {

constexpr size_t kPluginFrameBytes = 16;
void packPluginFrame(const VoiceFrame& v, bool damaged, uint8_t out[kPluginFrameBytes]);

enum class Verdict { Unknown = 0, Clear = 1, Encrypted = 2 };
const char* verdictText(Verdict v);

// The call on the traffic channel being received and what its signalling said about encryption.
class CallGate {
public:
    static constexpr double kEndSec = 1.0;      // no voice frame for this long: the call is over
    static constexpr double kPendingSec = 3.0;  // a D_CALL_START / D_CALL_CONNECT this recent applies to the call that starts
    // a signalling message heard on this channel
    void onSignal(const CallSignal& s, double now);
    // a voice frame; returns the verdict for it. started is set when this frame began a new call.
    Verdict onVoice(double now, bool& started);
    // the key reference a control channel announced for this traffic channel (D_GROUP_ACTIVATION), -1 none: used when the call's own
    // signalling on this channel carries none
    void setChannelKey(int keyRef) { chanKey_ = keyRef; }
    // ends the call when it has been quiet (call from the frame clock); returns true when a call just ended
    bool tick(double now);
    bool active() const { return active_; }
    Verdict verdict() const { return verdict_; }
    void reset() { *this = CallGate(); }
private:
    bool active_ = false;
    Verdict verdict_ = Verdict::Unknown;
    double last_ = -1e9;
    int pendingKey_ = -2;          // -2 none, -1 a message without key reference, else the key reference
    double pendingAt_ = -1e9;
    bool ended_ = false;
    int chanKey_ = -1;
};

// One WAV file (16-bit mono PCM) per clear call. The header is rewritten when the file is closed.
class WavWriter {
public:
    ~WavWriter() { close(); }
    bool open(const std::string& path, int rate);
    void write(const int16_t* pcm, size_t n);
    void close();
    bool isOpen() const { return f_ != nullptr; }
    const std::string& path() const { return path_; }
    double seconds() const { return rate_ > 0 ? (double)samples_ / rate_ : 0; }
private:
    std::FILE* f_ = nullptr;
    std::string path_;
    int rate_ = 0;
    uint64_t samples_ = 0;
};

// A known pattern for the 120 codec bits of voice frame n (the test signal: there is no free RP-CELP encoder)
void knownVoiceBits(uint32_t n, uint8_t out[kVoiceBits]);

} // namespace tetrapol
} // namespace dect2
