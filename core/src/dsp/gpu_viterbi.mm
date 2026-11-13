// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dsp/gpu_viterbi.h"
#include "dect2/common/diag.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace dect2 {

// The same trellis and arithmetic as the NEON path of dvbt::Viterbi: new state ns (j = ns & 31) comes from the old states 2j and 2j+1, the branch metric
// M_j = +-sx +-sy is one number per butterfly, path metrics are 16 bit and saturate, they are brought back by the best one every 4 steps, and a
// decision is 1 when the odd predecessor is strictly better. One simdgroup of 32 lanes per chunk, two states per lane; the 64 decisions of a step are two ballots.
static NSString* const kSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VP { uint steps; uint C; uint L; uint decStride; uint chunkBase; };

kernel void viterbi(constant VP& p [[buffer(0)]], device const char* soft [[buffer(1)]], device uchar* bitsOut [[buffer(2)]],
                    device int* metrics [[buffer(3)]], device uint* decAll [[buffer(4)]],
                    uint tid [[thread_position_in_threadgroup]], uint ci [[threadgroup_position_in_grid]]) {
    // one simdgroup (32 lanes) per chunk, lane j holds the new states j and j+32; the old states 2j and 2j+1 come through shuffles, so there is no
    // barrier in the loop
    const uint a = (ci + p.chunkBase) * p.C, b = min(p.steps, a + p.C);
    const uint lo = a > p.L ? a - p.L : 0, hi = min(p.steps, b + p.L);
    const uint len = hi - lo;
    threadgroup short pmEnd[64];
    threadgroup ulong tbuf[64];
    device uint* dec = decAll + (ulong)ci * p.decStride;
    const uint j = tid;
    const int sx = (popcount((2u * j) & 0x79u) & 1u) ? -1 : 1;
    const int sy = (popcount((2u * j) & 0x5Bu) & 1u) ? -1 : 1;
    const uint srcA = (2u * j) & 31u, srcB = (2u * j + 1u) & 31u;
    const bool firstHalf = j < 16;
    int pl = 0, ph = 0;
    int off = 0;
    for (uint t0 = 0; t0 < len; t0 += 32) {
        // the soft values of the next 32 steps, one per lane (one load, not 32 latencies in a row), handed on by shuffles
        const uint tl = t0 + tid;
        const int ma = tl < len ? (int)soft[2 * (lo + tl)] : 0, mb = tl < len ? (int)soft[2 * (lo + tl) + 1] : 0;
        const uint cnt = min(32u, len - t0);
        for (uint i = 0; i < cnt; i++) {
            const uint t = t0 + i;
            const int sa = simd_shuffle(ma, (ushort)i), sb = simd_shuffle(mb, (ushort)i);
            const int M = sx * sa + sy * sb;
            const int aL = simd_shuffle(pl, srcA), aH = simd_shuffle(ph, srcA);
            const int bL = simd_shuffle(pl, srcB), bH = simd_shuffle(ph, srcB);
            const int A = firstHalf ? aL : aH, B = firstHalf ? bL : bH;
            const int p0 = clamp(A + M, -32768, 32767), p1 = clamp(B - M, -32768, 32767);
            const int r0 = clamp(A - M, -32768, 32767), r1 = clamp(B + M, -32768, 32767);
            int nl = max(p0, p1), nh = max(r0, r1);
            const ulong v0 = (ulong)simd_ballot(p1 > p0), v1 = (ulong)simd_ballot(r1 > r0);
            if (tid == 0) { dec[2 * t] = (uint)v0; dec[2 * t + 1] = (uint)v1; }
            if ((t & 3u) == 3u) {
                const int top = simd_max(max(nl, nh));
                off += top;
                nl = (int)(short)(nl - top);
                nh = (int)(short)(nh - top);
            }
            pl = nl; ph = nh;
        }
    }
    pmEnd[tid] = (short)pl; pmEnd[tid + 32] = (short)ph;
    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
    int state = 0;
    if (tid == 0) {
        int bm = pmEnd[0];
        for (int s = 1; s < 64; s++) if (pmEnd[s] > bm) { bm = pmEnd[s]; state = s; }
        metrics[ci] = off + bm;
    }
    for (int base = len ? (int)((len - 1) / 64) * 64 : 0; len && base >= 0; base -= 64) {
        for (uint q = tid; q < 64; q += 32) {
            const uint tt = (uint)base + q;
            tbuf[q] = tt < len ? ((ulong)dec[2 * tt] | ((ulong)dec[2 * tt + 1] << 32)) : 0ul;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (tid == 0) {
            for (int k = (int)min(63u, len - 1 - (uint)base); k >= 0; k--) {
                const uint abs = lo + (uint)base + (uint)k;
                if (abs >= a && abs < b) bitsOut[abs] = (uchar)(state >> 5);
                state = ((state & 31) << 1) | (int)((tbuf[k] >> state) & 1ul);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
}
)MSL";

struct GpuViterbi::Impl {
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pso = nil;
    std::mutex mu;
    id<MTLBuffer> soft, bits, metrics, dec;
    size_t softCap = 0, chunkCap = 0;
    static constexpr size_t kC = 16384, kL = 192, kMaxChunks = 256;
    id<MTLBuffer> shared(size_t bytes) { return [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; }
};

GpuViterbi::GpuViterbi() : p_(new Impl) {
    @autoreleasepool {
        Impl& d = *p_;
        d.dev = MTLCreateSystemDefaultDevice();
        if (!d.dev) return;
        NSError* err = nil;
        id<MTLLibrary> lib = [d.dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) { NSLog(@"GpuViterbi: shader compile failed: %@", err); DIAG(543446, "The Metal Viterbi shader did not compile (%s), so the CPU Viterbi decoder is used.", err ? err.localizedDescription.UTF8String : "no reason given"); d.dev = nil; return; }
        id<MTLFunction> fn = [lib newFunctionWithName:@"viterbi"];
        d.pso = [d.dev newComputePipelineStateWithFunction:fn error:&err];
        if (!d.pso) { NSLog(@"GpuViterbi: pipeline failed: %@", err); DIAG(309719, "The Metal Viterbi pipeline could not be created (%s), so the CPU Viterbi decoder is used.", err ? err.localizedDescription.UTF8String : "no reason given"); d.dev = nil; return; }
        // the kernel's ballots assume a simdgroup of 32 threads
        if (d.pso.threadExecutionWidth != 32 || d.pso.maxTotalThreadsPerThreadgroup < 32) {
            DIAG_DEBUG(470707, "The Metal GPU runs %d threads per SIMD group, not the 32 the Viterbi kernel needs, so the CPU Viterbi decoder is used.", (int)d.pso.threadExecutionWidth);
            d.pso = nil; d.dev = nil; return;
        }
        d.queue = [d.dev newCommandQueue];
    }
}
GpuViterbi::~GpuViterbi() = default;
GpuViterbi& GpuViterbi::instance() { static GpuViterbi g; return g; }
bool GpuViterbi::available() const { return p_->pso != nil; }

bool GpuViterbi::decode(const std::vector<int8_t>& soft, std::vector<uint8_t>& bits, long* metric) {
    if (!available()) return false;
    const size_t steps = soft.size() / 2;
    bits.assign(steps, 0);
    if (metric) *metric = 0;
    if (!steps) return true;
    std::lock_guard<std::mutex> lk(p_->mu);
    @autoreleasepool {
        Impl& d = *p_;
        const size_t C = Impl::kC, L = Impl::kL;
        const size_t chunks = (steps + C - 1) / C;
        if (d.softCap < steps) { d.soft = d.shared(steps * 2); d.bits = d.shared(steps); d.softCap = steps; }
        const size_t nc = std::min(chunks, Impl::kMaxChunks);
        if (d.chunkCap < nc) { d.metrics = d.shared(nc * 4); d.dec = d.shared(nc * 2 * (C + 2 * L) * 4); d.chunkCap = nc; }
        memcpy(d.soft.contents, soft.data(), steps * 2);
        long total = 0;
        for (size_t c0 = 0; c0 < chunks; c0 += Impl::kMaxChunks) {
            const size_t cnt = std::min(Impl::kMaxChunks, chunks - c0);
            struct { uint32_t steps, C, L, decStride, chunkBase; } P = {(uint32_t)steps, (uint32_t)C, (uint32_t)L, (uint32_t)(2 * (C + 2 * L)), (uint32_t)c0};
            id<MTLCommandBuffer> cb = [d.queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:d.pso];
            [enc setBytes:&P length:sizeof P atIndex:0];
            [enc setBuffer:d.soft offset:0 atIndex:1];
            [enc setBuffer:d.bits offset:0 atIndex:2];
            [enc setBuffer:d.metrics offset:0 atIndex:3];
            [enc setBuffer:d.dec offset:0 atIndex:4];
            [enc dispatchThreadgroups:MTLSizeMake(cnt, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) return false;
            const int* mt = (const int*)d.metrics.contents;
            for (size_t i = 0; i < cnt; i++) total += mt[i];
        }
        memcpy(bits.data(), d.bits.contents, steps);
        if (metric) *metric = total;
    }
    return true;
}

} // namespace dect2
