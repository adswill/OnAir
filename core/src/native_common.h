// Shared parts of the native radio drivers (source_rtlsdr.cpp, source_airspy.cpp, ...): loading a radio's own library at run time and the
// base class every native source builds on.
#pragma once
#include "dect2/source.h"
#include "dect2/ring.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
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
        for (const auto& n : names) if (tryOpen(n)) return true;
        return false;
    }
    template <class F> bool get(F& fn, const char* name) {
        if (!h) return false;
#ifdef _WIN32
        fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress((HMODULE)h, name)));
#else
        fn = reinterpret_cast<F>(dlsym(h, name));
#endif
        return fn != nullptr;
    }

private:
    static std::string base(const std::string& n) { const size_t s = n.find_last_of("/\\"); return s == std::string::npos ? n : n.substr(s + 1); }
    bool tryOpen(const std::string& n) {
#ifdef _WIN32
        // a full path: the libraries it needs in turn are looked for in its own folder first as well
        const bool full = n.find('\\') != std::string::npos || n.find('/') != std::string::npos;
        h = full ? (void*)LoadLibraryExA(n.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : (void*)LoadLibraryA(n.c_str());
#else
        h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (h) path = n;
        if (h && getenv("DECT2_DEBUG")) fprintf(stderr, "native radio library loaded: %s\n", n.c_str());
        return h != nullptr;
    }
};

// Where each platform keeps a library called `stem` (with the usual name variants)
inline std::vector<std::string> libNames(const char* stem, std::initializer_list<const char*> soVersions, std::initializer_list<const char*> winNames) {
    std::vector<std::string> v;
#if defined(_WIN32)
    // The copies shipped in the program's folder first, by full path: a bare name would let Windows hand over another program's copy of the
    // same library from the PATH (SDR#, PothosSDR and others install their own rtlsdr.dll / airspy.dll), and that may not work with OnAir.
    // Bare names only after that, for radios whose library is not shipped with OnAir and that the user installed.
    {
        char path[MAX_PATH * 2] = {0};
        if (GetModuleFileNameA(nullptr, path, (DWORD)sizeof path - 1)) {
            std::string dir = path;
            const size_t cut = dir.find_last_of("\\/");
            if (cut != std::string::npos) { dir.resize(cut + 1); for (const char* w : winNames) v.push_back(dir + w); }
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
#else
    (void)winNames;
    for (const char* ver : soVersions) v.push_back(std::string("lib") + stem + ".so" + ver);
    v.push_back(std::string("lib") + stem + ".so");
#endif
    return v;
}

inline bool nativeDisabled() { const char* e = getenv("DECT2_NO_NATIVE"); return e && *e && *e != '0'; }

// ------------------------------------------------------------------ the common part of every native source

class NativeSource : public IqSource {
public:
    ~NativeSource() override = default;
    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        ring_ = &ring;
        if (!openDevice(s, err)) { closeDevice(); return false; }
        startStream();
        return true;
    }
    void stop() override {
        stopStream();
        closeDevice();
    }
    bool retune(const TuneSettings& s, std::string& err) override {
        std::lock_guard<std::mutex> lk(cfg_);
        const bool rateChange = s.sampleRate > 0 && std::fabs(s.sampleRate - requested_) > 1.0;
        if (rateChange && restartForRate()) {   // most radios change their sample rate only while not streaming
            stopStream();
            const bool ok = configure(s, err, false);
            startStream();
            return ok;
        }
        return configure(s, err, true);
    }
    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }

protected:
    virtual bool openDevice(const TuneSettings& s, std::string& err) = 0;   // open and configure()
    virtual bool configure(const TuneSettings& s, std::string& err, bool live) = 0;
    virtual void closeDevice() = 0;
    virtual void streamLoop() = 0;            // runs until run_ is false (or the stream fails)
    virtual void interrupt() {}              // makes a blocking read in streamLoop() return
    virtual bool restartForRate() const { return true; }

    void startStream() {
        run_ = true;
        th_ = std::thread([this] { streamLoop(); });
    }
    void stopStream() {
        if (!th_.joinable()) return;
        run_ = false;
        interrupt();
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
std::unique_ptr<IqSource> makeRtl(const DeviceInfo& d);
std::unique_ptr<IqSource> makeAirspy(const DeviceInfo& d);
std::unique_ptr<IqSource> makeBlade(const DeviceInfo& d);
std::unique_ptr<IqSource> makeLime(const DeviceInfo& d);
std::unique_ptr<IqSource> makePluto(const DeviceInfo& d);
std::unique_ptr<IqSource> makeUsrp(const DeviceInfo& d);

} // namespace native
} // namespace dect2
