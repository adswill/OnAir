// The Codec Setup assistant's logic (voice/codec_setup.h) on made-up folders: missing, installed, loaded, the first folder winning with
// copies in later folders listed as ignored, a plug-in built for another processor (an arm64 file in an Intel OnAir), the summary and
// completeness, and the per-OS steps naming the right command and tools.
// NOT covered: the window (app/codec/speech_codec_ui.cpp) and the build script itself (tools/codec-builder/build-codecs.sh).
#include "dect2/voice/codec_setup.h"
#include "dect2/codec/codec_registry.h"
#include <set>
#include <string>

using namespace dect2::voice;
#include "../common/test_util.h"

static const SetupItem* find(const std::vector<SetupItem>& v, const std::string& id) {
    for (const auto& i : v) if (i.id == id) return &i;
    return nullptr;
}

int main() {
    const std::vector<dect2::codec::CodecDir> dirs = {{"/home/u/codecs", "user"}, {"/opt/onair/codecs", "program"}};
    std::set<std::string> files;
    SetupProbe p;
    p.fileExists = [&](const std::string& f) { return files.count(f) > 0; };
    p.processArch = "arm64";
    std::set<std::string> loaded;
    p.loaded = [&](const std::string& id, std::string* path) { if (!loaded.count(id)) return false; if (path) *path = "/loaded/" + id; return true; };
    std::string arch;
    p.archOfFile = [&](const std::string&) { return arch; };

    auto items = codecSetupStatus(dirs, p);
    CHECK(items.size() >= 4, "the patented plug-ins of the registry: %zu", items.size());
    CHECK(find(items, "ambe2") && find(items, "ambe") && find(items, "imbe") && find(items, "tetra"), "ids");
    for (const auto& i : items) CHECK(i.state == SetupState::Missing && i.detail.find(i.fileName) != std::string::npos, "all missing at first: %s", i.id.c_str());
    CHECK(!setupComplete(items) && setupSummary(items).find("0 of") == 0, "summary %s", setupSummary(items).c_str());

    const std::string f2 = "onair-codec-ambe2" + std::string(dect2::codec::speechCodecFileName("ambe2").substr(std::string("onair-codec-ambe2").size()));
    files.insert("/opt/onair/codecs/" + f2);
    items = codecSetupStatus(dirs, p);
    CHECK(find(items, "ambe2")->state == SetupState::Ready && find(items, "ambe2")->where == "/opt/onair/codecs", "found next to the program");
    files.insert("/home/u/codecs/" + f2);
    items = codecSetupStatus(dirs, p);
    CHECK(find(items, "ambe2")->where == "/home/u/codecs" && find(items, "ambe2")->ignoredCopies.size() == 1 && find(items, "ambe2")->ignoredCopies[0] == "/opt/onair/codecs", "user folder wins, other copy ignored");
    loaded.insert("ambe2");
    items = codecSetupStatus(dirs, p);
    CHECK(find(items, "ambe2")->state == SetupState::Loaded && find(items, "ambe2")->where == "/loaded/ambe2", "loaded");

    loaded.clear();
    arch = "x86_64";   // an Intel file in an arm64 OnAir
    items = codecSetupStatus(dirs, p);
    CHECK(find(items, "ambe2")->state == SetupState::WrongArch && find(items, "ambe2")->detail.find("x86_64") != std::string::npos, "wrong processor");
    arch = "universal (arm64, x86_64)";
    items = codecSetupStatus(dirs, p);
    CHECK(find(items, "ambe2")->state == SetupState::Ready, "a universal file fits");
    arch = "";
    items = codecSetupStatus(dirs, p);
    CHECK(find(items, "ambe2")->state == SetupState::Ready, "unknown architecture is not blamed");

    for (const auto& i : items) files.insert("/home/u/codecs/" + i.fileName);
    items = codecSetupStatus(dirs, p);
    CHECK(setupComplete(items) && setupSummary(items) == std::to_string(items.size()) + " of " + std::to_string(items.size()) + " voice codecs installed", "complete");
    CHECK(!setupComplete({}), "nothing known is not complete");

    const auto mac = setupSteps(SetupOs::MacOS), win = setupSteps(SetupOs::Windows), lin = setupSteps(SetupOs::Linux);
    CHECK(mac.size() >= 4 && mac[0].find("brew install cmake") != std::string::npos, "mac steps");
    CHECK(win[0].find("MSYS2") != std::string::npos && win[2].find("build-codecs.ps1") != std::string::npos, "windows steps");
    CHECK(lin[0].find("apt install") != std::string::npos && lin[2].find("build-codecs.sh") != std::string::npos, "linux steps");
    CHECK(setupBuildCommand(SetupOs::MacOS) == "bash tools/codec-builder/build-codecs.sh", "command");
    CHECK(std::string(setupOsName(currentSetupOs())).size() > 2, "os name");
    return testSummary("voice_codec_setup");
}
