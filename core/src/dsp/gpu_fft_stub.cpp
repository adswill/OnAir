// Builds without the Metal FFT (everything except macOS for now): the GPU FFT reports itself as unavailable and the CPU transforms are used.
#include "dect2/dsp/gpu_fft.h"

namespace dect2 {

struct GpuFft::Impl {};
GpuFft::GpuFft() : p_(new Impl) {}
GpuFft::~GpuFft() = default;
GpuFft& GpuFft::instance() { static GpuFft g; return g; }
bool GpuFft::available() const { return false; }
const char* GpuFft::deviceName() const { return "none"; }
bool GpuFft::powerAccumulate(const cf32*, int, size_t, const float*, float*) { return false; }
bool GpuFft::transform(cf32*, int, size_t, bool) { return false; }
bool GpuFft::firDecimate(const cf32*, size_t, const float*, int, int, cf32*) { return false; }

} // namespace dect2
