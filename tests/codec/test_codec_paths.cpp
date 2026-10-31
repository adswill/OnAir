// The codec search path list (codec_paths.h): order, de-duplication, Windows paths with both slash styles and spaces, the macOS app bundle,
// and the multi-folder plug-in loader (first folder wins, a second copy is reported as shadowed).
#include "dect2/codec/codec_paths.h"
#include "dect2/codec/speech_codec.h"
#include "../common/test_util.h"
#include <filesystem>
#include <fstream>

using namespace dect2::codec;
namespace fs = std::filesystem;

namespace {

class Silence final : public SpeechCodec {
public:
    int sampleRate() const override { return 8000; }
    std::vector<int16_t> decode(const std::vector<uint8_t>&) override { return {}; }
};
std::unique_ptr<SpeechCodec> fakeOpen(const std::string&, std::string&) { return std::make_unique<Silence>(); }
std::unique_ptr<SpeechCodec> failOpen(const std::string&, std::string& err) { err = "broken"; return nullptr; }

void touch(const fs::path& p) { std::ofstream(p, std::ios::binary) << "x"; }

} // namespace

int main() {
    // 1. Windows, installed in a custom folder (the bug report): both locations, user first.
    {
        CodecPathInput in;
        in.exeDir = "C:/Radio/SDRApps/Onair/";
        in.userDir = "C:\\Users\\Me\\AppData\\Local\\OnAir\\codecs";
        const auto d = codecSearchDirs(in);
        CHECK(d.size() == 2, "two folders, got %zu", d.size());
        CHECK(d.size() == 2 && d[0].path == "C:\\Users\\Me\\AppData\\Local\\OnAir\\codecs" && d[0].source == "user", "user first");
        CHECK(d.size() == 2 && d[1].path == "C:\\Radio\\SDRApps\\Onair\\codecs" && d[1].source == "program", "program second: %s", d.size() > 1 ? d[1].path.c_str() : "-");
    }
    // 2. Spaces, mixed slashes, doubled and trailing separators, dot segments.
    {
        CodecPathInput in;
        in.exeDir = "D:\\Program Files//On Air\\.\\bin\\..\\";
        in.userDir = "C:/Users/Jane Doe/AppData/Local/OnAir/codecs/";
        const auto d = codecSearchDirs(in);
        CHECK(d.size() == 2 && d[0].path == "C:\\Users\\Jane Doe\\AppData\\Local\\OnAir\\codecs", "user path cleaned: %s", d.empty() ? "-" : d[0].path.c_str());
        CHECK(d.size() == 2 && d[1].path == "D:\\Program Files\\On Air\\codecs", "program path cleaned: %s", d.size() > 1 ? d[1].path.c_str() : "-");
    }
    // 3. Order with every input, and de-duplication (Windows paths ignore case and slash style).
    {
        CodecPathInput in;
        in.exeDir = "C:\\Radio\\Onair";
        in.userDir = "C:\\U\\codecs";
        in.envDir = "E:\\env codecs";
        in.extraDir = "X:/extra/";
        auto d = codecSearchDirs(in);
        CHECK(d.size() == 4, "four distinct folders, got %zu", d.size());
        CHECK(d.size() == 4 && d[0].source == "user" && d[1].source == "extra" && d[2].source == "ONAIR_CODEC_DIR" && d[3].source == "program", "order user, extra, env, program");
        in.userDir = "C:/Radio/Onair/CODECS";       // user folder equals the program one: kept once, as "user"
        in.extraDir.clear();
        in.envDir.clear();
        d = codecSearchDirs(in);
        CHECK(d.size() == 1 && d[0].source == "user", "duplicate dropped, first source kept");
    }
    // 4. macOS bundle: beside the app and inside it; Linux: next to the program; empty inputs skipped.
    {
        CodecPathInput in;
        in.exeDir = "/Applications/Radio Apps/OnAir.app/Contents/MacOS";
        in.userDir = "/Users/me/Library/Application Support/OnAir/codecs";
        auto d = codecSearchDirs(in);
        CHECK(d.size() == 4, "mac: user, MacOS/codecs, beside the app, Resources/codecs: %zu", d.size());
        CHECK(d.size() == 4 && d[2].path == "/Applications/Radio Apps/codecs", "mac beside the app: %s", d.size() > 2 ? d[2].path.c_str() : "-");
        CHECK(d.size() == 4 && d[3].path == "/Applications/Radio Apps/OnAir.app/Contents/Resources/codecs", "mac Resources");
        in.exeDir = "/opt/onair/bin/";
        in.userDir = "/home/u/.local/share/onair/codecs";
        in.envDir = "/mnt/codecs";
        d = codecSearchDirs(in);
        CHECK(d.size() == 3 && d[1].path == "/mnt/codecs" && d[2].path == "/opt/onair/bin/codecs", "linux order");
        CHECK(codecSearchDirs(CodecPathInput{}).empty(), "nothing in, nothing out");
    }
    // 5. Helpers.
    CHECK(normalizeDirPath("//srv/share/x/") == "/srv/share/x", "posix double slash start");
    CHECK(normalizeDirPath("\\\\srv\\share\\x\\") == "\\\\srv\\share\\x", "network share: %s", normalizeDirPath("\\\\srv\\share\\x\\").c_str());
    CHECK(normalizeDirPath("C:\\") == "C:\\", "drive root");
    CHECK(joinPath("C:\\a b", "f.dll") == "C:\\a b\\f.dll" && joinPath("/a/b/", "f.so") == "/a/b/f.so", "joinPath");
    CHECK(samePath("C:/A/b", "c:\\a\\B\\") && !samePath("/A/b", "/a/b"), "samePath case rules");

    // 6. The loader over real folders: first with the file wins, the second is shadowed; a broken first falls through; none = list in message.
    {
        const fs::path root = fs::temp_directory_path() / "onair-test-codec-paths";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "user dir", ec);
        fs::create_directories(root / "prog", ec);
        fs::create_directories(root / "empty", ec);
        const std::string file = speechCodecFileName("ysf");
        touch(root / "user dir" / file);
        touch(root / "prog" / file);
        const std::vector<std::string> dirs = {(root / "empty").string(), (root / "user dir").string(), (root / "prog").string()};
        std::unique_ptr<SpeechCodec> c;
        auto r = loadSpeechCodecInDirs("ysf", dirs, true, c, fakeOpen);
        CHECK(r.status == CodecStatus::Ready && r.folder == dirs[1], "found in the first folder that has it: %s", r.folder.c_str());
        CHECK(r.shadowed.size() == 1 && r.shadowed[0] == dirs[2], "second copy reported as shadowed");
        r = loadSpeechCodecInDirs("ysf", dirs, false, c, fakeOpen);
        CHECK(r.status == CodecStatus::NoticeRequired && r.folder == dirs[1], "notice state names the folder");
        r = loadSpeechCodecInDirs("ysf", {dirs[2], dirs[1]}, true, c, failOpen);
        CHECK(r.status == CodecStatus::LoadFailed && r.folder == dirs[2], "every copy broken: first failure reported");
        r = loadSpeechCodecInDirs("nothere", dirs, true, c, fakeOpen);
        CHECK(r.status == CodecStatus::NotInstalled && r.message.find(dirs[0]) != std::string::npos && r.message.find(dirs[2]) != std::string::npos, "not installed lists every folder");
        const auto hit = dirsWithFile(codecSearchDirs({"", (root / "user dir").string(), "", (root / "prog").string()}), file,
                                      [](const std::string& p) { std::error_code e; return fs::is_regular_file(p, e); });
        CHECK(hit.size() == 2, "dirsWithFile finds both: %zu", hit.size());
        fs::remove_all(root, ec);
    }
    {   // #279 #376: a single-library codec (LAME, FAAD2) is found in the codecs folders, user folder first, the program folder last
        CodecPathInput in;
        in.exeDir = "C:/Radio/SDRApps/Onair/"; in.userDir = "C:/Users/me/AppData/Local/OnAir/codecs";
        const auto dirs = codecSearchDirs(in);
        const std::vector<std::string> names = {"libmp3lame.dll", "lame_enc.dll"};
        std::vector<std::string> have = {"C:/Radio/SDRApps/Onair/libmp3lame.dll", "C:/Users/me/AppData/Local/OnAir/codecs/lame_enc.dll", "C:/Radio/SDRApps/Onair/codecs/libmp3lame.dll"};
        const auto ex = [&](const std::string& p) { for (const auto& h : have) if (h == p) return true; return false; };
        const auto r = libraryPaths(dirs, {"C:/Radio/SDRApps/Onair"}, names, ex);
        CHECK(r.size() == 3 && r[0] == "C:/Users/me/AppData/Local/OnAir/codecs/lame_enc.dll", "user folder first: %zu", r.size());
        CHECK(r[1] == "C:/Radio/SDRApps/Onair/codecs/libmp3lame.dll" && r[2] == "C:/Radio/SDRApps/Onair/libmp3lame.dll", "then the codecs folder next to the program, then the program folder");
        CHECK(libraryPaths(dirs, {}, {"nothing.dll"}, ex).empty(), "nothing found");
        CHECK(libraryPaths({}, {"C:/x", "C:/x/"}, {"a"}, [](const std::string&) { return true; }).size() == 1, "a folder listed twice counts once");
    }
    return testSummary("codec_paths");
}
