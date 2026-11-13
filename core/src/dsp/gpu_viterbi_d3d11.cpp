// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Direct3D 11 compute Viterbi decoder for Windows (GpuViterbi, see dect2/dsp/gpu_viterbi.h): the same chunks, trellis and arithmetic as the
// Metal kernel (gpu_viterbi.mm) and the exact CPU decoder: 16-bit saturating path metrics brought back by the best one every 4 steps, the odd
// predecessor taken when it is strictly better, trace-back from the best end state. Shader model 5.0 has no wave ballots, so one thread group of
// 64 threads (one per state) keeps the metrics in group-shared memory and builds each step's 64 decision bits with InterlockedOr: one barrier per
// step (two on the steps that renormalise). The HLSL is compiled when the program starts (d3dcompiler_47.dll), the libraries are loaded at run
// time, the software rasteriser is refused, and a self-test against the CPU decoder must give the same bits and path metric or the GPU decoder
// stays off (the CPU decoder is used). EXPERIMENTAL: off unless DECT2_GPU_VITERBI_D3D11=1 (not yet run on a Windows GPU). DECT2_NOGPU=1 keeps it off.
#include "dect2/dsp/gpu_viterbi.h"
#include "dect2/common/diag.h"
#include "dect2/dvbt/dvbt.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace dect2 {

