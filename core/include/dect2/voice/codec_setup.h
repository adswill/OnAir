// The Codec Setup assistant's logic (Help > Voice codecs > Setup): which voice codec plug-ins are installed, loaded or wrong for this
// computer, and the step-by-step instructions per operating system. Pure functions of what the caller found on disk, so they are
// unit-tested with made-up folders. OnAir never ships, downloads or builds a codec by itself: the steps tell the user to run the optional
// build helper (tools/codec-builder, docs/CODECS-BUILD-HELPER.md) on their own computer.
#pragma once
#include "dect2/codec/codec_paths.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2::voice {

enum class SetupOs { Windows, MacOS, Linux };
SetupOs currentSetupOs();
const char* setupOsName(SetupOs os);

enum class SetupState {
    Loaded,        // the plug-in is loaded and decoding
    Ready,         // the file is in a codecs folder (right processor); it loads when a voice mode starts and the notice is accepted
    WrongArch,     // the file is there but built for another processor than OnAir (macOS)
    Missing,       // no file in any searched folder
};

struct SetupItem {
    std::string id;          // "ambe2"
    std::string name;        // "AMBE+2"
    std::string modes;       // "DMR, NXDN, dPMR"
    std::string fileName;    // "onair-codec-ambe2.dylib"
    SetupState state = SetupState::Missing;
    std::string where;       // the folder the file was found in (the first one wins) or the loaded path
    std::string detail;      // plain-words status for the screen
    std::vector<std::string> ignoredCopies;   // the same file in later folders (not used)
};

struct SetupProbe {                              // what the caller can see; each member may be left empty in a test
    std::function<bool(const std::string&)> fileExists;
    std::function<std::string(const std::string&)> archOfFile;   // the processor(s) a file is built for ("" when not known or not applicable)
    std::function<bool(const std::string&, std::string*)> loaded;   // is plug-in `id` loaded; its path
    std::string processArch;                     // "arm64"
};

// The patented voice plug-ins (ambe2, ambe, imbe, tetra, tetrapol, ysf, ...) from the codec registry, one item each.
std::vector<SetupItem> codecSetupStatus(const std::vector<codec::CodecDir>& dirs, const SetupProbe& probe);

// "3 of 5 installed" style summary
std::string setupSummary(const std::vector<SetupItem>& items);
// true when every voice codec OnAir can use through a plug-in is installed (nothing missing or wrong)
bool setupComplete(const std::vector<SetupItem>& items);

// The numbered steps for one OS (the same wording on the screen and in docs/CODECS.md), including the exact command to run
std::vector<std::string> setupSteps(SetupOs os);
// The command that runs the build helper from the folder where the user saved the OnAir repository
std::string setupBuildCommand(SetupOs os);

} // namespace dect2::voice
