// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace dect2 {
// Model assets are obtained from the native Moonshine dependency manifest and
// placed in the SAME models directory used by Whisper (inside a named subdirectory).
class MoonshineModelDownloader {
public:
    enum class State { Idle, Downloading, Verifying, Done, Failed };
    ~MoonshineModelDownloader();
    bool start(const std::string& root, const std::string& language, int arch, const std::string& modelName);
    void cancel() { cancel_ = true; }
    State state() const { return state_.load(); }
    uint64_t doneBytes() const { return done_.load(); }
    uint64_t totalBytes() const { return total_.load(); }
    std::string error() const;
    std::string modelName() const;
private:
    void run(std::string root, std::string language, int arch, std::string name);
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> done_{0}, total_{0};
    mutable std::mutex mu_;
    std::string error_, name_;
    std::thread thread_;
};
} // namespace dect2