namespace {

const char* const kHlsl = R"HLSL(
cbuffer P : register(b0) { uint steps; uint C; uint L; uint decStride; uint chunkBase; uint pad0; uint pad1; uint pad2; };
ByteAddressBuffer soft : register(t0);            // the soft bytes, two per step
RWStructuredBuffer<uint> dec : register(u0);      // per chunk: 2 words per step (the decisions of states 0..31 and 32..63)
RWStructuredBuffer<uint> bitsOut : register(u1);  // one word per step: the decided bit
RWStructuredBuffer<int> metrics : register(u2);   // per chunk: the path metric
groupshared int pm[2][64];
groupshared uint dw[3][2];
groupshared int topv[3];
groupshared uint tb[128];

int softAt(uint i) {   // the signed byte i of the soft values
    const uint w = soft.Load(i & ~3u);
    const int b = (int)((w >> ((i & 3u) * 8u)) & 0xFFu);
    return b >= 128 ? b - 256 : b;
}
int s16(int x) { return (x << 16) >> 16; }   // the value as a 16-bit integer (wraps, like the Metal kernel's (short) cast)

[numthreads(64, 1, 1)]
void viterbi(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    const uint ci = gid.x;
    const uint a = (ci + chunkBase) * C, b = min(steps, a + C);
    const uint lo = a > L ? a - L : 0u, hi = min(steps, b + L);
    const uint len = hi - lo;
    const uint dbase = ci * decStride;
    const uint j = tid & 31u, u = tid >> 5;
    const int sx = (countbits((2u * j) & 0x79u) & 1u) ? -1 : 1;
    const int sy = (countbits((2u * j) & 0x5Bu) & 1u) ? -1 : 1;
    pm[0][tid] = 0;
    if (tid < 3) { dw[tid][0] = 0; dw[tid][1] = 0; topv[tid] = -32768; }
    GroupMemoryBarrierWithGroupSync();
    uint cur = 0;
    int off = 0;
    for (uint t = 0; t < len; t++) {
        const uint k = t % 3u;
        if (tid == 0) { const uint kn = (t + 1u) % 3u; dw[kn][0] = 0; dw[kn][1] = 0; topv[kn] = -32768; }   // the slot step t + 1 uses (last read at t - 2)
        const int sa = softAt(2u * (lo + t)), sb = softAt(2u * (lo + t) + 1u);
        const int M = sx * sa + sy * sb;
        const int A = pm[cur][2u * j], B = pm[cur][2u * j + 1u];
        int nv; uint d;
        if (u == 0) { const int p0 = clamp(A + M, -32768, 32767), p1 = clamp(B - M, -32768, 32767); nv = max(p0, p1); d = p1 > p0 ? 1u : 0u; }
        else { const int r0 = clamp(A - M, -32768, 32767), r1 = clamp(B + M, -32768, 32767); nv = max(r0, r1); d = r1 > r0 ? 1u : 0u; }
        uint dummy;
        InterlockedOr(dw[k][u], d << j, dummy);
        const bool ren = (t & 3u) == 3u;
        if (ren) { int old; InterlockedMax(topv[k], nv, old); }
        pm[cur ^ 1u][tid] = nv;
        GroupMemoryBarrierWithGroupSync();
        if (tid == 0) { dec[dbase + 2u * t] = dw[k][0]; dec[dbase + 2u * t + 1u] = dw[k][1]; }
        if (ren) {
            const int top = topv[k];
            off += top;
            pm[cur ^ 1u][tid] = s16(nv - top);
            GroupMemoryBarrierWithGroupSync();
        }
        cur ^= 1u;
    }
    // best end state (the first of the best), the metric, then the trace-back in tiles of 64 steps loaded by all threads
    GroupMemoryBarrierWithGroupSync();
    int state = 0;
    if (tid == 0) {
        int bm = pm[cur][0];
        for (int s = 1; s < 64; s++) if (pm[cur][s] > bm) { bm = pm[cur][s]; state = s; }
        metrics[ci] = off + bm;
    }
    DeviceMemoryBarrierWithGroupSync();
    for (int base = len ? (int)((len - 1u) / 64u) * 64 : 0; len != 0u && base >= 0; base -= 64) {
        const uint tt = (uint)base + tid;
        tb[2u * tid] = tt < len ? dec[dbase + 2u * tt] : 0u;
        tb[2u * tid + 1u] = tt < len ? dec[dbase + 2u * tt + 1u] : 0u;
        GroupMemoryBarrierWithGroupSync();
        if (tid == 0) {
            for (int q = (int)min(63u, len - 1u - (uint)base); q >= 0; q--) {
                const uint absPos = lo + (uint)base + (uint)q;
                if (absPos >= a && absPos < b) bitsOut[absPos] = (uint)(state >> 5);
                const uint w = state < 32 ? tb[2 * q] : tb[2 * q + 1];
                state = ((state & 31) << 1) | (int)((w >> (uint)(state & 31)) & 1u);
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
)HLSL";

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct CBuf { uint32_t steps, C, L, decStride, chunkBase, pad0, pad1, pad2; };
static_assert(sizeof(CBuf) % 16 == 0, "constant buffer size must be a multiple of 16 bytes");

}   // namespace

struct GpuViterbi::Impl {
    HMODULE hD3d = nullptr, hComp = nullptr;
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    Com<ID3D11ComputeShader> cs;
    Com<ID3D11Buffer> cbuf;
    std::mutex mu;
    std::atomic<bool> ready{false};
    static constexpr size_t kC = 16384, kL = 192, kMaxChunks = 256;
    // buffers, grown as needed
    size_t softCap = 0, chunkCap = 0;
    Com<ID3D11Buffer> soft, bits, bitsStage, metrics, metricsStage, dec;
    Com<ID3D11ShaderResourceView> vSoft;
    Com<ID3D11UnorderedAccessView> uBits, uMetrics, uDec;

    bool structured(Com<ID3D11Buffer>& b, Com<ID3D11UnorderedAccessView>& v, UINT count) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = std::max<UINT>(count, 1) * 4;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.StructureByteStride = 4;
        if (FAILED(dev->CreateBuffer(&d, nullptr, b.put()))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_UNKNOWN;
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.FirstElement = 0;
        u.Buffer.NumElements = std::max<UINT>(count, 1);
        return SUCCEEDED(dev->CreateUnorderedAccessView(b.p, &u, v.put()));
    }
    bool staging(Com<ID3D11Buffer>& b, UINT bytes) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = std::max<UINT>(bytes, 4);
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev->CreateBuffer(&d, nullptr, b.put()));
    }
    bool raw(Com<ID3D11Buffer>& b, Com<ID3D11ShaderResourceView>& v, UINT bytes) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = (std::max<UINT>(bytes, 4) + 3) & ~3u;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(dev->CreateBuffer(&d, nullptr, b.put()))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx.FirstElement = 0;
        sv.BufferEx.NumElements = d.ByteWidth / 4;
        sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        return SUCCEEDED(dev->CreateShaderResourceView(b.p, &sv, v.put()));
    }
    bool ensure(size_t steps, size_t chunks) {
        if (softCap < steps) {
            const size_t cap = steps + steps / 4;
            if (!raw(soft, vSoft, (UINT)(cap * 2)) || !structured(bits, uBits, (UINT)cap) || !staging(bitsStage, (UINT)(cap * 4))) { softCap = 0; return false; }
            softCap = cap;
        }
        if (chunkCap < chunks) {
            const size_t cap = std::min(Impl::kMaxChunks, chunks);
            if (!structured(metrics, uMetrics, (UINT)cap) || !staging(metricsStage, (UINT)(cap * 4)) || !structured(dec, uDec, (UINT)(cap * 2 * (kC + 2 * kL)))) { chunkCap = 0; return false; }
            chunkCap = cap;
        }
        return true;
    }
    void lost(const char* where) {
        const HRESULT r = dev ? dev->GetDeviceRemovedReason() : S_OK;
        if (r != S_OK && ready.exchange(false)) fprintf(stderr, "GpuViterbi: the device was lost (%s, 0x%08lx), using the CPU decoder from now on\n", where, (unsigned long)r); DIAG(115213, "The Direct3D 11 device was lost during %s with reason 0x%08lx, so the CPU Viterbi decoder is used from now on.", where, (unsigned long)r);
    }
};

