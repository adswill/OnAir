// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Speech models (whisper.cpp "ggml" files). None is shipped: the user downloads one on an explicit click, from the official model repository
// (huggingface.co/ggerganov/whisper.cpp, MIT) with its SHA-256 checked, or puts a file in the models folder (docs/TRANSCRIBE.md).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

struct SpeechModel {
    const char* id;          // "tiny.en"
    const char* file;        // "ggml-tiny.en.bin"
    uint64_t bytes;          // exact size of the file
    const char* sha256;      // of the file, lower case hex (the LFS digest the repository lists)
    bool multilingual;
    int mib;                 // size for the words on the button
};

const std::vector<SpeechModel>& speechModels();
const SpeechModel* findSpeechModel(const std::string& id);
std::string speechModelUrl(const SpeechModel& m);                          // https://huggingface.co/ggerganov/whisper.cpp/resolve/main/<file>
std::string speechModelPath(const std::string& dir, const SpeechModel& m);
bool speechModelInstalled(const std::string& dir, const SpeechModel& m);   // the file is there and has the right size
// Other "*.bin" files the user put in the folder (names), which are used as they are: their content is the user's responsibility.
std::vector<std::string> userSpeechModels(const std::string& dir);
// True for a ggml file that holds an English-only model (its name has ".en"), which cannot do other languages or auto-detect.
bool speechModelEnglishOnly(const std::string& fileName);

class ModelDownloader {
public:
    enum class State { Idle, Downloading, Verifying, Done, Failed };
    ~ModelDownloader();
    // Starts in the background; the file is written as "<file>.part", checked against the pinned SHA-256 and only then renamed. false: busy.
    bool start(const SpeechModel& m, const std::string& dir);
    void cancel();
    State state() const { return state_.load(); }
    uint64_t doneBytes() const { return done_.load(); }
    uint64_t totalBytes() const { return total_.load(); }
    std::string error() const;
    std::string modelId() const;
    // For tests: another base address than the official one (https only unless allowInsecure), and no wait for the thread
    void setBaseUrl(const std::string& u) { base_ = u; }
private:
    void run(SpeechModel m, std::string dir);
    std::atomic<State> state_{State::Idle};
    std::atomic<uint64_t> done_{0}, total_{0};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mu_;
    std::string err_, id_, base_;
    std::thread th_;
};

}  // namespace dect2
