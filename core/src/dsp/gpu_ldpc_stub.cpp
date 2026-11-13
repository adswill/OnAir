// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Builds without a GPU backend (Android): the GPU decoder reports itself as unavailable and the engine's
// Auto/CPU modes use the CPU decoder. Linux uses gpu_ldpc_vulkan.cpp.
#include "dect2/dsp/gpu_ldpc.h"
#include <cstdint>

namespace dect2 {

struct GpuLdpc::Impl {};
GpuLdpc::GpuLdpc() : p_(new Impl) {}
GpuLdpc::~GpuLdpc() = default;
GpuLdpc& GpuLdpc::instance() { static GpuLdpc g; return g; }
bool GpuLdpc::available() const { return false; }
const char* GpuLdpc::deviceName() const { return "none"; }
bool GpuLdpc::decode(const LdpcCode&, const float*, int, int, uint8_t*, uint8_t*, int*, float, bool) { return false; }

bool GpuLdpc::qcAvailable() const { return false; }
bool GpuLdpc::decodeQc(const QcLdpc&, const float*, int, int, uint8_t*, uint8_t*, int*, float, bool) { return false; }
bool GpuLdpc::boxCorrKernels() const { return false; }

} // namespace dect2
