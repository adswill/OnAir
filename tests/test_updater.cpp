// The updater: versions, SHA-256, choosing a release and its package from GitHub's answer, and the whole path (check, download, check the
// checksum, prepare, swap) against a fake release made of local files, with a fake portable Linux installation.
#include "dect2/updater.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace dect2;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static void put(const std::string& p, const std::string& s) { fs::create_directories(fs::path(p).parent_path()); std::ofstream(p, std::ios::binary) << s; }

int main() {
    CHECK(compareVersions("0.1.3", "0.1.2") > 0 && compareVersions("v0.2.0", "0.1.9") > 0 && compareVersions("0.1.3", "v0.1.3") == 0 && compareVersions("0.1", "0.1.0") == 0 && compareVersions("1.0.0-rc1", "0.9.9") > 0 && compareVersions("0.10.0", "0.9.0") > 0, "version comparison");
    CHECK(sha256Bytes("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
    CHECK(sha256Bytes("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    const std::string million(1000000, 'a');
    CHECK(sha256Bytes(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 of a million a");

    const std::string json = R"([
      {"tag_name":"v0.1.4","name":"OnAir 0.1.4","draft":false,"prerelease":true,"html_url":"https://example.org/r/0.1.4","body":"New: a é test\nline two",
       "assets":[
         {"name":"OnAir-0.1.4-macos-arm64.dmg","size":10,"browser_download_url":"https://example.org/a.dmg","digest":"sha256:aa"},
         {"name":"OnAir-0.1.4-macos-x86_64.dmg","size":11,"browser_download_url":"https://example.org/b.dmg","digest":"sha256:bb"},
         {"name":"OnAir-0.1.4-windows-x64-setup.exe","size":12,"browser_download_url":"https://example.org/c.exe","digest":"sha256:cc"},
         {"name":"onair-0.1.4-linux-x86_64-portable.tar.gz","size":13,"browser_download_url":"https://example.org/d.tgz","digest":"sha256:dd"},
         {"name":"onair-0.1.4-linux-aarch64-portable.tar.gz","size":14,"browser_download_url":"https://example.org/e.tgz","digest":"sha256:ee"},
         {"name":"onair_0.1.4_amd64.deb","size":15,"browser_download_url":"https://example.org/f.deb"}]},
      {"tag_name":"v0.1.3","name":"OnAir 0.1.3","draft":false,"prerelease":false,"assets":[]},
      {"tag_name":"v0.9.0","name":"draft","draft":true,"prerelease":false,"assets":[]}])";
    {
        auto r = pickRelease(json, "0.1.3", true, InstallKind::MacApp, "arm64");
        CHECK(r.valid && r.version == "0.1.4" && r.assetName == "OnAir-0.1.4-macos-arm64.dmg" && r.sha256 == "aa" && r.size == 10 && r.prerelease, "mac arm64 package");
        CHECK(r.notes == "New: a \xc3\xa9 test\nline two" && r.pageUrl == "https://example.org/r/0.1.4", "notes and page decoded");
        r = pickRelease(json, "0.1.3", true, InstallKind::MacApp, "x86_64");
        CHECK(r.valid && r.assetName == "OnAir-0.1.4-macos-x86_64.dmg", "mac Intel package");
        r = pickRelease(json, "0.1.3", true, InstallKind::WindowsInstall, "x86_64");
        CHECK(r.valid && r.assetName == "OnAir-0.1.4-windows-x64-setup.exe", "Windows installer");
        r = pickRelease(json, "0.1.3", true, InstallKind::LinuxPortable, "aarch64");
        CHECK(r.valid && r.assetName == "onair-0.1.4-linux-aarch64-portable.tar.gz", "Linux portable for arm");
        r = pickRelease(json, "0.1.3", true, InstallKind::LinuxDeb, "x86_64");
        CHECK(r.valid && r.assetName == "onair_0.1.4_amd64.deb" && r.sha256.empty(), "deb without a digest");
        r = pickRelease(json, "0.1.3", false, InstallKind::MacApp, "arm64");
        CHECK(!r.valid, "a pre-release is skipped when only releases are wanted");
        r = pickRelease(json, "0.1.4", true, InstallKind::MacApp, "arm64");
        CHECK(!r.valid, "no update when up to date");
        std::string err;
        r = pickRelease("not json", "0.1.3", true, InstallKind::MacApp, "arm64", &err);
        CHECK(!r.valid && !err.empty(), "bad answer reported");
    }

    // ---- the whole path against files
    const fs::path base = fs::temp_directory_path() / ("onair-updater-test-" + std::to_string(getpid()));
    fs::remove_all(base);
    const std::string inst = (base / "install" / "onair-0.0.1-linux-x86_64-portable").string();
    put(inst + "/bin/onair", "OLD BINARY\n");
    put(inst + "/README.txt", "portable\n");
    put(inst + "/lib/libold.so", "old lib\n");
    // the package of the new version
    const std::string pk = (base / "pkg" / "onair-9.9.9-linux-x86_64-portable").string();
    put(pk + "/bin/onair", "#!/bin/sh\necho new\n");
    put(pk + "/README.txt", "portable\n");
    put(pk + "/lib/libnew.so", "new lib\n");
    const std::string tgz = (base / "onair-9.9.9-linux-x86_64-portable.tar.gz").string();
    CHECK(system(("cd " + (base / "pkg").string() + " && tar czf " + tgz + " onair-9.9.9-linux-x86_64-portable").c_str()) == 0, "made the package");
    std::string sum = sha256File(tgz);
    auto writeApi = [&](const std::string& digest) {
        std::string d = "{\"name\":\"onair-9.9.9-linux-x86_64-portable.tar.gz\",\"size\":" + std::to_string(fs::file_size(tgz)) + ",\"browser_download_url\":\"file://" + tgz + "\",\"digest\":\"sha256:" + digest + "\"}";
        std::string d2 = d;
        const size_t k = d2.find("x86_64"); d2.replace(k, 6, "aarch64");
        const size_t k2 = d2.find("x86_64"); (void)k2;
        put((base / "api.json").string(), "[{\"tag_name\":\"v9.9.9\",\"name\":\"OnAir 9.9.9\",\"draft\":false,\"prerelease\":false,\"html_url\":\"https://example.org\",\"body\":\"notes\",\"assets\":[" + d + "," + d2 + "]}]");
    };
    auto waitFor = [&](Updater& u, Updater::Phase want, int secs) {
        for (int i = 0; i < secs * 20; i++) { const auto p = u.status().phase; if (p == want || p == Updater::Phase::Failed) return p; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        return u.status().phase;
    };

    // a wrong checksum is refused
    {
        writeApi(std::string(64, '0'));
        Updater u("0.0.1");
        u.setApiUrl("file://" + (base / "api.json").string());
        u.setExecutable(inst + "/bin/onair", (int)InstallKind::LinuxPortable);
        u.check(true);
        CHECK(waitFor(u, Updater::Phase::Available, 10) == Updater::Phase::Available, "found the update");
        u.download();
        const auto p = waitFor(u, Updater::Phase::Ready, 20);
        CHECK(p == Updater::Phase::Failed && u.status().error.find("checksum") != std::string::npos, "a wrong checksum is refused");
        CHECK(slurp(inst + "/bin/onair") == "OLD BINARY\n", "the installation is untouched");
    }
    // the right one goes through, and the helper swaps the installation once the program it waits for has ended
    {
        writeApi(sum);
        Updater u("0.0.1");
        u.setApiUrl("file://" + (base / "api.json").string());
        u.setExecutable(inst + "/bin/onair", (int)InstallKind::LinuxPortable);
        u.check(true);
        CHECK(waitFor(u, Updater::Phase::Available, 10) == Updater::Phase::Available, "found the update (2)");
        CHECK(u.status().release.version == "9.9.9" && u.status().autoInstallable, "release and install kind");
        u.download();
        CHECK(waitFor(u, Updater::Phase::Ready, 30) == Updater::Phase::Ready, "downloaded, verified and prepared: %s", u.status().error.c_str());
        CHECK(slurp(inst + "/bin/onair") == "OLD BINARY\n", "still the old program before the swap");
        // the program the helper waits for: a short-lived child
        const pid_t child = fork();
        if (child == 0) { sleep(2); _exit(0); }
        u.setWaitPid((int)child);
        std::string err;
        CHECK(u.apply(false, &err), "apply: %s", err.c_str());
        sleep(1);
        CHECK(slurp(inst + "/bin/onair") == "OLD BINARY\n", "the helper waits for the program to end");
        int stt = 0;
        waitpid(child, &stt, 0);
        for (int i = 0; i < 100 && slurp(inst + "/bin/onair") != "#!/bin/sh\necho new\n"; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(slurp(inst + "/bin/onair") == "#!/bin/sh\necho new\n" && fs::exists(inst + "/lib/libnew.so") && !fs::exists(inst + "/lib/libold.so"), "the installation was replaced");
    }
#ifdef __APPLE__
    // the macOS app: the new version comes in a disk image and is copied next to the installed app, then swapped for it
    {
        const std::string app = (base / "Applications" / "OnAir.app").string();
        put(app + "/Contents/MacOS/OnAir", "#!/bin/sh\necho old\n");
        put(app + "/Contents/Info.plist", "<plist>old</plist>");
        const std::string newApp = (base / "dmgroot" / "OnAir.app").string();
        put(newApp + "/Contents/MacOS/OnAir", "#!/bin/sh\necho new\n");
        put(newApp + "/Contents/Info.plist", "<plist>new</plist>");
        const std::string dmg = (base / "OnAir-9.9.9-macos-arm64.dmg").string();
        const std::string dmg2 = (base / "OnAir-9.9.9-macos-x86_64.dmg").string();
        CHECK(system(("hdiutil create -quiet -volname OnAir -srcfolder " + (base / "dmgroot").string() + " -ov " + dmg).c_str()) == 0, "made the disk image");
        fs::copy_file(dmg, dmg2, fs::copy_options::overwrite_existing);
        const std::string sum2 = sha256File(dmg);
        auto asset = [&](const std::string& f) { return "{\"name\":\"" + fs::path(f).filename().string() + "\",\"size\":" + std::to_string(fs::file_size(f)) + ",\"browser_download_url\":\"file://" + f + "\",\"digest\":\"sha256:" + sum2 + "\"}"; };
        put((base / "api2.json").string(), "[{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,\"assets\":[" + asset(dmg) + "," + asset(dmg2) + "]}]");
        Updater u("0.0.1");
        u.setApiUrl("file://" + (base / "api2.json").string());
        u.setExecutable(app + "/Contents/MacOS/OnAir");
        CHECK(u.status().kind == InstallKind::MacApp, "recognised as a macOS app");
        u.check(true);
        CHECK(waitFor(u, Updater::Phase::Available, 10) == Updater::Phase::Available, "mac: found the update");
        u.download();
        CHECK(waitFor(u, Updater::Phase::Ready, 60) == Updater::Phase::Ready, "mac: downloaded and prepared: %s", u.status().error.c_str());
        const pid_t child = fork();
        if (child == 0) { sleep(1); _exit(0); }
        u.setWaitPid((int)child);
        std::string err;
        CHECK(u.apply(false, &err), "mac: apply %s", err.c_str());
        int stt = 0;
        waitpid(child, &stt, 0);
        for (int i = 0; i < 100 && slurp(app + "/Contents/Info.plist") != "<plist>new</plist>"; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(slurp(app + "/Contents/Info.plist") == "<plist>new</plist>" && slurp(app + "/Contents/MacOS/OnAir") == "#!/bin/sh\necho new\n", "mac: the app was replaced");
        CHECK(!fs::exists((base / "Applications" / ".OnAir-update").string()), "mac: the staging folder is gone");
    }
#endif
    fs::remove_all(base);
    printf(fails ? "updater: FAILED\n" : "updater: ok\n");
    return fails ? 1 : 0;
}
