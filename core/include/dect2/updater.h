// Updates: asks GitHub for the newest release, downloads the package that fits this computer, checks its SHA-256 against the digest GitHub lists
// for the file, prepares it and, when the program closes (or on request), swaps the installed program for the new one and starts it again.
// The network access goes through the `curl` program every supported system has (Windows 10 and later, macOS, Linux), so no TLS library is linked.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

struct ReleaseInfo {
    bool valid = false;
    std::string tag, version, name, notes, pageUrl;
    bool prerelease = false;
    std::string assetName, assetUrl, sha256;   // the package for this computer
    uint64_t size = 0;
};

// How this copy of the program was installed: decides what an update can do.
enum class InstallKind {
    Unknown,         // a development build or a place we cannot replace
    MacApp,          // OnAir.app
    WindowsInstall,  // installed by the setup program (or unpacked from the zip)
    LinuxPortable,   // the .tar.gz: a directory with bin/onair in it
    LinuxDeb         // installed by the .deb: needs the administrator to change
};

int compareVersions(const std::string& a, const std::string& b);   // numeric, dot separated; > 0 if a is newer
std::string sha256File(const std::string& path, std::string* err = nullptr);
std::string sha256Bytes(const void* data, size_t n);
std::string executablePath();
InstallKind detectInstallKind(const std::string& exePath);
const char* installKindName(InstallKind k);
// The newest release in a GitHub "list releases" answer that is newer than `current`, with the package for (kind, arch); valid = false if none.
// `arch` is "arm64", "x86_64" (also "aarch64" / "amd64" are understood).
ReleaseInfo pickRelease(const std::string& json, const std::string& current, bool includePrerelease, InstallKind kind, const std::string& arch, std::string* err = nullptr);
std::string thisArch();

class Updater {
public:
    enum class Phase { Idle, Checking, UpToDate, Available, Downloading, Preparing, Ready, Failed };
    struct Status {
        Phase phase = Phase::Idle;
        ReleaseInfo release;
        uint64_t done = 0, total = 0;
        std::string error;
        InstallKind kind = InstallKind::Unknown;
        bool autoInstallable = false;      // the program can replace itself (not for a development build or a .deb)
        double checkedAt = 0;              // seconds since 1970 of the last successful check
    };
    explicit Updater(const std::string& currentVersion);
    ~Updater();
    const std::string& currentVersion() const { return current_; }
    // Where to ask: the GitHub releases of the project, or what ONAIR_UPDATE_URL says (also a file:// address, for tests)
    void setApiUrl(const std::string& url) { api_ = url; }
    void setExecutable(const std::string& path, int kindOverride = -1);   // for tests; defaults to this program
    void setWaitPid(int pid) { waitPid_ = pid; }     // for tests: the process the helper waits for (default: this one)
    void check(bool includePrerelease);              // in the background
    void download();                                 // in the background: fetch, verify, prepare (needs phase Available)
    Status status() const;
    // Starts the helper that replaces the installed program once this process has ended (needs phase Ready). The caller then exits.
    // With restart the new version is started afterwards. For a .deb the package is handed to the system's installer (asks for a password).
    bool apply(bool restart, std::string* err = nullptr);
    void openReleasePage() const;
    void cancel();

private:
    void setPhase(Phase p, const std::string& err = std::string());
    void runCheck(bool pre);
    void runDownload();
    bool prepare(const std::string& file, std::string& err);
    std::string workDir() const;
    std::string current_, api_, exe_;
    InstallKind kind_ = InstallKind::Unknown;
    mutable std::mutex mu_;
    Status st_;
    std::thread th_;
    std::atomic<bool> cancel_{false};
    std::string file_, staged_;
    int waitPid_ = 0;
};

} // namespace dect2
