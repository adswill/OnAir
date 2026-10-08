// Shared parts of the native radio drivers (source_rtlsdr.cpp, source_airspy.cpp, source_sdrplay.cpp, ...): loading a radio's own library at run time and the
// base class every native source builds on.
#pragma once
#include "dect2/source.h"
#include "dect2/ring.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define DECT2_CALL __cdecl
#else
#include <dlfcn.h>
#include <glob.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#define DECT2_CALL
#endif


namespace dect2 {
namespace native {

// ------------------------------------------------------------------ loading a library at run time

struct DynLib {
    void* h = nullptr;
    std::string path;
    bool open(const std::vector<std::string>& names) {
        if (const char* dir = getenv("DECT2_NATIVE_LIBDIR")) {   // tests: fake libraries in one folder
            for (const auto& n : names) {
                std::string b = base(n);
                const size_t so = b.find(".so.");
                if (so != std::string::npos) b.resize(so + 3);   // the test libraries carry no version number
                if (tryOpen(std::string(dir) + "/" + b)) return true;
            }
            return false;
        }
        // The outcome always goes to the log (stderr; onair.log on Windows): when a radio does not show up, this says which library was used or why none was
        for (const auto& n : names) if (tryOpen(n)) { fprintf(stderr, "radio library: %s\n", n.c_str()); fflush(stderr); return true; }
        if (!names.empty()) { fprintf(stderr, "radio library not found: %s\n", base(names.back()).c_str()); fflush(stderr); }
        return false;
    }
    // a function the driver cannot work without: when it is missing, the log says so (once per library), since the radio then silently
    // does not show up
    template <class F> bool get(F& fn, const char* name) {
        fn = reinterpret_cast<F>(sym(name));
        if (!fn && h && !warned_) {
            warned_ = true;
            fprintf(stderr, "radio library %s lacks %s (too old, or another library with the same name): %s radio disabled\n", path.c_str(), name, stem().c_str());
            fflush(stderr);
        }
        return fn != nullptr;
    }
    // a function only some versions of the library have: its absence is expected and not reported
    template <class F> bool opt(F& fn, const char* name) { fn = reinterpret_cast<F>(sym(name)); return fn != nullptr; }
    // "rtlsdr" for librtlsdr.so.0 or rtlsdr.dll
    std::string stem() const {
        std::string b = base(path);
        if (b.compare(0, 3, "lib") == 0 && b.size() > 3) b.erase(0, 3);
        const size_t dot = b.find('.');
        if (dot != std::string::npos) b.resize(dot);
        return b;
    }

private:
    bool warned_ = false;
    void* sym(const char* name) const {
        if (!h) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress((HMODULE)h, name));
#else
        return dlsym(h, name);
#endif
    }
    static std::string base(const std::string& n) { const size_t s = n.find_last_of("/\\"); return s == std::string::npos ? n : n.substr(s + 1); }
    bool tryOpen(const std::string& n) {
#ifdef _WIN32
        // a full path: the libraries it needs in turn are looked for in its own folder first as well
        const bool full = n.find('\\') != std::string::npos || n.find('/') != std::string::npos;
        h = full ? (void*)LoadLibraryExA(n.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : (void*)LoadLibraryA(n.c_str());
        if (!h && full && GetFileAttributesA(n.c_str()) != INVALID_FILE_ATTRIBUTES) {   // the file is there but does not load: a library it needs is missing, or it was blocked
            fprintf(stderr, "radio library %s is there but does not load (Windows error %lu)\n", n.c_str(), (unsigned long)GetLastError());
            fflush(stderr);
        }
#else
        h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (h) path = n;
        return h != nullptr;
    }
};

// True when version a ("4.9.0", "0.6.0git") is newer than b: the dot-separated parts compare as numbers where both are numbers
inline bool newerVersion(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        const size_t ie = std::min(a.find('.', i), a.size()), je = std::min(b.find('.', j), b.size());
        const std::string pa = i < a.size() ? a.substr(i, ie - i) : std::string(), pb = j < b.size() ? b.substr(j, je - j) : std::string();
        const bool na = !pa.empty() && isdigit((unsigned char)pa[0]), nb = !pb.empty() && isdigit((unsigned char)pb[0]);
        if (na && nb) {
            const unsigned long long x = strtoull(pa.c_str(), nullptr, 10), y = strtoull(pb.c_str(), nullptr, 10);
            if (x != y) return x > y;
        }
        if (pa != pb) return pb.empty() || (!pa.empty() && (na != nb ? na : pa > pb));   // more parts is newer; a number beats text
        i = ie + 1; j = je + 1;
    }
    return false;
}

#ifndef _WIN32
// Every versioned copy of lib<stem> in these folders, newest first: a newer library than the fixed names (UHD 4.9 when the list stops at 4.8)
// is found even without the -dev package's unversioned link. Linux: lib<stem>.so.<version>, macOS: lib<stem>.<version>.dylib.
inline std::vector<std::string> versionedLibs(const char* stem, const std::vector<std::string>& dirs) {
    std::vector<std::pair<std::string, std::string>> found;   // version, path
    for (const auto& dir : dirs) {
        if (dir.empty()) continue;
#ifdef __APPLE__
        const std::string head = std::string("lib") + stem + ".", tail = ".dylib", pattern = dir + head + "*" + tail;
#else
        const std::string head = std::string("lib") + stem + ".so.", tail, pattern = dir + head + "*";
#endif
        glob_t g{};
        if (glob(pattern.c_str(), 0, nullptr, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++) {
                const std::string p = g.gl_pathv[k];
                const size_t at = p.rfind(head);
                if (at == std::string::npos || p.size() < at + head.size() + tail.size()) continue;
                found.push_back({p.substr(at + head.size(), p.size() - at - head.size() - tail.size()), p});
            }
        }
        globfree(&g);
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return newerVersion(x.first, y.first); });
    std::vector<std::string> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}
