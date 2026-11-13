// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// User-installed speech codecs for the voice modes (TETRA voice now, Iridium voice later). OnAir ships no speech codec: a codec is a
// plug-in file the user puts in the codecs folder under the app data folder, and it is loaded only after the user has accepted the legal
// notice. See docs/CODECS.md. Nothing here knows any particular codec.
//
// Plug-in ABI (C, so it does not depend on the compiler): the file exports these functions. A codec is a shared library named
// onair-codec-<id>.dll / .dylib / .so (speechCodecFileName).
//
//   void*   onair_speech_open(void);                     // a new decoder state, nullptr on failure
//   void    onair_speech_close(void* state);
//   int     onair_speech_sample_rate(void);              // PCM sample rate in Hz, e.g. 8000
//   int     onair_speech_decode(void* state, const uint8_t* coded, size_t nbytes,
//                               int16_t* pcm, size_t max_samples);
//                                                        // one coded frame in, PCM samples out; returns the count, -1 on error
//   int     onair_speech_abi(void);                      // optional: returns 1 (kSpeechAbi); a plug-in that returns another
//                                                        // number is refused with a plain-words message instead of being called
// Every call into a plug-in runs on the codec guard (codec_guard.h): one that crashes or hangs is disabled, OnAir goes on.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::codec {

constexpr int kSpeechAbi = 1;   // the plug-in interface version (onair_speech_abi)

// What a voice mode uses: one decoder, one coded frame at a time.
class SpeechCodec {
public:
    virtual ~SpeechCodec() = default;
    virtual int sampleRate() const = 0;   // PCM sample rate in Hz
    // Decodes one coded frame into PCM. Empty when the frame could not be decoded.
    virtual std::vector<int16_t> decode(const std::vector<uint8_t>& coded) = 0;
};

enum class CodecStatus {
    Ready,          // the codec is loaded and decode() works
    NotInstalled,   // no plug-in file in the codecs folder
    NoticeRequired, // the plug-in is there, but the user has not accepted the legal notice yet
    LoadFailed,     // the file is there but would not open or does not have the functions
};

struct CodecLookup {
    CodecStatus status = CodecStatus::NotInstalled;
    std::string path;      // the plug-in file that was found (empty when not installed)
    std::string message;   // English text for the voice mode to show
    std::string folder;    // the folder the plug-in was found in (empty when not installed)
    std::vector<std::string> shadowed;   // other searched folders that hold a file with the same name; they are not used
};

// The legal notice the user accepts once per codec. English; the same text for every codec.
std::string speechCodecNotice();
// The file name a plug-in must have for this codec id on this OS, e.g. "onair-codec-tetra.dylib".
std::string speechCodecFileName(const std::string& id);
// Opens a plug-in file and returns its decoder, or nullptr with err set. Test code replaces it.
using CodecOpener = std::function<std::unique_ptr<SpeechCodec>(const std::string& path, std::string& err)>;

// Looks for the plug-in for `id` in `folder` and, when the notice has been accepted (noticeAccepted), opens it. The notice is checked
// only after the plug-in is found, so a missing codec never asks for it. `opener` defaults to the shared-library loader.
CodecLookup loadSpeechCodec(const std::string& id, const std::string& folder, bool noticeAccepted,
                            std::unique_ptr<SpeechCodec>& out, const CodecOpener& opener = {});

// The same, searching several folders in order (codec_paths.h: user folder, extra, ONAIR_CODEC_DIR, next to the program). The first folder
// whose plug-in loads wins; a plug-in in a later folder is only listed in CodecLookup::shadowed. `folders` are plain paths.
CodecLookup loadSpeechCodecInDirs(const std::string& id, const std::vector<std::string>& folders, bool noticeAccepted,
                                  std::unique_ptr<SpeechCodec>& out, const CodecOpener& opener = {});

} // namespace dect2::codec
