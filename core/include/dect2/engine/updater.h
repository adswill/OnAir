// Updates: asks GitHub for the newest release, downloads the package that fits this computer, checks its SHA-256 against the digest GitHub lists
// for the file, prepares it and, when the program closes (or on request), swaps the installed program for the new one and starts it again.
// The network access goes through the `curl` program every supported system has (Windows 10 and later, macOS, Linux), so no TLS library is linked.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
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
    bool fromMirror = false;         // the list of releases came through the user's mirror (not from github.com)
    bool officialVerified = false;   // the checksum was confirmed by github.com itself (its API or its SHA256SUMS file)
};

// Where to ask. Automatic = GitHub, and only when `fallback` is on (off by default) the mirror after GitHub failed; GitHubOnly = never the
// mirror; MirrorOnly = only the mirror. The mirror is a web address the user typed: nothing is sent to it unless they chose it.
enum class UpdateSource { Automatic = 0, GitHubOnly = 1, MirrorOnly = 2 };
struct MirrorConfig {
    UpdateSource source = UpdateSource::Automatic;
    bool fallback = false;      // Automatic only: also try the mirror when GitHub fails
    std::string tmpl;           // "https://mirror.example/{url}" or a base address "https://mirror.example/gh" (the address is appended)
};
// True when the mirror may be contacted at all (it has an address and the choices allow it).
bool mirrorEnabled(const MirrorConfig& c);
// The address to ask the mirror for `url`: {url} in the template is replaced by it (also {urlenc}: percent-encoded); without a placeholder the
// address is appended after a "/". "" when the template is not an https address (http only if allowHttp, for tests), has spaces or control
// characters, is longer than 400 characters, or when `url` is not one of the project's own GitHub addresses (the mirror is never asked for
// anything else).
std::string mirrorUrl(const std::string& tmpl, const std::string& url, bool allowHttp = false);
bool isOfficialUrl(const std::string& url);   // api.github.com/repos/adswill/OnAir/... or github.com/adswill/OnAir/releases/...
// The checksum of `name` in the text of a SHA256SUMS file ("<64 hex>  name" per line, also "*name"); "" if absent.
std::string checksumFor(const std::string& sums, const std::string& name);

// How this copy of the program was installed: decides what an update can do.
enum class InstallKind {
    Unknown,         // a development build or a place we cannot replace
    MacApp,          // OnAir.app
    WindowsInstall,  // installed by the setup program (or unpacked from the zip)
    LinuxPortable,   // the .tar.gz: a directory with bin/onair in it
    LinuxDeb         // installed by the .deb: needs the administrator to change
};

int compareVersions(const std::string& a, const std::string& b);   // numeric, dot separated; > 0 if a is newer
// The start-up check: what to do now. 0 = nothing, 1 = the first check, 2 = the one retry after a failed first check.
// enabled: the "check for updates" setting; sinceStart: seconds since the program started; attempts: checks made so far;
// firstFailed: the last start-up check failed; sinceFail: seconds since it failed. No once-a-day throttle: every start checks.
inline int startupCheckAction(bool enabled, double sinceStart, int attempts, bool firstFailed, double sinceFail) {
    if (!enabled) return 0;
    if (attempts == 0) return sinceStart >= 3 ? 1 : 0;
    if (attempts == 1 && firstFailed && sinceFail >= 30) return 2;
    return 0;
}
std::string sha256File(const std::string& path, std::string* err = nullptr);
std::string sha256Bytes(const void* data, size_t n);
// An https download to a file through curl (no shell, no console window on Windows); the exit code of curl (0 = ok, -1 = cancelled by `cancel`). tick() runs about every 100 ms.
int curlDownloadFile(const std::string& url, const std::string& outFile, uint64_t maxBytes, int maxTime, const std::atomic<bool>& cancel, const std::function<void()>& tick);
std::string curlDownloadError(int rc);
// Runs curl with these arguments (no shell); its standard output is appended to *out when out is not null. The exit code of curl, -1 when cancelled, -2 when curl is missing.
int curlRun(const std::vector<std::string>& args, std::string* out, const std::atomic<bool>& cancel);
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
        bool viaMirror = false;            // the last check was answered by the mirror
        bool mirrorUnverified = false;     // from the mirror and github.com could not confirm the checksum: needs the user's explicit yes
    };
    explicit Updater(const std::string& currentVersion);
    ~Updater();
    const std::string& currentVersion() const { return current_; }
    // Where to ask: the GitHub releases of the project, or what ONAIR_UPDATE_URL says (also a file:// address, for tests)
    void setApiUrl(const std::string& url) { api_ = url; if (url.compare(0, 8, "https://") != 0) insecure_ = true; }   // an address that is not https (tests, file://) lifts the https-only rule
    void setExecutable(const std::string& path, int kindOverride = -1);   // for tests; defaults to this program
    void setWaitPid(int pid) { waitPid_ = pid; }     // for tests: the process the helper waits for (default: this one)
    void check(bool includePrerelease);              // in the background
    void download(bool confirmMirrorOnly = false);   // in the background: fetch, verify, prepare (needs phase Available). A package whose checksum
                                                     // only the mirror gave is fetched only with confirmMirrorOnly (the user's explicit yes).
    void setMirror(const MirrorConfig& c) { mirror_ = c; }
    void setAllowInsecure(bool v) { insecure_ = v; }  // for tests: http:// and file:// addresses
    void setReleaseBase(const std::string& dl) { dlBase_ = dl; }   // for tests: where github.com/.../releases/download/ is
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
    MirrorConfig mirror_;
    bool insecure_ = false, confirmed_ = false;
    std::string dlBase_ = "https://github.com/adswill/OnAir/releases/download/";
    int fetch(const std::string& url, std::string* body, const std::string& outFile, int maxTime, uint64_t maxBytes, const std::function<void()>& tick = {});
    int fetchWithFallback(const std::string& url, std::string* body, const std::string& outFile, int maxTime, uint64_t maxBytes, bool* usedMirror, const std::function<void()>& tick = {});
    bool confirmOfficial(ReleaseInfo& r);
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