#endif

// Where each platform keeps a library called `stem` (with the usual name variants)
inline std::vector<std::string> libNames(const char* stem, std::initializer_list<const char*> soVersions, std::initializer_list<const char*> winNames) {
    std::vector<std::string> v;
#if defined(_WIN32)
    // The copies shipped in the program's folder first, by full path: a bare name would let Windows hand over another program's copy of the
    // same library from the PATH (SDR#, PothosSDR and others install their own rtlsdr.dll / airspy.dll), and that may not work with OnAir.
    // Then the folders of the vendor installers (PothosSDR, which also carries LimeSuite.dll, and UHD), each only if it exists; by full
    // path, so that the libraries such a DLL needs in turn are found in its own folder (tryOpen loads it with LOAD_WITH_ALTERED_SEARCH_PATH).
    // Bare names only after that, for radios whose library is not shipped with OnAir and that the user installed (libiio puts its DLL in System32).
    {
        char path[MAX_PATH * 2] = {0};
        if (GetModuleFileNameA(nullptr, path, (DWORD)sizeof path - 1)) {
            std::string dir = path;
            const size_t cut = dir.find_last_of("\\/");
            if (cut != std::string::npos) { dir.resize(cut + 1); for (const char* w : winNames) v.push_back(dir + w); }
        }
    }
    if (const char* pf = getenv("ProgramFiles")) {
        for (const char* sub : {"\\PothosSDR\\bin\\", "\\UHD\\bin\\"}) {
            const std::string dir = std::string(pf) + sub;
            const DWORD a = GetFileAttributesA(dir.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) for (const char* w : winNames) v.push_back(dir + w);
        }
    }
    for (const char* w : winNames) v.push_back(w);
    (void)stem; (void)soVersions;
#elif defined(__APPLE__)
    (void)winNames;
    // the libraries inside the app (Contents/Frameworks, put there by tools/package/bundle_macos.py) come first, then the usual places
    std::vector<std::string> dirs;
    {
        char path[4096];
        uint32_t size = sizeof path;
        if (_NSGetExecutablePath(path, &size) == 0) {
            std::string d(path);
            const size_t sl = d.find_last_of('/');
            if (sl != std::string::npos) dirs.push_back(d.substr(0, sl) + "/../Frameworks/");
        }
    }
    for (const char* dir : {"", "/opt/homebrew/lib/", "/usr/local/lib/", "/opt/local/lib/"}) dirs.push_back(dir);
    for (const auto& dir : dirs) {
        for (const char* ver : soVersions) v.push_back(dir + std::string("lib") + stem + ver + ".dylib");
        v.push_back(dir + std::string("lib") + stem + ".dylib");
    }
    for (auto& p : versionedLibs(stem, dirs)) if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
#else
    (void)winNames;
    for (const char* ver : soVersions) v.push_back(std::string("lib") + stem + ".so" + ver);
    v.push_back(std::string("lib") + stem + ".so");
    // the multiarch folder of this build (Debian/Ubuntu); other distributions use lib64
    std::vector<std::string> dirs = {"/usr/local/lib/", "/usr/local/lib64/", "/usr/lib/", "/usr/lib64/"};
#if defined(__x86_64__)
    dirs.push_back("/usr/lib/x86_64-linux-gnu/");
#elif defined(__aarch64__)
    dirs.push_back("/usr/lib/aarch64-linux-gnu/");
#elif defined(__arm__)
    dirs.push_back("/usr/lib/arm-linux-gnueabihf/");
#elif defined(__i386__)
    dirs.push_back("/usr/lib/i386-linux-gnu/");
#elif defined(__riscv) && __riscv_xlen == 64
    dirs.push_back("/usr/lib/riscv64-linux-gnu/");
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
    dirs.push_back("/usr/lib/powerpc64le-linux-gnu/");
#endif
    for (auto& p : versionedLibs(stem, dirs)) v.push_back(p);
#endif
    return v;
}

