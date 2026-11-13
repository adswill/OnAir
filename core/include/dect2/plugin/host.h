// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The plugin host: loads plugin libraries, runs each plugin instance on its own worker thread with a watchdog (or in a helper process),
// keeps the list of installed plugins, their enabled state, permissions and settings, hands out stable mode numbers, and provides safe mode.
// The C ABI the plugins see is sdk/include/onair_plugin.h; this file is the C++ side the program uses. docs/plugins.md explains it.
#pragma once
#include "dect2/automation/event.h"
#include "dect2/plugin/manifest.h"
#include "dect2/plugin/results.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::plugin {

constexpr int kFirstModeNumber = 1000;          // the "family" numbers of plugin modes start here and are never reused
constexpr double kWatchdogSeconds = 2.0;        // a plugin call that runs longer than this gets the plugin switched off
constexpr size_t kIqQueueBytes = 8u << 20;      // IQ waiting for a slow plugin; more than this is dropped (the decoder never waits)

// The current values of a plugin's settings (text keyed by setting key); typed reads use the manifest's defaults.
class SettingValues {
public:
    explicit SettingValues(const Manifest& m) : m_(m) {}
    bool set(const std::string& key, const std::string& value);      // false: not a declared key, or an invalid value (clamped values are accepted)
    std::string text(const std::string& key) const;                  // current value or default as text
    double number(const std::string& key, double def) const;
    bool flag(const std::string& key, bool def) const;
    std::map<std::string, std::string> all() const;
    void load(const std::map<std::string, std::string>& v);
private:
    const Manifest m_;
    mutable std::mutex mu_;
    std::map<std::string, std::string> v_;
};

enum class RunState { Idle, Starting, Running, Failed, Hung, Stopped };
const char* runStateName(RunState s);

// What a running plugin may call back into (set by the Manager; tests use their own).
struct Services {
    std::function<void(int level, const std::string& msg)> log;
    std::function<int(double centreHz, double sampleRate)> requestIq;
    std::function<int(const std::string& type, const automation::Event& ev)> emitEvent;     // ev already holds the fields
    std::function<int(const float* pcm, size_t n, double rate)> audioOut;
    std::string dataDir;                                          // the plugin's own folder for the filesystem permission
};

struct IqMeta { double centreHz = 0, sampleRate = 0, timestamp = 0; uint64_t firstSample = 0; };

class Runner {
public:
    virtual ~Runner() = default;
    virtual void configure(double centreHz, double sampleRate) = 0;
    virtual void feedIq(const float* iq, size_t nComplex, const IqMeta& meta) = 0;      // never blocks
    virtual void feedEvent(const automation::Event& ev) = 0;                              // never blocks
    virtual void settingChanged(const std::string& key) = 0;
    virtual void tick(double nowUnix) = 0;
    virtual RunState state() const = 0;
    virtual std::string lastError() const = 0;
    virtual uint64_t droppedBlocks() const = 0;
    virtual uint64_t processedBlocks() const = 0;
    virtual void stop() = 0;                                                              // joins a healthy worker, abandons a hung one
};

struct StartOptions {
    std::string libraryPath;                       // full path of the plugin library
    const Manifest* manifest = nullptr;
    std::string modeId;
    uint32_t granted = 0;                          // permissions the user approved
    std::shared_ptr<Results> results;
    std::shared_ptr<SettingValues> settings;
    Services services;
    double watchdogSeconds = kWatchdogSeconds;
    std::string hostExe;                           // for startProcess: the onair-plugin-host executable
};

// In-process: the library is loaded into this process; its code runs on a worker thread of its own. nullptr and err when it cannot start.
std::unique_ptr<Runner> startInProcess(const StartOptions& o, std::string& err);
// Out-of-process: a helper process loads the library; IQ goes in and results come out over pipes. A hung or crashing plugin kills only the helper.
// POSIX only for now (macOS and Linux); returns nullptr with an error on Windows.
std::unique_ptr<Runner> startProcess(const StartOptions& o, std::string& err);
// Loads the library and checks its vtable against the manifest, without creating an instance. For the manager's checks and the tests.
bool probeLibrary(const std::string& path, const Manifest& m, std::string& err);

// The loop of the helper process (tools/onair_plugin_host.cpp): reads the protocol from fdIn, writes results to fdOut. Returns the exit code.
int helperMain(const std::string& libPath, int fdIn, int fdOut);

