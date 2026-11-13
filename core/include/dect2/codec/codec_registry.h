// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One registry of every voice codec OnAir knows, whether it is open or patented, and the plug-ins the app has loaded. A patented codec is never
// shipped: the app loads onair-codec-<id> from the user's codecs folder (speech_codec.h) and hands the receivers a factory made here. Receivers
// of the AMBE / IMBE family ask pluginVocoder() for a decoder and fall back to the user's own mbelib when there is none. See docs/CODECS.md.
//
// Plug-in frame formats (the bytes handed to onair_speech_decode, 8 kHz PCM, 160 samples per call, as for TETRA):
//   ambe2, ambe : 12 bytes = the four de-interleaved code words C0..C3 of mbelib, 24 bits each, row by row, most significant bit first
//   imbe        : 23 bytes = the eight code words c0..c7 of mbelib, 23 bits each, row by row, most significant bit first (184 bits)
//   tetrapol    : 16 bytes = the 120 RP-CELP bits of one 20 ms voice frame, most significant bit first, then a flag byte (bit 0: damaged
//                 frame); see tetrapol/tetrapol_voice.h
#pragma once
#include "dect2/codec/speech_codec.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::codec {

enum class CodecKind { Open, Patented, Unsupported };

struct CodecInfo {
    const char* id;          // plug-in id, e.g. "ambe2"; empty for a codec that needs none
    const char* name;        // e.g. "AMBE+2"
    CodecKind kind;
    const char* modes;       // the modes that use it
    const char* note;        // what the user must do / why unsupported
};

const std::vector<CodecInfo>& codecTable();
const CodecInfo* findCodec(const std::string& id);

// The size of one coded frame for ids that use the code-word format (0 for others)
size_t pluginFrameBytes(const std::string& id);
// Packs mbelib code words (rows x cols chars of 0/1) into the plug-in frame format
std::vector<uint8_t> packCodeWords(const char* bits, int rows, int cols);

using DecoderFactory = std::function<std::unique_ptr<SpeechCodec>()>;
// The app calls this when a plug-in turned Ready (path for the status line) and with an empty factory when it is gone.
void setPluginFactory(const std::string& id, DecoderFactory f, const std::string& path = std::string());
bool pluginLoaded(const std::string& id, std::string* path = nullptr);
// A new decoder instance (one per speaker / slot) or nullptr when no plug-in is loaded.
std::unique_ptr<SpeechCodec> pluginVocoder(const std::string& id);

// The text of the install helper: what is loaded or missing and the exact file name looked for on this OS.
std::string pluginStatusLine(const std::string& id, const std::string& folder);

} // namespace dect2::codec