GpuViterbi::GpuViterbi() : p_(new Impl) {
    Impl& d = *p_;
    if (getenv("DECT2_NOGPU")) return;
    if (!getenv("DECT2_GPU_VITERBI_D3D11")) return;   // experimental: not yet run on a Windows GPU, off unless asked for
    d.hD3d = LoadLibraryA("d3d11.dll");
    d.hComp = LoadLibraryA("d3dcompiler_47.dll");
    if (!d.hD3d || !d.hComp) return;
    auto create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d.hD3d, "D3D11CreateDevice");
    auto compile = (pD3DCompile)GetProcAddress(d.hComp, "D3DCompile");
    if (!create || !compile) return;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    // the adapter with the most own video memory (on two-GPU laptops the default one is often the weak integrated chip), never the software one
    Com<IDXGIAdapter> best;
    if (HMODULE hDxgi = LoadLibraryA("dxgi.dll")) {
        typedef HRESULT(WINAPI * PFN_FACTORY)(REFIID, void**);
        if (auto mk = (PFN_FACTORY)GetProcAddress(hDxgi, "CreateDXGIFactory1")) {
            Com<IDXGIFactory1> fac;
            if (SUCCEEDED(mk(__uuidof(IDXGIFactory1), (void**)fac.put()))) {
                SIZE_T bestMem = 0;
                for (UINT i = 0;; i++) {
                    Com<IDXGIAdapter> ad;
                    if (fac->EnumAdapters(i, ad.put()) != S_OK) break;
                    DXGI_ADAPTER_DESC ds{};
                    if (FAILED(ad->GetDesc(&ds)) || ds.VendorId == 0x1414) continue;
                    if (!best || ds.DedicatedVideoMemory > bestMem) { best.reset(); best.p = ad.p; ad.p = nullptr; bestMem = ds.DedicatedVideoMemory; }
                }
            }
        }
    }
    if (FAILED(create(best.p, best ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, d.dev.put(), &got, d.ctx.put()))) return;
    if (got < D3D_FEATURE_LEVEL_11_0) { DIAG_DEBUG(895245, "The Direct3D device only supports feature level 0x%x, below 11.0, so the CPU Viterbi decoder is used.", (unsigned)got); d.dev.reset(); d.ctx.reset(); return; }
    Com<ID3DBlob> code, err;
    if (FAILED(compile(kHlsl, strlen(kHlsl), "viterbi.hlsl", nullptr, nullptr, "viterbi", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), err.put())) || !code) {
        fprintf(stderr, "GpuViterbi: shader compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?"); DIAG(643827, "The Direct3D 11 Viterbi shader did not compile (%s), so the CPU Viterbi decoder is used.", err ? (const char*)err->GetBufferPointer() : "no message");
        d.dev.reset(); d.ctx.reset();
        return;
    }
    if (FAILED(d.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, d.cs.put()))) { d.dev.reset(); d.ctx.reset(); return; }
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = sizeof(CBuf);
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(d.dev->CreateBuffer(&cd, nullptr, d.cbuf.put()))) { d.dev.reset(); d.ctx.reset(); return; }
    d.ready = true;
    // self-test: noisy, punctured and full-scale soft values; the GPU must give the CPU decoder's bits and path metric exactly
    std::mt19937 rng(41);
    std::vector<int8_t> s(2 * 70000);
    for (auto& v : s) { const int r = (int)(rng() % 255) - 127; v = (int8_t)((rng() % 5) ? r / 3 : r); }
    for (size_t i = 3; i < s.size(); i += 4) s[i] = 0;
    std::vector<uint8_t> ga, ca;
    long gm = 0, cm = 0;
    dvbt::Viterbi::decode(s, ca, 1, &cm, false);
    if (!decode(s, ga, &gm) || ga != ca || gm != cm) { d.ready = false; fprintf(stderr, "GpuViterbi: self-test failed, using the CPU decoder\n"); DIAG(794526, "The Direct3D 11 Viterbi self-test gave a wrong result, so the CPU Viterbi decoder is used."); }
}
GpuViterbi::~GpuViterbi() = default;
GpuViterbi& GpuViterbi::instance() { static GpuViterbi g; return g; }
bool GpuViterbi::available() const { return p_->ready.load(); }

