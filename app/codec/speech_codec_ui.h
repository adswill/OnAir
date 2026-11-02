// The app side of user-installed speech codecs: the codecs folder, the accepted-notice flag in prefs, and the notice dialog a voice
// mode draws until its codec is ready. English only. See docs/CODECS.md.
#pragma once
#include "dect2/codec/speech_codec.h"
#include "dect2/codec/codec_paths.h"
#include <vector>
#include <memory>
#include <string>

struct App;

namespace appcodec {

// The user folder (the first one searched, and the one "Open codecs folder" opens): the app data folder + "/codecs".
std::string codecsFolder();
// Every folder the plug-ins and libraries are looked for in, in order, with where each comes from (codec_paths.h): the user folder, the
// extra folder set in Voice codecs, ONAIR_CODEC_DIR, then the folders next to the program.
std::vector<dect2::codec::CodecDir> codecSearchFolders();
std::vector<std::string> codecSearchPaths();
// Saves the extra folder (empty clears it) and makes every loader look again.
void setExtraFolder(const std::string& dir);
// "Open codecs folder" (user folder) plus "Open folder next to OnAir" and "Open extra folder" when those exist, on the current line.
bool openCodecFolderButtons();
// Lists the searched folders for plug-in `codec`, marks where the file is, and warns when it is in more than one.
void codecFoldersList(const std::string& codec);

// Tries to load the speech codec `id` without drawing anything, so a voice mode can call it from its per-frame tick and get audio whatever
// tab is shown. Retries a missing or failed plug-in at most once a second; logs the reason when it changes. `out` holds the codec when Ready.
dect2::codec::CodecLookup speechCodecPoll(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out);

// Draws the state of the speech codec `id` inside a voice mode: the not-installed text (with an Open codecs folder button), or the
// legal notice dialog until the user accepts it once. Call every frame. Returns true when `out` holds a ready codec.
bool speechCodecGate(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out);

// Forgets the last verdict for `id` so the next poll looks in the folder again (the Rescan button).
void speechCodecRescan(const char* id);

// Call every frame from a voice mode: loads the plug-in `id` once it is installed and the notice accepted, and registers it in
// dect2::codec (codec_registry.h) so every receiver of that family decodes through it. True when registered.
bool voiceCodecTick(const char* id);

// The install helper: loaded / not found with the exact file name, the codecs folder path, Open codecs folder, Build steps and Rescan buttons.
void codecInstallHelper(const char* id);
// OnAir's processor, the Rosetta notice (Intel build on Apple Silicon) and, with buildTip, which architecture to build plug-ins for
void codecArchLines(bool buildTip);
// Shows the codec notice popup (with "I accept") when the plug-in is installed but the notice was not accepted yet; draws nothing otherwise. Call it from a voice panel every frame.
void codecNoticePrompt(const char* id);

// The one-line voice codec status every voice panel shows (same text as DMR): green "Voice codec loaded: <name> plug-in (path)" or amber
// "Voice codec missing: ...", on the current line; it also opens the legal notice dialog when the plug-in is there but not accepted yet.
// `libPath` is the path of a plain libmbe when that is what decodes (empty if none). Put the "?" button on the same line after it.
void voiceCodecLine(const char* id, const char* name, const std::string& libPath = std::string());

// The "Voice codecs" window (Help menu): every voice codec OnAir knows, the modes that use it, whether it is open, patented (plug-in only,
// never shipped) or unsupported, and whether it is loaded or missing, with the install helper for each plug-in.
void voiceCodecsWindow(App& a);

// The setup wizard's codec screen (wizard.cpp): one status line per voice codec plug-in, Open codecs folder, "Look again" (searches again, no restart),
// Copy build command and the install steps for this computer. Texts go through tr(). OnAir never ships or downloads a patented codec.
void wizardCodecPanel();

} // namespace appcodec
