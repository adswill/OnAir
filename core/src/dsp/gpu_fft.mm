// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dsp/gpu_fft.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace dect2 {

static NSString* const kSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct FP { uint n; uint nb; uint s; uint len; uint inverse; uint useWin; };

// first pass input: x * window into the work buffer a (the batch is the second grid dimension)
kernel void load_win(constant FP& p [[buffer(0)]], device const float2* x [[buffer(1)]], device const float* win [[buffer(2)]], device float2* a [[buffer(3)]],
                     uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.n) return;
    const uint i = gid.y * p.n + gid.x;
    const float w = p.useWin ? win[gid.x] : 1.0f;
    a[i] = x[i] * w;
}
// one Stockham radix-2 pass: len = current sub-transform length, s = stride. thread t: p = t / s, q = t % s
kernel void stockham(constant FP& p [[buffer(0)]], device const float2* in [[buffer(1)]], device float2* out [[buffer(2)]], uint2 gid [[thread_position_in_grid]]) {
    const uint half_ = p.n / 2;
    if (gid.x >= half_) return;
    const uint n2 = p.len / 2, s = p.s;
    const uint pp = gid.x / s, q = gid.x % s;
    device const float2* xi = in + gid.y * p.n;
    device float2* yo = out + gid.y * p.n;
    const float2 a = xi[q + s * pp], b = xi[q + s * (pp + n2)];
    float ang = (p.inverse ? 2.0f : -2.0f) * M_PI_F * (float)pp / (float)p.len;
    float c; const float sn = precise::sincos(ang, c);
    const float2 d = a - b;
    yo[q + s * (2 * pp)] = a + b;
    yo[q + s * (2 * pp + 1)] = float2(d.x * c - d.y * sn, d.x * sn + d.y * c);
}
kernel void pow_acc(constant FP& p [[buffer(0)]], device const float2* a [[buffer(1)]], device float* acc [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
    if (gid >= p.n) return;
    float sum = 0;
    for (uint b = 0; b < p.nb; b++) { const float2 v = a[b * p.n + gid]; sum += v.x * v.x + v.y * v.y; }
    acc[gid] += sum;
}
// FIR + decimation: s = number of taps, len = decimation, n = number of outputs
kernel void fir_dec(constant FP& p [[buffer(0)]], device const float2* x [[buffer(1)]], device const float* taps [[buffer(2)]], device float2* out [[buffer(3)]],
                    uint gid [[thread_position_in_grid]]) {
    if (gid >= p.n) return;
    device const float2* xi = x + (size_t)gid * p.len;
    float2 sum = float2(0);
    for (uint t = 0; t < p.s; t++) sum += taps[t] * xi[t];
    out[gid] = sum;
}
)MSL";