bool GpuViterbi::decode(const std::vector<int8_t>& softIn, std::vector<uint8_t>& bits, long* metric) {
    if (!available()) return false;
    const size_t steps = softIn.size() / 2;
    bits.assign(steps, 0);
    if (metric) *metric = 0;
    if (!steps) return true;
    Impl& d = *p_;
    std::lock_guard<std::mutex> lk(d.mu);
    const size_t C = Impl::kC, L = Impl::kL;
    const size_t chunks = (steps + C - 1) / C;
    if (!d.ensure(steps, chunks)) return false;
    {
        D3D11_BOX box{0, 0, 0, (UINT)((steps * 2 + 3) & ~(size_t)3), 1, 1};
        std::vector<int8_t> padded;   // (the box is whole words: the last one is padded)
        const void* src = softIn.data();
        if (box.right != steps * 2) { padded.assign(softIn.begin(), softIn.begin() + (ptrdiff_t)(steps * 2)); padded.resize(box.right, 0); src = padded.data(); }
        d.ctx->UpdateSubresource(d.soft.p, 0, &box, src, 0, 0);
    }
    long total = 0;
    for (size_t c0 = 0; c0 < chunks; c0 += Impl::kMaxChunks) {
        const size_t cnt = std::min(Impl::kMaxChunks, chunks - c0);
        const CBuf P = {(uint32_t)steps, (uint32_t)C, (uint32_t)L, (uint32_t)(2 * (C + 2 * L)), (uint32_t)c0, 0, 0, 0};
        d.ctx->UpdateSubresource(d.cbuf.p, 0, nullptr, &P, 0, 0);
        ID3D11UnorderedAccessView* uavs[3] = {d.uDec.p, d.uBits.p, d.uMetrics.p};
        d.ctx->CSSetShader(d.cs.p, nullptr, 0);
        d.ctx->CSSetShaderResources(0, 1, &d.vSoft.p);
        d.ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        d.ctx->CSSetConstantBuffers(0, 1, &d.cbuf.p);
        d.ctx->Dispatch((UINT)cnt, 1, 1);
        ID3D11UnorderedAccessView* nullU[3] = {};
        ID3D11ShaderResourceView* nullS[1] = {};
        d.ctx->CSSetUnorderedAccessViews(0, 3, nullU, nullptr);
        d.ctx->CSSetShaderResources(0, 1, nullS);
        d.ctx->CopyResource(d.metricsStage.p, d.metrics.p);
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (FAILED(d.ctx->Map(d.metricsStage.p, 0, D3D11_MAP_READ, 0, &mp))) { d.lost("metrics"); return false; }
        const int* mt = (const int*)mp.pData;
        for (size_t i = 0; i < cnt; i++) total += mt[i];
        d.ctx->Unmap(d.metricsStage.p, 0);
    }
    d.ctx->CopyResource(d.bitsStage.p, d.bits.p);
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(d.ctx->Map(d.bitsStage.p, 0, D3D11_MAP_READ, 0, &mp))) { d.lost("bits"); return false; }
    const uint32_t* bw = (const uint32_t*)mp.pData;
    for (size_t i = 0; i < steps; i++) bits[i] = (uint8_t)bw[i];
    d.ctx->Unmap(d.bitsStage.p, 0);
    if (metric) *metric = total;
    return true;
}

}   // namespace dect2
