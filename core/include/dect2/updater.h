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

// What the Windows helper leaves behind when the setup program has run (or could not): read once by the next start of the program.
struct UpdateResult {
    bool found = false;        // a readable result file existed
    bool ok = false;           // the setup program ended with exit code 0
    std::string version;       // the version it was asked to install
    int setupExit = 0;
    std::string time;
};
UpdateResult parseUpdateResult(const std::string& text);
// Where the helper writes it. A fixed place: the program that reads it is a new process (another PID than the one that started the update).
std::string updateResultPath();
// The text of the script that runs, elevated, once the program has ended (Windows): waits for `pid`, runs the setup program silently, records
// its exit code in `resultFile`, then starts the program again without administrator rights. The paths are put in the script as values
// (not on a command line), so spaces, apostrophes, ampersands and percent signs in them are safe. Callable on every system, for tests.
std::string windowsApplyScript(int pid, const std::string& setupExe, const std::string& installedExe, const std::string& resultFile, const std::string& version, bool restart);
// The arguments for cmd.exe that run the script (what ShellExecute is given with the "runas" verb).
std::string windowsApplyParams(const std::string& scriptPath);

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
        std::string applyError;            // why the last apply() did not start the installation (the update stays Ready)
        UpdateResult lastResult;           // what the helper of an earlier start recorded (see loadResult)
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
    void loadResult();                               // reads and removes the result file of an update that ran before this start
    void dismissResult();
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

// Opens a web address in the user's browser (the shared channel database uses it for its pre-filled issue).
void openUrl(const std::string& url);

} // namespace dect2
