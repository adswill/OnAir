// The optional-libraries list (docs/OPTIONAL-LIBRARIES.md): the file-name matcher and the list itself.
#include "dect2/codec/optional_libs.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace dect2::codec;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    CHECK(optionalNameMatches("libfaad.so*", "libfaad.so.2"));
    CHECK(optionalNameMatches("libfaad.so*", "libfaad.so"));
    CHECK(!optionalNameMatches("libfaad.so*", "libfaad_hdc.so.2"));
    CHECK(optionalNameMatches("FOBOS_SDR.DLL", "fobos_sdr.dll"));
    CHECK(optionalNameMatches("libairspy.*dylib", "libairspy.0.dylib"));
    CHECK(!optionalNameMatches("libairspy.*dylib", "libairspyhf.0.dylib"));
    CHECK(optionalNameMatches("onair-codec-*.dll", "onair-codec-ambe2.dll"));
    CHECK(!optionalNameMatches("onair-codec-*.dll", "onair-codec-ambe2.so"));

    // a library placed in the user codecs folder is reported as found there (an isolated data folder)
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "onair_optlibs_test";
    fs::remove_all(root);
    fs::create_directories(root / "codecs");
#if defined(_WIN32)
    const char* file = "fobos_sdr.dll";
    _putenv_s("DECT2_DATA_DIR", root.string().c_str());
#else
#if defined(__APPLE__)
    const char* file = "libfobos_sdr.0.dylib";
#else
    const char* file = "libfobos_sdr.so.0";
#endif
    setenv("DECT2_DATA_DIR", root.string().c_str(), 1);
#endif
    std::ofstream(root / "codecs" / file) << "x";
    bool sawFobos = false, sawLame = false;
    for (const auto& o : optionalLibraries()) {
        CHECK(!o.files.empty() && !o.source.empty() && !o.where.empty() && !o.name.empty());
        if (o.id == "fobos") { sawFobos = true; CHECK(o.found); CHECK(o.foundPath.find("codecs") != std::string::npos); }
        if (o.id == "lame") sawLame = true;
    }
    CHECK(sawFobos && sawLame);
    fs::remove_all(root);
    printf(fails ? "optional_libs: %d FAILED\n" : "optional_libs: all passed\n", fails);
    return fails ? 1 : 0;
}