inline bool nativeDisabled() { const char* e = getenv("DECT2_NO_NATIVE"); return e && *e && *e != '0'; }

// ------------------------------------------------------------------ USB open errors in plain words

#ifdef _WIN32
constexpr bool kOnWindows = true;
#else
constexpr bool kOnWindows = false;
#endif

// libusb's error codes (also what librtlsdr's open returns); the numbers are fixed by the libusb API, so no libusb header is needed here
inline const char* usbErrorName(int code) {
    switch (code) {
    case -1: return "LIBUSB_ERROR_IO";
    case -2: return "LIBUSB_ERROR_INVALID_PARAM";
    case -3: return "LIBUSB_ERROR_ACCESS";
    case -4: return "LIBUSB_ERROR_NO_DEVICE";
    case -5: return "LIBUSB_ERROR_NOT_FOUND";
    case -6: return "LIBUSB_ERROR_BUSY";
    case -7: return "LIBUSB_ERROR_TIMEOUT";
    case -8: return "LIBUSB_ERROR_OVERFLOW";
    case -9: return "LIBUSB_ERROR_PIPE";
    case -10: return "LIBUSB_ERROR_INTERRUPTED";
    case -11: return "LIBUSB_ERROR_NO_MEM";
    case -12: return "LIBUSB_ERROR_NOT_SUPPORTED";
    default: return "LIBUSB_ERROR_OTHER";
    }
}
inline const char* usbErrorMeaning(int code, bool windows = kOnWindows) {
    switch (code) {
    case -1: return "A USB transfer failed: unplug and replug the radio, or try another cable or USB port.";
    case -3:
        if (windows) return "Windows refused access: another program has the radio open, or the radio does not have the WinUSB driver.";
#ifdef __linux__
        return "This user has no permission to open the radio: OnAir's udev rules are not installed.";
#else
        return "Access was refused: another program may be using the radio.";
#endif
    case -4: return "The radio was unplugged.";
    case -5: return windows ? "The radio is not there any more, or Windows has no usable driver for it." : "The radio is not there any more.";
    case -6: return "Another program is using the radio.";
    case -7: return "The radio did not answer in time: unplug and replug it.";
    case -9: return "The radio refused the request: its firmware may be too old.";
    case -12: return windows ? "Windows has no driver for the radio that libusb can use." : "The operating system does not let libusb use this radio.";
    default: return "The radio could not be opened.";
    }
}
// The one sentence every Windows USB open failure that points at a missing driver ends with (HackRF, RTL-SDR, Airspy, bladeRF)
inline const char* windowsUsbDriverHint() { return "On Windows this radio needs the WinUSB driver: install it once with Zadig (zadig.akeo.ie)"; }
// codes that on Windows mean "no WinUSB driver" (libusb: NOT_SUPPORTED, NOT_FOUND, ACCESS)
inline bool usbDriverMissing(int code) { return code == -12 || code == -5 || code == -3; }
// "LIBUSB_ERROR_ACCESS: <what it means>" and, on Windows, the Zadig hint where it applies
inline std::string usbErrorText(int code, bool windows = kOnWindows) {
    std::string s = std::string(usbErrorName(code)) + ": " + usbErrorMeaning(code, windows);
    if (windows && usbDriverMissing(code)) s += std::string(" ") + windowsUsbDriverHint();
    return s;
}

