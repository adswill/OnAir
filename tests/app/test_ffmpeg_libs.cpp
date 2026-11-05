// Settings > Advanced > FFmpeg libraries (app/platform/ffmpeg_libs.h): the folder lookup (the folder itself, its lib/ or bin/), the trial
// load by a short copy of the program (a working FFmpeg passes; files that are not FFmpeg libraries, another major version or a missing
// folder give a problem and a solution), and an unusable choice leaves the shipped FFmpeg in use.
#include "../../app/platform/ffmpeg_libs.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
static void touch(const fs::path& p) { fs::create_directories(p.parent_path()); std::ofstream(p) << "not a library"; }
int main(int argc, char** argv) {
    ffmpeglibs::apply("", argc, argv);   // as in the app: the check copy of ffmpeglibs::check() runs (and ends) here
    const std::vector<std::string> names = ffmpeglibs::libFileNames();
    check(names.size() == 5, "five libraries");
    const fs::path root = fs::temp_directory_path() / ("onair-fflibs-test-" + std::to_string((unsigned long long)fs::file_time_type::clock::now().time_since_epoch().count()));
    check(ffmpeglibs::findLibFolder("").empty(), "empty folder: none");
    // all five in the folder itself, or in bin/ (Windows builds) or lib/ (Linux, macOS)
    for (const std::string& n : names) { touch(root / "flat" / n); touch(root / "win" / "bin" / n); touch(root / "nix" / "lib" / n); }
    check(fs::path(ffmpeglibs::findLibFolder((root / "flat").string())) == root / "flat", "libraries in the folder itself");
    check(fs::path(ffmpeglibs::findLibFolder((root / "win").string())) == root / "win" / "bin", "bin subfolder");
    check(fs::path(ffmpeglibs::findLibFolder((root / "nix").string())) == root / "nix" / "lib", "lib subfolder");

    ffmpeglibs::Check c = ffmpeglibs::check((root / "nothere").string());
    check(!c.ok && c.problem.find("does not exist") != std::string::npos && !c.solution.empty(), "missing folder: problem and solution");
    c = ffmpeglibs::check((root / "flat").string());   // the right names, but not libraries: the trial load fails
    printf("files that are not libraries: %s | %s\n", c.problem.c_str(), c.solution.c_str());
    check(!c.ok && !c.problem.empty() && !c.solution.empty(), "not libraries: problem and solution");
    // another major version: other file names
#if defined(_WIN32)
    touch(root / "other" / "bin" / "avcodec-99.dll");
#elif defined(__APPLE__)
    touch(root / "other" / "lib" / "libavcodec.99.dylib");
#else
    touch(root / "other" / "lib" / "libavcodec.so.99");
#endif
    c = ffmpeglibs::check((root / "other").string());
    printf("another version: %s | %s\n", c.problem.c_str(), c.solution.c_str());
    check(!c.ok && c.problem.find("libavcodec 99") != std::string::npos, "another major version is named");

    // the FFmpeg this program runs with passes
    const ffmpeglibs::Info i = ffmpeglibs::info();
    printf("FFmpeg in use: %s in %s\n", i.version.c_str(), i.folder.c_str());
    check(!i.version.empty() && !i.folder.empty(), "version and folder of the FFmpeg in use");
    c = ffmpeglibs::check(i.folder);
    if (!c.ok) printf("  %s | %s\n", c.problem.c_str(), c.solution.c_str());
    check(c.ok, "the FFmpeg in use passes the trial load");

    // an unusable choice at start: the shipped FFmpeg stays, with the reason
    ffmpeglibs::apply((root / "flat").string(), argc, argv);
    check(!ffmpeglibs::status().external && !ffmpeglibs::status().problem.empty(), "unusable choice: the shipped FFmpeg and a reason");
    std::error_code ec;
    fs::remove_all(root, ec);
    std::fputs(fails ? "FAILED\n" : "OK\n", stdout);
    return fails ? 1 : 0;
}
