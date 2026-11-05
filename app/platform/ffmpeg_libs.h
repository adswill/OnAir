// Which FFmpeg the app runs with: the one shipped with OnAir (default) or the FFmpeg installation the user chose in Settings > Advanced >
// FFmpeg libraries (prefs key "ffmpegDir"). The choice applies at the next start. Docs: docs/ffmpeg-libraries.md.
//   Linux, macOS: the program links the FFmpeg libraries dynamically. When a folder is chosen, the program starts itself once more with
//                 LD_LIBRARY_PATH / DYLD_LIBRARY_PATH set to that folder, so the dynamic loader takes the libraries from there.
//   Windows:      the FFmpeg DLLs are delay-loaded. When a folder is chosen, its DLLs are loaded (LoadLibraryEx) before any FFmpeg call,
//                 and the delay-load stubs then bind to them.
// A folder is only used when its libraries load and have the same major versions as the FFmpeg OnAir was built with (and no older minor
// versions). To know that before it counts, OnAir starts a short copy of itself with the folder (check()): it only loads FFmpeg, checks
// the versions and where each library came from, and exits. When something is wrong, the reason and a possible solution are kept for
// the Settings window and OnAir uses its own FFmpeg.
#pragma once
#include <string>
#include <vector>

namespace ffmpeglibs {

inline constexpr const char* kPrefKey = "ffmpegDir";

// First thing in main(), before any FFmpeg call. dir: the chosen folder (empty: the shipped FFmpeg). On Linux and macOS this does not
// return when the folder is usable: it restarts the program with the loader path set. Otherwise status() says why it is not used.
// It also runs the check copy of check() (and exits there).
void apply(const std::string& dir, int argc, char** argv);

// the folder that holds the FFmpeg libraries this program needs, dir itself or its lib/ or bin/ subfolder; empty when none
std::string findLibFolder(const std::string& dir);

// the file names this program needs, e.g. libavcodec.so.62, libavcodec.62.dylib, avcodec-62.dll
std::vector<std::string> libFileNames();

// the trial load of a folder by a short copy of this program; ok, or what is wrong and what to do about it
struct Check {
    bool ok = false;
    std::string folder;     // the folder with the libraries (dir or its lib/ or bin/)
    std::string problem;    // what is wrong
    std::string solution;   // a possible solution
};
Check check(const std::string& dir);

struct Status {
    std::string chosen;      // the folder in the settings at start ("" = shipped)
    std::string folder;      // the folder the libraries were taken from when the chosen one is in use
    std::string problem, solution;   // why the chosen folder is not used (OnAir then uses its own FFmpeg)
    bool external = false;   // the chosen libraries are in use
};
const Status& status();

// for the Settings window (calls FFmpeg): the folder libavcodec was loaded from, and the FFmpeg version
struct Info { std::string folder, version; };
Info info();

// OnAir's own FFmpeg, also while another one is in use: the folder the system would load it from (empty: not found) and its version
Info own();

// Settings: restart OnAir (after its window has closed and it has shut down; main() calls restartIfRequested() last)
void requestRestart();
bool restartRequested();
void restartIfRequested(char** argv);

}   // namespace ffmpeglibs