// ------------------------------------------------------------------ the common part of every native source

// An open that failed for a reason that may pass by itself (the radio was just closed by us or another program, a libusb handle is still
// closing, a USB hiccup): worth trying again. Drivers can say so themselves through openFailure_; otherwise their message decides.
inline bool openErrorLooksTransient(const std::string& e) {
    std::string l(e);
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    for (const char* k : {"busy", "in use", "i/o", "input/output", "libusb_error_io", "not found", "no device", "not available", "unplugged",
                          "cannot open", "cannot connect", "timed out", "timeout"})
        if (l.find(k) != std::string::npos) return true;
    return false;
}

class NativeSource : public IqSource {
public:
    ~NativeSource() override = default;
    // A failed open is tried again up to kOpenRetries times, 0.5 s apart, when the error may pass by itself (#28): a radio that was just
    // stopped, or that another program is letting go of, is often busy for a moment.
    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        ring_ = &ring;
        { std::lock_guard<std::mutex> lk(cfg_); last_ = s; }
        reconnectErr_.clear();
        for (int attempt = 0;; attempt++) {
            std::string e;
            openFailure_ = OpenFailure::Unknown;
            if (openDevice(s, e)) { err = e; break; }   // (e may carry a warning, such as a lower sample rate)
            closeDevice();
            if (attempt >= kOpenRetries || !openRetryable(e)) { err = e; return false; }
            fprintf(stderr, "radio did not open (%s): trying again (%d of %d)\n", e.c_str(), attempt + 1, kOpenRetries);
            fflush(stderr);
            std::this_thread::sleep_for(std::chrono::milliseconds(kOpenRetryMs));
        }
        open_ = true;
        startStream();
        return true;
    }
    void stop() override {
        stopStream();
        std::lock_guard<std::mutex> lk(devMu_);
        closeDevice();
        open_ = false;
    }
    bool retune(const TuneSettings& s, std::string& err) override {
        std::lock_guard<std::mutex> lk(cfg_);
        last_ = s;   // a radio that comes back after an unplug is opened with these
        if (!open_) return true;   // the radio is away: it gets the new settings when it is back
        const bool rateChange = s.sampleRate > 0 && std::fabs(s.sampleRate - requested_) > 1.0;
        if (rateChange && restartForRate()) {   // most radios change their sample rate only while not streaming
            stopStream();
            const bool ok = open_ ? configure(s, err, false) : true;
            startStream();
            return ok;
        }
        return configure(s, err, true);
    }
    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }

    static constexpr int kOpenRetries = 3;
    static constexpr int kOpenRetryMs = 500;
    static constexpr int kReconnectMs = 1000;

protected:
    virtual bool openDevice(const TuneSettings& s, std::string& err) = 0;   // open and configure()
    virtual bool configure(const TuneSettings& s, std::string& err, bool live) = 0;
    virtual void closeDevice() = 0;          // must be safe to call twice, and after a failed openDevice()
    virtual void streamLoop() = 0;            // runs until run_ is false (or the stream fails: then the radio is opened again)
    virtual void interrupt() {}              // makes a blocking read in streamLoop() return
    virtual bool restartForRate() const { return true; }
    // whether a failed openDevice() is worth trying again (start() only; after an unplug every attempt is)
    virtual bool openRetryable(const std::string& err) const {
        if (openFailure_ == OpenFailure::Retry) return true;
        if (openFailure_ == OpenFailure::Final) return false;
        return openErrorLooksTransient(err);
    }

    void startStream() {
        run_ = true;
        th_ = std::thread([this] { streamThread(); });
    }
    void stopStream() {
        if (!th_.joinable()) return;
        run_ = false;
        { std::lock_guard<std::mutex> lk(waitMu_); }
        wake_.notify_all();
        { std::lock_guard<std::mutex> lk(devMu_); interrupt(); }
        th_.join();
    }
    void push(const cf32* p, size_t n) { if (ring_ && n) ring_->write(p, n); }
    // reports a rate below what the channel needs (the receiver may still work for narrower channels)
    void checkRate(double want, std::string& err) {
        if (rate_ < want * 0.97) {
            char b[200];
            snprintf(b, sizeof b, "this radio only offers %.2f Msps, %.2f Msps were requested for this channel width", rate_ / 1e6, want / 1e6);
            err = b;
        }
    }

    IqRing* ring_ = nullptr;
    std::atomic<bool> run_{false};
    std::thread th_;
    std::mutex cfg_;
    double rate_ = 0, requested_ = 0;
    std::vector<cf32> conv_;
    // set by openDevice() when it knows better than the message whether trying again can help
    enum class OpenFailure { Unknown, Retry, Final };
    OpenFailure openFailure_ = OpenFailure::Unknown;

