// The updater: versions, SHA-256, choosing a release and its package from GitHub's answer, and the whole path (check, download, check the
// checksum, prepare, swap) against a fake release made of local files, with a fake portable Linux installation (not on Windows), and the
// script and result file of the Windows installation step (on every system).
#include "dect2/engine/updater.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace dect2;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static void put(const std::string& p, const std::string& s) { fs::create_directories(fs::path(p).parent_path()); std::ofstream(p, std::ios::binary) << s; }

int main() {
    CHECK(compareVersions("0.1.3", "0.1.2") > 0 && compareVersions("v0.2.0", "0.1.9") > 0 && compareVersions("0.1.3", "v0.1.3") == 0 && compareVersions("0.1", "0.1.0") == 0 && compareVersions("1.0.0-rc1", "0.9.9") > 0 && compareVersions("0.10.0", "0.9.0") > 0, "version comparison");
    // 3.0 follows 0.2.9 (numeric, not text: "3.0" > "0.2.9" and "0.10" > "0.9"), then 3.1, 3.2 ... 3.10
    CHECK(compareVersions("3.0", "0.2.9") > 0 && compareVersions("v3.0.0", "0.2.9") > 0 && compareVersions("3.0", "3.0.0") == 0 && compareVersions("3.1", "3.0") > 0 &&
          compareVersions("3.10", "3.9") > 0 && compareVersions("3.0.1", "3.0") > 0 && compareVersions("0.2.9", "3.0") < 0 && compareVersions("3.1-beta.1", "3.0") > 0 &&
          compareVersions("3.1", "3.1-beta.2") > 0, "version 3.0 and later compare as numbers");
    CHECK(compareVersions("0.2.6-beta.2", "0.2.6-beta.1") > 0 && compareVersions("0.2.6", "0.2.6-beta.9") > 0 && compareVersions("0.2.6-beta.1", "0.2.5") > 0 && compareVersions("0.2.6-beta", "0.2.6-beta.0") == 0, "a beta is newer than the last release and older than the finished version");
    CHECK(startupCheckAction(true, 3, 0, false, 0) == 1 && startupCheckAction(true, 1, 0, false, 0) == 0 && startupCheckAction(false, 99, 0, false, 0) == 0, "start-up check runs every start unless switched off");
    CHECK(startupCheckAction(true, 99, 1, true, 30) == 2 && startupCheckAction(true, 99, 1, true, 5) == 0 && startupCheckAction(true, 99, 1, false, 99) == 0 && startupCheckAction(true, 99, 2, true, 99) == 0, "one retry after a failed start-up check");
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

    // ---- the Windows helper script: paths with spaces, apostrophes, ampersands and percent signs go in as values, never in a command line
    {
        const std::string setup = "C:\\Users\\O'Brien & Sons\\AppData\\Local\\Temp\\onair-update-42\\OnAir-0.2.0-windows-x64-setup.exe";
        const std::string exe = "C:\\Program Files\\On'Air 100%\\OnAir.exe";
        const std::string res = "C:\\Users\\O'Brien & Sons\\AppData\\Local\\Temp\\onair-update-result.txt";
        const std::string sc = windowsApplyScript(4242, setup, exe, res, "0.2.0", true);
        CHECK(has(sc, "set \"SETUP=" + setup + "\"\r\n"), "script: the setup path is a quoted value");
        CHECK(has(sc, "set \"EXE=C:\\Program Files\\On'Air 100%%\\OnAir.exe\"\r\n"), "script: a percent sign is doubled");
        CHECK(has(sc, "set \"RESULT=" + res + "\"\r\n") && has(sc, "set \"PID=4242\"") && has(sc, "set \"VER=0.2.0\""), "script: result file, pid and version");
        CHECK(!has(sc, "powershell") && !has(sc, "Start-Process") && !has(sc, "-Verb"), "script: no PowerShell string holds a path");
        CHECK(has(sc, "start \"\" /wait \"%SETUP%\" /S") && has(sc, "set \"CODE=%ERRORLEVEL%\""), "script: runs the setup silently, waits and keeps its exit code");
        CHECK(has(sc, "echo version=%VER%") && has(sc, "echo setup_exit=%CODE%") && has(sc, "echo time="), "script: writes the result file");
        CHECK(has(sc, "start \"\" explorer.exe \"%EXE%\""), "script: the program is started again through explorer, without administrator rights");
        CHECK(sc.find("start \"\" explorer.exe") > sc.find("setup_exit=") && sc.find("/wait") < sc.find("setup_exit="), "script: order is setup, result, restart");
        CHECK(has(sc, "tasklist /FI \"PID eq %PID%\"") && has(sc, "set \"RESTART=1\""), "script: waits for OnAir to end");
        CHECK(has(windowsApplyScript(1, setup, exe, res, "0.2.0", false), "set \"RESTART=0\""), "script: no restart on request");
        CHECK(has(windowsApplyScript(1, "C:/a b/it's/setup.exe", exe, res, "0.2.0", true), "set \"SETUP=C:\\a b\\it's\\setup.exe\""), "script: slashes become backslashes");
        CHECK(!has(windowsApplyScript(1, "C:\\a\"b\r\n.exe", exe, res, "0.2.0", true), "a\"b"), "script: a quote in a path cannot end the value");
        CHECK(windowsApplyParams("C:\\Temp\\a b's & c\\apply.cmd") == "/C \"\"C:\\Temp\\a b's & c\\apply.cmd\"\"", "cmd arguments: the whole command in an outer pair of quotes");
    }
    // ---- the result file
    {
        UpdateResult r = parseUpdateResult("version=0.2.0\r\nsetup_exit=0\r\ntime=Wed 10/07/2026 12:00:00.00\r\n");
        CHECK(r.found && r.ok && r.version == "0.2.0" && r.setupExit == 0 && r.time == "Wed 10/07/2026 12:00:00.00", "result: success");
        r = parseUpdateResult("version=0.2.0\r\nsetup_exit=2\r\n");
        CHECK(r.found && !r.ok && r.setupExit == 2, "result: failure");
        r = parseUpdateResult("\xEF\xBB\xBFversion=0.2.0\nsetup_exit=-2 \n");
        CHECK(r.found && !r.ok && r.setupExit == -2 && r.version == "0.2.0", "result: byte order mark, negative code, trailing blank");
        CHECK(!parseUpdateResult("").found && !parseUpdateResult("version=0.2.0\n").found && !parseUpdateResult("setup_exit=0\n").found && !parseUpdateResult("garbage").found, "result: incomplete files are ignored");
    }

#ifndef _WIN32
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
    auto writeApi = [&](const std::string& digest, uint64_t sizeOverride = 0) {
        std::string d = "{\"name\":\"onair-9.9.9-linux-x86_64-portable.tar.gz\",\"size\":" + std::to_string(sizeOverride ? sizeOverride : fs::file_size(tgz)) + ",\"browser_download_url\":\"file://" + tgz + "\",\"digest\":\"sha256:" + digest + "\"}";
        std::string d2 = d;
        const size_t k = d2.find("x86_64"); d2.replace(k, 6, "aarch64");
        const size_t k2 = d2.find("x86_64"); (void)k2;
        put((base / "api.json").string(), "[{\"tag_name\":\"v9.9.9\",\"name\":\"OnAir 9.9.9\",\"draft\":false,\"prerelease\":false,\"html_url\":\"https://example.org\",\"body\":\"notes\",\"assets\":[" + d + "," + d2 + "]}]");
    };
    auto waitFor = [&](Updater& u, Updater::Phase want, int secs) {
        secs = secs * 6 + 20;   /* CI is heavily loaded: poll with a generous deadline, returns as soon as the phase arrives */
        for (int i = 0; i < secs * 20; i++) { const auto p = u.status().phase; if (p == want || p == Updater::Phase::Failed) return p; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        return u.status().phase;
    };

    // a size that is not the listed one is refused
    {
        writeApi(sum, fs::file_size(tgz) + 1);
        Updater u("0.0.1");
        u.setApiUrl("file://" + (base / "api.json").string());
        u.setExecutable(inst + "/bin/onair", (int)InstallKind::LinuxPortable);
        u.check(true);
        CHECK(waitFor(u, Updater::Phase::Available, 10) == Updater::Phase::Available, "found the update (size)");
        u.download();
        const auto p = waitFor(u, Updater::Phase::Ready, 20);
        CHECK(p == Updater::Phase::Failed && u.status().error.find("size") != std::string::npos, "a wrong size is refused: %s", u.status().error.c_str());
    }
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
    // a .deb: downloaded and checked, then the user is told the exact command; nothing is run (no pkexec, no dpkg) and the program is not replaced
    {
        const std::string deb = (base / "onair_9.9.9_amd64.deb").string();
        put(deb, "!<arch>\nnot a real package, only bytes\n");
        const std::string dsum = sha256File(deb);
        auto asset = [&](const std::string& n, const std::string& digest) { return "{\"name\":\"" + n + "\",\"size\":" + std::to_string(fs::file_size(deb)) + ",\"browser_download_url\":\"file://" + deb + "\",\"digest\":\"sha256:" + digest + "\"}"; };
        put((base / "apideb.json").string(), "[{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,\"assets\":[" + asset("onair_9.9.9_amd64.deb", dsum) + "," + asset("onair_9.9.9_arm64.deb", dsum) + "]}]");
        Updater u("0.0.1");
        u.setApiUrl("file://" + (base / "apideb.json").string());
        u.setExecutable("/usr/bin/onair", (int)InstallKind::LinuxDeb);
        CHECK(u.status().manualInstall && !u.status().autoInstallable, "deb: manual install, not self-replacing");
        u.check(true);
        CHECK(waitFor(u, Updater::Phase::Available, 10) == Updater::Phase::Available, "deb: found the update");
        const std::string wantDeb = thisArch() == "arm64" ? "onair_9.9.9_arm64.deb" : "onair_9.9.9_amd64.deb";
        CHECK(u.status().release.assetName == wantDeb, "deb: the package for this arch: %s", u.status().release.assetName.c_str());
        u.download();
        CHECK(waitFor(u, Updater::Phase::Ready, 20) == Updater::Phase::Ready, "deb: downloaded and checked: %s", u.status().error.c_str());
        const std::string cmd = u.status().manualCommand;
        CHECK(cmd.compare(0, 19, "sudo apt install '/") == 0 && has(cmd, wantDeb + "'"), "deb: the command: %s", cmd.c_str());
        std::string err;
        CHECK(!u.apply(true, &err) && has(err, "sudo apt install"), "deb: apply runs nothing and names the command: %s", err.c_str());
        CHECK(debInstallCommand("/tmp/it's a.deb") == "sudo apt install '/tmp/it'\\''s a.deb'", "deb: quoting: %s", debInstallCommand("/tmp/it's a.deb").c_str());
        // a wrong checksum is refused for a .deb too
        put((base / "apideb.json").string(), "[{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,\"assets\":[" + asset("onair_9.9.9_amd64.deb", std::string(64, 'a')) + "," + asset("onair_9.9.9_arm64.deb", std::string(64, 'a')) + "]}]");
        Updater v("0.0.1");
        v.setApiUrl("file://" + (base / "apideb.json").string());
        v.setExecutable("/usr/bin/onair", (int)InstallKind::LinuxDeb);
        v.check(true);
        CHECK(waitFor(v, Updater::Phase::Available, 10) == Updater::Phase::Available, "deb: found the update (bad sum)");
        v.download();
        CHECK(waitFor(v, Updater::Phase::Ready, 20) == Updater::Phase::Failed && has(v.status().error, "checksum") && v.status().manualCommand.empty(), "deb: a wrong checksum is refused");
    }
    // an AppImage: found by its header, checked against SHA256SUMS (also when GitHub lists no digest), replaced in place without root
    {
        const std::string ar = thisArch() == "arm64" ? "aarch64" : "x86_64";
        auto fakeAi = [](const std::string& tail) { std::string h("\x7f" "ELF", 4); h += std::string(4, '\0'); h += "AI"; h += '\x02'; return h + tail; };
        const std::string old = (base / "apps" / ("OnAir-0.0.1-linux-" + ar + ".AppImage")).string();
        put(old, fakeAi("OLD"));
        chmod(old.c_str(), 0755);
        CHECK(looksLikeAppImage(old) && !looksLikeAppImage(tgz), "appimage: recognised by its header");
#ifdef __linux__
        CHECK(detectInstallKind(old) == InstallKind::LinuxAppImage, "appimage: install kind");
#endif
        const std::string nu = (base / "rel" / ("OnAir-9.9.9-linux-" + ar + ".AppImage")).string();
        put(nu, fakeAi("NEW VERSION"));
        const std::string nsum = sha256File(nu);
        const std::string an = "OnAir-9.9.9-linux-" + ar + ".AppImage";
        auto api = [&](const std::string& digest) {
            put((base / "apiai.json").string(), "[{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"" + an + "\",\"size\":" + std::to_string(fs::file_size(nu)) + ",\"browser_download_url\":\"file://" + nu + "\"" + (digest.empty() ? "" : ",\"digest\":\"sha256:" + digest + "\"") + "}]}]");
        };
        auto sums = [&](const std::string& h) { put((base / "dl" / "v9.9.9" / "SHA256SUMS").string(), h + "  " + an + "\n" + std::string(64, 'e') + "  other.deb\n"); };
        auto mk = [&](Updater& u) { u.setApiUrl("file://" + (base / "apiai.json").string()); u.setReleaseBase("file://" + (base / "dl").string() + "/"); u.setExecutable(old, (int)InstallKind::LinuxAppImage); };
        {   // a wrong SHA256SUMS line is refused even though GitHub's digest is right
            api(nsum); sums(std::string(64, 'a'));
            Updater u("0.0.1"); mk(u);
            CHECK(u.status().autoInstallable && !u.status().manualInstall, "appimage: self-replacing");
            u.check(true);
            CHECK(waitFor(u, Updater::Phase::Available, 10) == Updater::Phase::Failed && has(u.status().error, "SHA256SUMS"), "appimage: digest and SHA256SUMS must agree: %s", u.status().error.c_str());
        }
        {   // a wrong checksum in SHA256SUMS alone (no digest) is caught when the download is checked
            api(""); sums(std::string(64, 'b'));
            Updater u("0.0.1"); mk(u);
            u.check(true);
            CHECK(waitFor(u, Updater::Phase::Available, 10) == Updater::Phase::Available, "appimage: found (no digest)");
            u.download();
            CHECK(waitFor(u, Updater::Phase::Ready, 20) == Updater::Phase::Failed && has(u.status().error, "checksum") && slurp(old) == fakeAi("OLD"), "appimage: wrong sum refused, file untouched");
        }
        {   // something that is no AppImage is refused even with a matching checksum
            const std::string bad = (base / "rel2" / an).string();
            put(bad, "just text, not an AppImage");
            put((base / "apiai.json").string(), "[{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"" + an + "\",\"size\":" + std::to_string(fs::file_size(bad)) + ",\"browser_download_url\":\"file://" + bad + "\",\"digest\":\"sha256:" + sha256File(bad) + "\"}]}]");
            sums(sha256File(bad));
            Updater u("0.0.1"); mk(u);
            u.check(true);
            CHECK(waitFor(u, Updater::Phase::Available, 10) == Updater::Phase::Available, "appimage: found (bad file)");
            u.download();
            CHECK(waitFor(u, Updater::Phase::Ready, 20) == Updater::Phase::Failed && has(u.status().error, "not an AppImage") && slurp(old) == fakeAi("OLD"), "appimage: not an AppImage refused");
        }
        {   // the right one: checked against SHA256SUMS, staged next to the old file, swapped once the program has ended
            api(""); sums(nsum);
            Updater u("0.0.1"); mk(u);
            u.check(true);
            CHECK(waitFor(u, Updater::Phase::Available, 10) == Updater::Phase::Available, "appimage: found");
            CHECK(u.status().release.assetName == an && u.status().release.sha256 == nsum, "appimage: asset and checksum from SHA256SUMS");
            u.download();
            CHECK(waitFor(u, Updater::Phase::Ready, 30) == Updater::Phase::Ready, "appimage: ready: %s", u.status().error.c_str());
            CHECK(slurp(old) == fakeAi("OLD"), "appimage: old file kept until the swap");
            const pid_t child = fork();
            if (child == 0) { sleep(2); _exit(0); }
            u.setWaitPid((int)child);
            std::string err;
            CHECK(u.apply(false, &err), "appimage: apply: %s", err.c_str());
            int stt = 0;
            waitpid(child, &stt, 0);
            for (int i = 0; i < 100 && slurp(old) != fakeAi("NEW VERSION"); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CHECK(slurp(old) == fakeAi("NEW VERSION") && !fs::exists(old + ".new") && (fs::status(old).permissions() & fs::perms::owner_exec) != fs::perms::none, "appimage: replaced in place and still executable");
            const auto r = pickRelease("[{\"tag_name\":\"v9.9.9\",\"assets\":[{\"name\":\"OnAir-9.9.9-linux-aarch64.AppImage\",\"size\":1},{\"name\":\"OnAir-9.9.9-linux-x86_64.AppImage\",\"size\":2}]}]", "1.0", false, InstallKind::LinuxAppImage, "aarch64");
            CHECK(r.valid && r.assetName == "OnAir-9.9.9-linux-aarch64.AppImage", "appimage: the package for the architecture");
        }
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
        // the helper removes the staging folder just after the swap: give it the time on a busy machine
        for (int i = 0; i < 100 && fs::exists((base / "Applications" / ".OnAir-update").string()); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(!fs::exists((base / "Applications" / ".OnAir-update").string()), "mac: the staging folder is gone");
    }
#endif
    fs::remove_all(base);
#endif
    printf(fails ? "updater: FAILED\n" : "updater: ok\n");
    return fails ? 1 : 0;
}
