// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dsp/gpu_ldpc.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <map>
#include <mutex>

namespace dect2 {

static NSString* const kSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;
constant int Z = 360;
struct P { int k, n, q, G, maxIter; float alpha; uint msgPer; uint postPer; };
// log(1 + e^-t), t >= 0: the four tangents of softplusNeg in ldpc.cpp
inline float spNeg(float t) { return max(0.f, max(max(0.6854f - 0.4378f * t, 0.5822f - 0.2689f * t), max(0.3653f - 0.1192f * t, 0.1323f - 0.0293f * t))); }

inline int vidx(int j, int nc0, int c0, int r, int m, int q, int G, device const int* cGrp, device const int* cSh) {
    if (j < nc0) { int n = m - cSh[c0 + j]; if (n < 0) n += Z; return cGrp[c0 + j] * Z + n; }
    const int pb = G * Z;
    if (j == nc0) return pb + r * (Z + 1) + 1 + m;
    return pb + (r > 0 ? (r - 1) * (Z + 1) + 1 + m : (q - 1) * (Z + 1) + m);
}

kernel void ldpc(constant P& p [[buffer(0)]], device const int* lStart [[buffer(1)]], device const int* cGrp [[buffer(2)]],
                 device const int* cSh [[buffer(3)]], device const uint* mBase [[buffer(4)]], device float* postAll [[buffer(5)]],
                 device float* msgAll [[buffer(6)]], device const float* llrAll [[buffer(7)]], device uchar* hardAll [[buffer(8)]],
                 device int* info [[buffer(9)]], device const int* cRank [[buffer(10)]], device const int* layerR [[buffer(11)]], uint mt [[thread_position_in_threadgroup]], uint blk [[threadgroup_position_in_grid]]) {
    const int m = (int)mt;
    device float* inf = postAll + (ulong)blk * p.postPer;
    device float* msg = msgAll + (ulong)blk * p.msgPer;
    device const float* llr = llrAll + (ulong)blk * (ulong)p.n;
    const int q = p.q, G = p.G, pb = G * Z;
    threadgroup atomic_int bad;
    for (int i = m; i < G * Z; i += Z) inf[i] = llr[i];
    for (int r = 0; r < q; r++) {
        inf[pb + r * (Z + 1) + 1 + m] = llr[p.k + r + q * m];
        if (m == 0) inf[pb + r * (Z + 1)] = 1e4f;
    }
    for (uint i = (uint)m; i < p.msgPer; i += Z) msg[i] = 0.f;
    threadgroup_barrier(mem_flags::mem_device);
    int it = 0;
    bool ok = false;
    while (it < p.maxIter) {
        for (int r = 0; r < q; r++) {
            const int c0 = lStart[r], nc0 = lStart[r + 1] - c0, nc = nc0 + 2;
            device float* mr = msg + mBase[r];
            float min1 = 1e30f, min2 = 1e30f;
            int idx1 = -1;
            uint sg = 0;
            for (int j = 0; j < nc; j++) {
                float x = inf[vidx(j, nc0, c0, r, m, q, G, cGrp, cSh)] - mr[j * Z + m];
                float a = fabs(x);
                sg ^= as_type<uint>(x) & 0x80000000u;
                if (a < min1) { min2 = min1; min1 = a; idx1 = j; }
                else if (a < min2) min2 = a;
            }
            if (p.alpha < 0.f) min1 = max(0.f, min1 + spNeg(min1 + min2) - spNeg(min2 - min1));   // the two-minimum correction (LdpcCode::decodeFast)
            // A layer may reach the same variable group twice (a variable then has two edges in the layer, owned by different
            // threads). Every new message is computed from the posteriors as they were at the start of the layer, and the
            // posterior increments are applied afterwards: unique edges at once, repeated ones one rank at a time.
            const int R = layerR[r];
            float dl[64];
            for (int j = 0; j < nc; j++) {
                const int vi = vidx(j, nc0, c0, r, m, q, G, cGrp, cSh);
                float x = inf[vi] - mr[j * Z + m];
                float mag = (idx1 == j ? min2 : min1) * fabs(p.alpha);
                float nm = as_type<float>(as_type<uint>(mag) | (sg ^ (as_type<uint>(x) & 0x80000000u)));
                const float d = nm - mr[j * Z + m];
                mr[j * Z + m] = nm;
                if (R == 0) { if (!(r == 0 && j == nc0 + 1 && m == 0)) inf[vi] += d; } // the pad variable stays known
                else dl[j] = d;
            }
            if (R > 0) {
                threadgroup_barrier(mem_flags::mem_device);
                for (int rk = 0; rk <= R; rk++) {
                    for (int j = 0; j < nc; j++) {
                        const int rank = j < nc0 ? cRank[c0 + j] : 0;
                        if (rank != rk) continue;
                        if (r == 0 && j == nc0 + 1 && m == 0) continue;
                        inf[vidx(j, nc0, c0, r, m, q, G, cGrp, cSh)] += dl[j];
                    }
                    threadgroup_barrier(mem_flags::mem_device);
                }
            }
            threadgroup_barrier(mem_flags::mem_device);
        }
        it++;
        if (m == 0) atomic_store_explicit(&bad, 0, memory_order_relaxed);
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (int r = 0; r < q; r++) {
            const int c0 = lStart[r], nc0 = lStart[r + 1] - c0, nc = nc0 + 2;
            int par = 0;
            for (int j = 0; j < nc; j++) par ^= inf[vidx(j, nc0, c0, r, m, q, G, cGrp, cSh)] < 0.f;
            if (par) { atomic_store_explicit(&bad, 1, memory_order_relaxed); break; }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        ok = atomic_load_explicit(&bad, memory_order_relaxed) == 0;
        threadgroup_barrier(mem_flags::mem_threadgroup); // everyone has read `bad` before the next round may reset it
        if (ok) break;
    }
    device uchar* hard = hardAll + (ulong)blk * (ulong)p.n;
    for (int i = m; i < G * Z; i += Z) hard[i] = inf[i] < 0.f;
    for (int r = 0; r < q; r++) hard[p.k + r + q * m] = inf[pb + r * (Z + 1) + 1 + m] < 0.f;
    if (m == 0) { info[blk * 2] = ok ? 1 : 0; info[blk * 2 + 1] = it; }
}
)MSL";


// Table-driven quasi-cyclic layered normalised min-sum (DTMB). One threadgroup of z threads per word, thread m owns check m of every block row.
// The same operations in the same order as the CPU decoder (dtmb_ldpc.cpp), so the results agree to the bit: totals live in threadgroup memory.
static NSString* const kQcSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct QP { int z, rows, cols, punct, sent, info, maxIter, gu0, gd0, gu1, gd1; float alpha, clip; uint msgPer; };
inline float spNeg(float t) { return max(0.f, max(max(0.6854f - 0.4378f * t, 0.5822f - 0.2689f * t), max(0.3653f - 0.1192f * t, 0.1323f - 0.0293f * t))); }

kernel void qc(constant QP& p [[buffer(0)]], device const int* rowStart [[buffer(1)]], device const int* ecol [[buffer(2)]], device const int* esh [[buffer(3)]],
               device float* msgAll [[buffer(4)]], device const float* llrAll [[buffer(5)]], device uchar* infoAll [[buffer(6)]], device int* res [[buffer(7)]],
               threadgroup float* lv [[threadgroup(0)]], uint m [[thread_position_in_threadgroup]], uint blk [[threadgroup_position_in_grid]]) {
    threadgroup atomic_int weight;
    const int z = p.z;
    const int mm = (int)m;
    device float* msg = msgAll + (ulong)blk * p.msgPer;
    device const float* llr = llrAll + (ulong)blk * (ulong)p.sent;
    for (int v = mm; v < p.cols * z; v += z) lv[v] = v < p.punct ? 0.f : max(-p.clip, min(p.clip, llr[v - p.punct]));
    for (uint i = m; i < p.msgPer; i += (uint)z) msg[i] = 0.f;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    int iters = 0;
    bool ok = false;
    // parity checks of the hard decisions: the number of unsatisfied checks of the whole word
    #define CHECKS(out) { \
        if (mm == 0) atomic_store_explicit(&weight, 0, memory_order_relaxed); \
        threadgroup_barrier(mem_flags::mem_threadgroup); \
        int bad_ = 0; \
        for (int r = 0; r < p.rows; r++) { \
            int par = 0; \
            for (int e = rowStart[r]; e < rowStart[r + 1]; e++) { int sh = mm + esh[e]; if (sh >= z) sh -= z; par ^= lv[ecol[e] * z + sh] < 0.f; } \
            bad_ += par; \
        } \
        atomic_fetch_add_explicit(&weight, bad_, memory_order_relaxed); \
        threadgroup_barrier(mem_flags::mem_threadgroup); \
        out = atomic_load_explicit(&weight, memory_order_relaxed); \
        threadgroup_barrier(mem_flags::mem_threadgroup); }
    int w0;
    CHECKS(w0)
    if (w0 == 0) { ok = true; iters = 1; }
    else {
        for (int iter = 1; iter <= p.maxIter; iter++) {
            for (int r = 0; r < p.rows; r++) {
                const int e0 = rowStart[r], e1 = rowStart[r + 1];
                float min1 = 0.f, min2 = 3.4028235e38f;
                uint sg = 0;
                for (int e = e0; e < e1; e++) {
                    int sh = mm + esh[e]; if (sh >= z) sh -= z;
                    const float q = lv[ecol[e] * z + sh] - msg[(ulong)e * z + m];
                    const float a = fabs(q);
                    if (e == e0) min1 = a;
                    else { min2 = min(min2, max(a, min1)); min1 = min(min1, a); }
                    sg ^= as_type<uint>(q) & 0x80000000u;
                }
                // a negative alpha: the two-minimum correction (dect2/dsp/minsum_corr.h) for the edges not holding the smallest input
                const float c1 = p.alpha < 0.f ? max(0.f, min1 + spNeg(min1 + min2) - spNeg(min2 - min1)) : min1;
                for (int e = e0; e < e1; e++) {
                    int sh = mm + esh[e]; if (sh >= z) sh -= z;
                    const int vi = ecol[e] * z + sh;
                    const float q = lv[vi] - msg[(ulong)e * z + m];
                    const float a = fabs(q);
                    const float mg = (a == min1 ? min2 : c1) * fabs(p.alpha);
                    const float v = as_type<float>(as_type<uint>(mg) | (sg ^ (as_type<uint>(q) & 0x80000000u)));
                    msg[(ulong)e * z + m] = v;
                    lv[vi] = max(-p.clip, min(p.clip, q + v));
                }
                threadgroup_barrier(mem_flags::mem_threadgroup);   // the next block row reads totals other threads have just written
            }
            int w;
            CHECKS(w)
            iters = iter;
            if (w == 0) { ok = true; break; }
            const int gu = iter == p.gu0 ? p.gd0 : iter == p.gu1 ? p.gd1 : 0;
            if (gu > 0 && w > p.rows * z / gu) break;
        }
    }
    device uchar* info = infoAll + (ulong)blk * (ulong)p.info;
    for (int v = mm; v < p.info; v += z) info[v] = lv[p.rows * z + v] < 0.f;
    if (mm == 0) { res[blk * 2] = ok ? 1 : 0; res[blk * 2 + 1] = iters; }
}
)MSL";