private:
    // The stream thread. When streamLoop() returns while nobody asked it to stop, the radio is gone (unplugged, or its stream died): it is
    // closed, and opened again every second with the settings in use until it is back; then the samples go on into the same ring.
    void streamThread() {
        while (run_) {
            if (open_) streamLoop();
            if (!run_ || waitStop(std::chrono::milliseconds(100))) return;   // a stop that was already on its way is not a loss
            if (open_) {
                fprintf(stderr, "radio lost: the stream ended (unplugged?); trying to open it again every second\n");
                fflush(stderr);
                if (!closeWhileRunning()) return;
            }
            while (!reopen()) if (waitStop(std::chrono::milliseconds(kReconnectMs))) return;
            fprintf(stderr, "radio reconnected\n");
            fflush(stderr);
        }
    }
    // true when stop() was asked for within d
    bool waitStop(std::chrono::milliseconds d) {
        std::unique_lock<std::mutex> lk(waitMu_);
        return wake_.wait_for(lk, d, [this] { return !run_.load(); });
    }
    // retune() holds cfg_ while it waits for this thread to end: never block on it
    bool closeWhileRunning() {
        while (run_) {
            std::unique_lock<std::mutex> lk(cfg_, std::try_to_lock);
            if (!lk.owns_lock()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            std::lock_guard<std::mutex> dl(devMu_);
            closeDevice();
            open_ = false;
            return true;
        }
        return false;
    }
    bool reopen() {
        if (!run_) return false;
        std::unique_lock<std::mutex> lk(cfg_, std::try_to_lock);
        if (!lk.owns_lock()) return false;
        std::lock_guard<std::mutex> dl(devMu_);
        if (!run_) return false;
        std::string e;
        openFailure_ = OpenFailure::Unknown;
        if (openDevice(last_, e)) { open_ = true; reconnectErr_.clear(); return true; }
        closeDevice();
        if (e != reconnectErr_) {   // each new reason once, not every second
            reconnectErr_ = e;
            fprintf(stderr, "radio not available yet: %s\n", e.c_str());
            fflush(stderr);
        }
        return false;
    }

    TuneSettings last_;                 // the settings in use (cfg_)
    std::atomic<bool> open_{false};     // the device is open (it is closed while the radio is away)
    std::mutex devMu_;                  // opening/closing on the stream thread against interrupt() from stop()
    std::mutex waitMu_;
    std::condition_variable wake_;
    std::string reconnectErr_;
};

inline std::string trimmed(const char* s, size_t max) {
    std::string r(s, strnlen(s, max));
    while (!r.empty() && (r.back() == ' ' || r.back() == '\n')) r.pop_back();
    return r;
}


// one entry per radio: list the connected ones, and open one (the arguments come from the list)
void listRtl(std::vector<DeviceInfo>& out);
void listAirspy(std::vector<DeviceInfo>& out);
void listBlade(std::vector<DeviceInfo>& out);
void listLime(std::vector<DeviceInfo>& out);
void listPluto(std::vector<DeviceInfo>& out);
void listUsrp(std::vector<DeviceInfo>& out);
void listSdrplay(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeRtl(const DeviceInfo& d);
std::unique_ptr<IqSource> makeAirspy(const DeviceInfo& d);
std::unique_ptr<IqSource> makeBlade(const DeviceInfo& d);
std::unique_ptr<IqSource> makeLime(const DeviceInfo& d);
std::unique_ptr<IqSource> makePluto(const DeviceInfo& d);
std::unique_ptr<IqSource> makeUsrp(const DeviceInfo& d);
std::unique_ptr<IqSource> makeSdrplay(const DeviceInfo& d);

} // namespace native
} // namespace dect2