// ---- installed plugins ----
struct Entry {
    Manifest m;
    std::string dir, libraryPath, error;           // error: why it cannot run ("" = fine)
    bool bundled = false, valid = false, enabled = false, permsAnswered = false;
    bool revoked = false;                          // switched off by the store's revocation list (error says why)
    uint32_t granted = 0;
    bool needsPrompt() const { return valid && m.permissions && !permsAnswered; }
};

struct ManagerConfig {
    std::string pluginsDir;                        // dataDir()/plugins
    std::string bundledDir;                        // examples shipped beside the program (may be empty)
    std::string hostVersion;                       // "0.2.7"
    std::string hostExe;                           // onair-plugin-host path ("" = no out-of-process mode)
};

class Manager {
public:
    explicit Manager(ManagerConfig c);
    ~Manager();
    // discovery: reads pluginsDir/<id>/manifest.json and bundledDir/<id>/manifest.json, validates them and the state file
    void scan();
    std::vector<Entry> entries() const;
    bool find(const std::string& id, Entry& out) const;

    // Safe mode. beginSession() looks for the marker a crashed run leaves behind and turns safe mode on (all plugins off) if it finds one.
    void beginSession();
    void endSession();                              // clean exit: removes the "plugin code was running" marker
    bool safeMode() const;
    std::string safeModeReason() const;             // e.g. "OnAir did not shut down cleanly while plugin tone-detector was running"
    void leaveSafeMode();                           // the user's choice; the plugin that was running stays disabled
    void forceSafeMode(bool on);                    // the command line / settings switch

    bool setEnabled(const std::string& id, bool on, std::string& err);
    bool setGranted(const std::string& id, uint32_t mask, std::string& err);       // answers the first-use prompt (mask 0 = refuse everything)
    bool setSetting(const std::string& id, const std::string& key, const std::string& value);
    std::shared_ptr<SettingValues> settings(const std::string& id);
    std::shared_ptr<Results> results(const std::string& id, const std::string& modeId = {});
    void setOutOfProcess(const std::string& id, bool on);
    bool outOfProcess(const std::string& id) const;

    // Stable numbers for the mode list ("family" numbers, from kFirstModeNumber), kept in the state file and never reused.
    int modeNumber(const std::string& pluginId, const std::string& modeId);
    bool modeFromNumber(int number, std::string& pluginId, std::string& modeId) const;
    struct ModeRow { int number; std::string pluginId, modeId, name; double defaultHz, bandwidthHz; std::vector<double> sampleRates; bool available; };
    std::vector<ModeRow> modeRows() const;          // all modes of valid plugins, numbered; available = enabled, allowed, not in safe mode

    // running
    Runner* start(const std::string& id, const std::string& modeId, std::string& err);   // nullptr + err when refused or failing
    void stop(const std::string& id, const std::string& modeId);
    void stopAll();
    Runner* running(const std::string& id, const std::string& modeId) const;
    void startActions();                            // starts every enabled action plugin
    void dispatchEvent(const automation::Event& ev);   // to the running action plugins
    void attachAutomation();                        // hooks dispatchEvent to the automation engine and makes plugin events reach its rules
    void tick(double nowUnix);                      // call about once a second: ticks plugins, notices failed ones (disables them, logs why)
    // the IQ stream a running mode asked for (0 = none)
    bool wantedIq(const std::string& id, double& centreHz, double& sampleRate) const;
    // The sound of the plugins (audio_out, permission "audio-out"): the app plays it. Called on the plugin's thread (or the helper's reader
    // thread) with mono float PCM; must not block. Without a sink the audio is dropped.
    using AudioSink = std::function<int(const std::string& pluginId, const float* pcm, size_t n, double sampleRate)>;
    void setAudioSink(AudioSink s);

    // install / remove
    bool installFile(const std::string& zipPath, std::string& installedId, std::string& err);
    bool installBytes(const std::vector<uint8_t>& zip, std::string& installedId, std::string& err);
    bool remove(const std::string& id, std::string& err);
    // The kill switch of the plugin store: id -> (version spec, reason). Written to plugins/revoked.json; every installed plugin it covers is
    // stopped, shown as failed with the reason and cannot be turned on until the entry is gone. An empty map clears the list.
    void setRevoked(const std::map<std::string, std::pair<std::string, std::string>>& r);
    std::map<std::string, std::pair<std::string, std::string>> revoked() const;

    std::vector<std::string> log(const std::string& id, size_t n = 200) const;
    std::string pluginsDir() const { return cfg_.pluginsDir; }

    struct Impl;   // public so the implementation file's helpers can name it
private:
    std::shared_ptr<Impl> p_;   // shared: a plugin thread that had to be abandoned may still log through it after the Manager is gone
    ManagerConfig cfg_;
};

} // namespace dect2::plugin
