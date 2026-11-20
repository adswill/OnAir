// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include "dect2/transcribe/transcriber.h"
namespace dect2 {
struct MoonshineLibraries {
    void* moonshine = nullptr;
    void* onnx = nullptr;
    std::string moonshinePath, onnxPath, error, provider, moonshineVersion, onnxVersion;
    MoonshineLibraries() = default;
    MoonshineLibraries(const MoonshineLibraries&) = delete;
    MoonshineLibraries& operator=(const MoonshineLibraries&) = delete;
    ~MoonshineLibraries();
    bool open(const std::string& modelsRoot); // first colocated pair; Moonshine chooses the provider
    void close();
};
}