struct GpuLdpc::Impl {
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pso = nil;
    std::mutex mu;

    struct State {
        id<MTLBuffer> lStart, cGrp, cSh, mBase, cRank, layerR;
        bool ok = true;
        uint32_t msgPer = 0, postPer = 0;
        int cap = 0;
        id<MTLBuffer> post, msg, llr, hard, info;
    };
    std::map<const LdpcCode*, State> codes;

    static constexpr int kChunk = 128;

    // table-driven quasi-cyclic codes (decodeQc)
    id<MTLComputePipelineState> qcPso = nil;
    bool qcTried = false;
    struct QcState {
        id<MTLBuffer> rowStart, col, shift;
        uint32_t msgPer = 0;
        int cap = 0;
        id<MTLBuffer> msg, llr, info, res;
    };
    std::map<std::vector<int>, QcState> qcCodes;

    id<MTLBuffer> shared(size_t bytes) { return [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; }

    State& state(const LdpcCode& c) {
        auto it = codes.find(&c);
        if (it != codes.end()) return it->second;
        State s;
        const auto& L = c.layers();
        std::vector<int> ls(1, 0), cg, cs, rk, lr;
        std::vector<uint32_t> mb;
        uint32_t off = 0;
        for (auto& l : L) {
            int maxRank = 0;
            for (size_t i = 0; i < l.size(); i++) {
                int rank = 0;
                for (size_t k = 0; k < i; k++) if (l[k].group == l[i].group) rank++;
                maxRank = std::max(maxRank, rank);
                cg.push_back(l[i].group); cs.push_back(l[i].shift); rk.push_back(rank);
            }
            lr.push_back(maxRank);
            if (l.size() + 2 > 64) s.ok = false; // kernel keeps one increment per edge in a fixed array
            ls.push_back((int)cg.size());
            mb.push_back(off);
            off += (uint32_t)(l.size() + 2) * 360;
        }
        s.msgPer = off;
        s.postPer = (uint32_t)(c.groups() * 360 + c.q() * 361);
        auto mk = [&](const void* d, size_t n) { return [dev newBufferWithBytes:d length:std::max<size_t>(n, 4) options:MTLResourceStorageModeShared]; };
        s.lStart = mk(ls.data(), ls.size() * 4);
        s.cGrp = mk(cg.data(), cg.size() * 4);
        s.cSh = mk(cs.data(), cs.size() * 4);
        s.mBase = mk(mb.data(), mb.size() * 4);
        s.cRank = mk(rk.data(), rk.size() * 4);
        s.layerR = mk(lr.data(), lr.size() * 4);
        return codes.emplace(&c, std::move(s)).first->second;
    }
};

GpuLdpc::GpuLdpc() : p_(new Impl) {
    @autoreleasepool {
        Impl& d = *p_;
        d.dev = MTLCreateSystemDefaultDevice();
        if (!d.dev) return;
        NSError* err = nil;
        id<MTLLibrary> lib = [d.dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) { NSLog(@"GpuLdpc: shader compile failed: %@", err); d.dev = nil; return; }
        id<MTLFunction> fn = [lib newFunctionWithName:@"ldpc"];
        d.pso = [d.dev newComputePipelineStateWithFunction:fn error:&err];
        if (!d.pso) { NSLog(@"GpuLdpc: pipeline failed: %@", err); d.dev = nil; return; }
        d.queue = [d.dev newCommandQueue];
    }
}
GpuLdpc::~GpuLdpc() = default;

GpuLdpc& GpuLdpc::instance() { static GpuLdpc g; return g; }
bool GpuLdpc::available() const { return p_->pso != nil; }
bool GpuLdpc::boxCorrKernels() const { return true; }
const char* GpuLdpc::deviceName() const { return p_->dev ? p_->dev.name.UTF8String : "none"; }

bool GpuLdpc::decode(const LdpcCode& code, const float* llr, int nb, int maxIter, uint8_t* hard, uint8_t* ok, int* iters, float alpha, bool boxCorr) {
    if (!available() || code.q() * 360 != code.n() - code.k()) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    @autoreleasepool {
        Impl& d = *p_;
        Impl::State& s = d.state(code);
        if (!s.ok) return false;
        const int n = code.n();
        for (int b0 = 0; b0 < nb; b0 += Impl::kChunk) {
            const int cnt = std::min(Impl::kChunk, nb - b0);
            if (s.cap < cnt) {
                s.post = d.shared((size_t)cnt * s.postPer * 4);
                s.msg = d.shared((size_t)cnt * s.msgPer * 4);
                s.llr = d.shared((size_t)cnt * n * 4);
                s.hard = d.shared((size_t)cnt * n);
                s.info = d.shared((size_t)cnt * 8);
                s.cap = cnt;
            }
            memcpy(s.llr.contents, llr + (size_t)b0 * n, (size_t)cnt * n * 4);
            struct { int k, n, q, G, maxIter; float alpha; uint32_t msgPer, postPer; } P = {code.k(), n, code.q(), code.groups(), maxIter, boxCorr ? -alpha : alpha, s.msgPer, s.postPer};   // negative: the two-minimum correction
            id<MTLCommandBuffer> cb = [d.queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:d.pso];
            [enc setBytes:&P length:sizeof P atIndex:0];
            [enc setBuffer:s.lStart offset:0 atIndex:1];
            [enc setBuffer:s.cGrp offset:0 atIndex:2];
            [enc setBuffer:s.cSh offset:0 atIndex:3];
            [enc setBuffer:s.mBase offset:0 atIndex:4];
            [enc setBuffer:s.post offset:0 atIndex:5];
            [enc setBuffer:s.msg offset:0 atIndex:6];
            [enc setBuffer:s.llr offset:0 atIndex:7];
            [enc setBuffer:s.hard offset:0 atIndex:8];
            [enc setBuffer:s.info offset:0 atIndex:9];
            [enc setBuffer:s.cRank offset:0 atIndex:10];
            [enc setBuffer:s.layerR offset:0 atIndex:11];
            [enc dispatchThreadgroups:MTLSizeMake(cnt, 1, 1) threadsPerThreadgroup:MTLSizeMake(360, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) return false;
            memcpy(hard + (size_t)b0 * n, s.hard.contents, (size_t)cnt * n);
            const int* inf = (const int*)s.info.contents;
            for (int i = 0; i < cnt; i++) { ok[b0 + i] = (uint8_t)inf[i * 2]; iters[b0 + i] = inf[i * 2 + 1]; }
        }
    }
    return true;
}

bool GpuLdpc::qcAvailable() const { return available(); }

bool GpuLdpc::decodeQc(const QcLdpc& code, const float* llr, int nb, int maxIter, uint8_t* info, uint8_t* ok, int* iters, float alpha, bool boxCorr) {
    if (!available() || nb <= 0) return nb == 0;
    const int z = code.z, rows = code.blockRows, cols = code.blockCols;
    if (z <= 0 || z > 1024 || rows <= 0 || cols <= rows || (int)code.rowStart.size() != rows + 1 || code.col.size() != code.shift.size() || (int)code.col.size() != code.rowStart.back()) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    @autoreleasepool {
        Impl& d = *p_;
        if (!d.qcPso && !d.qcTried) {
            d.qcTried = true;
            NSError* err = nil;
            MTLCompileOptions* opt = [MTLCompileOptions new];
            opt.fastMathEnabled = NO;   // the CPU decoder's arithmetic, operation for operation
            id<MTLLibrary> lib = [d.dev newLibraryWithSource:kQcSource options:opt error:&err];
            if (!lib) { NSLog(@"GpuLdpc: QC shader compile failed: %@", err); return false; }
            id<MTLFunction> fn = [lib newFunctionWithName:@"qc"];
            d.qcPso = [d.dev newComputePipelineStateWithFunction:fn error:&err];
            if (!d.qcPso) { NSLog(@"GpuLdpc: QC pipeline failed: %@", err); return false; }
        }
        if (!d.qcPso) return false;
        const size_t tgMem = (size_t)cols * z * 4;
        if ((int)d.qcPso.maxTotalThreadsPerThreadgroup < z || tgMem > d.dev.maxThreadgroupMemoryLength) return false;
        // a block column twice in a block row would make two threads write one variable
        for (int r = 0; r < rows; r++)
            for (int a = code.rowStart[r]; a < code.rowStart[r + 1]; a++) {
                if (code.col[a] < 0 || code.col[a] >= cols || code.shift[a] < 0 || code.shift[a] >= z) return false;
                for (int b = a + 1; b < code.rowStart[r + 1]; b++) if (code.col[a] == code.col[b]) return false;
            }
        std::vector<int> key = {z, rows, cols, code.punctured};
        key.insert(key.end(), code.rowStart.begin(), code.rowStart.end());
        key.insert(key.end(), code.col.begin(), code.col.end());
        key.insert(key.end(), code.shift.begin(), code.shift.end());
        auto it = d.qcCodes.find(key);
        if (it == d.qcCodes.end()) {
            Impl::QcState s;
            auto mk = [&](const std::vector<int>& v) { return [d.dev newBufferWithBytes:v.data() length:std::max<size_t>(v.size() * 4, 4) options:MTLResourceStorageModeShared]; };
            s.rowStart = mk(code.rowStart); s.col = mk(code.col); s.shift = mk(code.shift);
            s.msgPer = (uint32_t)code.col.size() * (uint32_t)z;
            it = d.qcCodes.emplace(key, std::move(s)).first;
        }
        Impl::QcState& s = it->second;
        const int sent = code.sentBits(), infoN = code.infoBits();
        for (int b0 = 0; b0 < nb; b0 += Impl::kChunk) {
            const int cnt = std::min(Impl::kChunk, nb - b0);
            if (s.cap < cnt) {
                s.msg = d.shared((size_t)cnt * s.msgPer * 4);
                s.llr = d.shared((size_t)cnt * sent * 4);
                s.info = d.shared((size_t)cnt * infoN);
                s.res = d.shared((size_t)cnt * 8);
                s.cap = cnt;
            }
            memcpy(s.llr.contents, llr + (size_t)b0 * sent, (size_t)cnt * sent * 4);
            struct { int z, rows, cols, punct, sent, info, maxIter, gu0, gd0, gu1, gd1; float alpha, clip; uint32_t msgPer; } P = {
                z, rows, cols, code.punctured, sent, infoN, maxIter, code.giveUpIter[0], code.giveUpDiv[0], code.giveUpIter[1], code.giveUpDiv[1], boxCorr ? -alpha : alpha, code.clip, s.msgPer};
            id<MTLCommandBuffer> cb = [d.queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:d.qcPso];
            [enc setBytes:&P length:sizeof P atIndex:0];
            [enc setBuffer:s.rowStart offset:0 atIndex:1];
            [enc setBuffer:s.col offset:0 atIndex:2];
            [enc setBuffer:s.shift offset:0 atIndex:3];
            [enc setBuffer:s.msg offset:0 atIndex:4];
            [enc setBuffer:s.llr offset:0 atIndex:5];
            [enc setBuffer:s.info offset:0 atIndex:6];
            [enc setBuffer:s.res offset:0 atIndex:7];
            [enc setThreadgroupMemoryLength:(tgMem + 15) & ~(size_t)15 atIndex:0];
            [enc dispatchThreadgroups:MTLSizeMake(cnt, 1, 1) threadsPerThreadgroup:MTLSizeMake(z, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) return false;
            memcpy(info + (size_t)b0 * infoN, s.info.contents, (size_t)cnt * infoN);
            const int* rs = (const int*)s.res.contents;
            for (int i = 0; i < cnt; i++) { ok[b0 + i] = (uint8_t)rs[i * 2]; iters[b0 + i] = rs[i * 2 + 1]; }
        }
    }
    return true;
}

} // namespace dect2