struct GpuFft::Impl {
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> load = nil, pass = nil, pow = nil, fir = nil;
    std::mutex mu;
    id<MTLBuffer> a = nil, b = nil, win = nil, acc = nil, taps = nil;
    size_t aCap = 0, winCap = 0, accCap = 0, tapCap = 0;
    id<MTLBuffer> shared(size_t bytes) { return [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; }
    id<MTLComputePipelineState> make(id<MTLLibrary> lib, NSString* name) {
        NSError* err = nil;
        id<MTLFunction> fn = [lib newFunctionWithName:name];
        return fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
    }
    struct P { uint32_t n, nb, s, len, inverse, useWin; };
    // encodes the passes of `nb` transforms of n points reading from `src`; returns the buffer the result is in
    id<MTLBuffer> encodeFft(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> src, id<MTLBuffer> work, size_t n, int nb, bool inverse) {
        id<MTLBuffer> in = src, out = work;
        uint32_t len = (uint32_t)n, s = 1;
        while (len > 1) {
            P p = {(uint32_t)n, (uint32_t)nb, s, len, inverse ? 1u : 0u, 0};
            [enc setComputePipelineState:pass];
            [enc setBytes:&p length:sizeof p atIndex:0];
            [enc setBuffer:in offset:0 atIndex:1];
            [enc setBuffer:out offset:0 atIndex:2];
            [enc dispatchThreads:MTLSizeMake(n / 2, nb, 1) threadsPerThreadgroup:MTLSizeMake(std::min<size_t>(256, n / 2), 1, 1)];
            std::swap(in, out);
            len /= 2; s *= 2;
        }
        return in;
    }
    static bool pow2(size_t n) { return n >= 2 && (n & (n - 1)) == 0; }
};

GpuFft::GpuFft() : p_(new Impl) {
    @autoreleasepool {
        Impl& d = *p_;
        d.dev = MTLCreateSystemDefaultDevice();
        if (!d.dev) return;
        NSError* err = nil;
        id<MTLLibrary> lib = [d.dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) { NSLog(@"GpuFft: shader compile failed: %@", err); d.dev = nil; return; }
        d.load = d.make(lib, @"load_win"); d.pass = d.make(lib, @"stockham"); d.pow = d.make(lib, @"pow_acc"); d.fir = d.make(lib, @"fir_dec");
        if (!d.load || !d.pass || !d.pow || !d.fir) { d.dev = nil; return; }
        d.queue = [d.dev newCommandQueue];
    }
}
GpuFft::~GpuFft() = default;
GpuFft& GpuFft::instance() { static GpuFft g; return g; }
bool GpuFft::available() const { return p_->queue != nil && !getenv("DECT2_NOGPU"); }
const char* GpuFft::deviceName() const { return p_->dev ? [p_->dev.name UTF8String] : "none"; }

bool GpuFft::powerAccumulate(const cf32* x, int nb, size_t n, const float* window, float* acc) {
    if (!available() || nb < 1 || !Impl::pow2(n) || n > (1u << 20) || nb > 256) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    @autoreleasepool {
        Impl& d = *p_;
        const size_t bytes = (size_t)nb * n * sizeof(cf32);
        if (d.aCap < bytes) { d.a = d.shared(bytes); d.b = d.shared(bytes); d.aCap = bytes; }
        if (d.winCap < n * 4) { d.win = d.shared(n * 4); d.winCap = n * 4; }
        if (d.accCap < n * 4) { d.acc = d.shared(n * 4); d.accCap = n * 4; }
        id<MTLBuffer> xin = [d.dev newBufferWithBytes:x length:bytes options:MTLResourceStorageModeShared];   // (no copy: the caller's samples may not be page aligned)
        if (window) memcpy(d.win.contents, window, n * 4);
        memcpy(d.acc.contents, acc, n * 4);
        Impl::P pl = {(uint32_t)n, (uint32_t)nb, 1, (uint32_t)n, 0, window ? 1u : 0u};
        id<MTLCommandBuffer> cb = [d.queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:d.load];
        [enc setBytes:&pl length:sizeof pl atIndex:0];
        [enc setBuffer:xin offset:0 atIndex:1]; [enc setBuffer:d.win offset:0 atIndex:2]; [enc setBuffer:d.a offset:0 atIndex:3];
        [enc dispatchThreads:MTLSizeMake(n, nb, 1) threadsPerThreadgroup:MTLSizeMake(std::min<size_t>(256, n), 1, 1)];
        id<MTLBuffer> res = d.encodeFft(enc, d.a, d.b, n, nb, false);
        [enc setComputePipelineState:d.pow];
        [enc setBytes:&pl length:sizeof pl atIndex:0];
        [enc setBuffer:res offset:0 atIndex:1]; [enc setBuffer:d.acc offset:0 atIndex:2];
        [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min<size_t>(256, n), 1, 1)];
        [enc endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) return false;
        memcpy(acc, d.acc.contents, n * 4);
    }
    return true;
}

bool GpuFft::transform(cf32* x, int nb, size_t n, bool inverse) {
    if (!available() || nb < 1 || !Impl::pow2(n) || n > (1u << 20) || nb > 256) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    @autoreleasepool {
        Impl& d = *p_;
        const size_t bytes = (size_t)nb * n * sizeof(cf32);
        if (d.aCap < bytes) { d.a = d.shared(bytes); d.b = d.shared(bytes); d.aCap = bytes; }
        memcpy(d.a.contents, x, bytes);
        id<MTLCommandBuffer> cb = [d.queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        id<MTLBuffer> res = d.encodeFft(enc, d.a, d.b, n, nb, inverse);
        [enc endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) return false;
        memcpy(x, res.contents, bytes);
    }
    return true;
}

bool GpuFft::firDecimate(const cf32* x, size_t nOut, const float* taps, int nTaps, int dec, cf32* out) {
    if (!available() || nOut < 1 || nTaps < 1 || dec < 1) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    @autoreleasepool {
        Impl& d = *p_;
        const size_t inN = (nOut - 1) * (size_t)dec + (size_t)nTaps;
        id<MTLBuffer> xin = [d.dev newBufferWithBytes:x length:inN * sizeof(cf32) options:MTLResourceStorageModeShared];
        id<MTLBuffer> tb = [d.dev newBufferWithBytes:taps length:(size_t)nTaps * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> ob = d.shared(nOut * sizeof(cf32));
        Impl::P pl = {(uint32_t)nOut, 1, (uint32_t)nTaps, (uint32_t)dec, 0, 0};
        id<MTLCommandBuffer> cb = [d.queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:d.fir];
        [enc setBytes:&pl length:sizeof pl atIndex:0];
        [enc setBuffer:xin offset:0 atIndex:1]; [enc setBuffer:tb offset:0 atIndex:2]; [enc setBuffer:ob offset:0 atIndex:3];
        [enc dispatchThreads:MTLSizeMake(nOut, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min<size_t>(256, nOut), 1, 1)];
        [enc endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) return false;
        memcpy(out, ob.contents, nOut * sizeof(cf32));
    }
    return true;
}

} // namespace dect2
