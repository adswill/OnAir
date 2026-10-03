// Direct3D 11 compute LDPC decoder for Windows: the same layered normalised min-sum as the Metal version (one thread group of 360 threads
// per FEC block), written in HLSL and compiled when the program starts. d3d11.dll and d3dcompiler_47.dll are loaded at run time, so a
// computer without a usable GPU or driver simply reports the decoder as unavailable and the CPU decoder is used.
//
// HLSL's shader compiler only accepts thread-group barriers in code that every thread of the group reaches together, so unlike the Metal
// kernel this one never leaves its loops early. It runs a few iterations per dispatch (a block that has converged does no more work),
// the CPU then looks at which blocks are still unfinished and launches only those again.
#include "dect2/gpu_ldpc.h"
#include "dect2/t2fec.h"

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
#include <cstdio>
#include <cstring>
#include <map>
#include <cmath>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace dect2 {

namespace {

const char* const kHlsl = R"HLSL(
static const int Z = 360;

cbuffer P : register(b0) {
    uint k, n, q, G;
    uint chunk;       // iterations in this dispatch
    float alpha;
    uint msgPer, postPer;
    uint first;       // 1: initialise posteriors and messages before iterating
    uint nWords;      // unused
    uint iterBase;    // iterations done by earlier dispatches
    uint pad;
    uint4 layerR[64]; // layerR[r / 4][r % 4]: highest repeat rank of layer r
};

Buffer<int> lStart : register(t0);
Buffer<int> cGrp : register(t1);
Buffer<int> cSh : register(t2);
Buffer<uint> mBase : register(t3);
Buffer<int> cRank : register(t4);
Buffer<float> llrAll : register(t5);
Buffer<uint> activeList : register(t6);
Buffer<int> lCount : register(t7);   // information edges per layer (not derived from lStart: one compiler folded the difference to 0)

// typed 32-bit buffers: reading and writing them is supported by every Direct3D 11 GPU and every shader compiler
RWBuffer<float> postAll : register(u0);
RWBuffer<uint> msgAll : register(u1);
RWBuffer<uint> infoB : register(u2);
RWBuffer<uint> hardAll : register(u3);
float LP(uint i) { return postAll[i]; }
void SP(uint i, float v) { postAll[i] = v; }
float LM(uint i) { return asfloat(msgAll[i]); }
void SM(uint i, float v) { msgAll[i] = asuint(v); }

groupshared uint bad;

uint vidx(int j, int nc0, int c0, int r, int m) {
    uint res;
    int pb = (int)G * Z;
    if (j < nc0) {
        int nn = m - cSh[c0 + j];
        if (nn < 0) nn += Z;
        res = (uint)(cGrp[c0 + j] * Z + nn);
    } else if (j == nc0) {
        res = (uint)(pb + r * (Z + 1) + 1 + m);
    } else if (r > 0) {
        res = (uint)(pb + (r - 1) * (Z + 1) + 1 + m);
    } else {
        res = (uint)(pb + ((int)q - 1) * (Z + 1) + m);
    }
    return res;
}

[numthreads(360, 1, 1)]
void init(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    int m = (int)gtid.x;
    uint blk = activeList[gid.x];
    uint pBase = blk * postPer, mB = blk * msgPer, lBase = blk * n;
    int qi = (int)q, Gi = (int)G, pb = Gi * Z;
    for (int i = m; i < Gi * Z; i += Z) SP(pBase + i, llrAll[lBase + i]);
    for (int r = 0; r < qi; r++) {
        SP(pBase + pb + r * (Z + 1) + 1 + m, llrAll[lBase + k + r + qi * m]);
        if (m == 0) SP(pBase + pb + r * (Z + 1), 1e4f);
    }
    for (uint i = (uint)m; i < msgPer; i += Z) SM(mB + i, 0.0f);
}

[numthreads(360, 1, 1)]
void ldpc(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    int m = (int)gtid.x;
    uint blk = activeList[gid.x];
    uint pBase = blk * postPer, mB = blk * msgPer, lBase = blk * n;
    int qi = (int)q, Gi = (int)G, pb = Gi * Z;

    bool active = true;
    uint firstOk = 0;
    for (uint c = 0; c < chunk; c++) {
        for (int r = 0; r < qi; r++) {
            int c0 = lStart[r];
            int nc0 = lCount[r];
            int nc = nc0 + 2;
            uint mr = mB + mBase[r];
            uint R = layerR[r >> 2][r & 3];
            float dl[64];   // pending posterior updates of this thread's edges (kept in registers)
            if (active) {
                float min1 = 1e30f, min2 = 1e30f;
                int idx1 = -1;
                uint sg = 0;
                for (int j = 0; j < nc; j++) {
                    float x = LP(pBase + vidx(j, nc0, c0, r, m)) - LM(mr + j * Z + m);
                    float a = abs(x);
                    sg ^= asuint(x) & 0x80000000u;
                    if (a < min1) { min2 = min1; min1 = a; idx1 = j; }
                    else if (a < min2) min2 = a;
                }
                // A layer may reach the same variable group twice (a variable then has two edges in the layer, owned by different
                // threads): the new messages are computed from the posteriors as they were at the start of the layer and the
                // increments applied afterwards, unique edges at once and repeated ones one rank at a time.
                for (int j = 0; j < nc; j++) {
                    uint vi = pBase + vidx(j, nc0, c0, r, m);
                    float oldMsg = LM(mr + j * Z + m);   // read once: the old message is needed again for the update below
                    float x = LP(vi) - oldMsg;
                    float mag = (idx1 == j ? min2 : min1) * alpha;
                    float nm = asfloat(asuint(mag) | (sg ^ (asuint(x) & 0x80000000u)));
                    float d = nm - oldMsg;
                    SM(mr + j * Z + m, nm);
                    dl[j] = d;   // pending update: applied below, after every thread has read the old posteriors
                }
            }
            // every barrier here is unconditional and at the top level of the loop (the same for all threads)
            AllMemoryBarrierWithGroupSync();
            for (uint rk = 0; rk <= R; rk++) {
                if (active) {
                    for (int j = 0; j < nc; j++) {
                        int rank = 0;
                        if (j < nc0) rank = cRank[c0 + j];
                        bool pad = (r == 0 && j == nc0 + 1 && m == 0);   // the pad variable stays known
                        if ((uint)rank == rk && !pad) {
                            uint vv = pBase + vidx(j, nc0, c0, r, m);
                            SP(vv, LP(vv) + dl[j]);
                        }
                    }
                }
                AllMemoryBarrierWithGroupSync();
            }
        }
        // parity check of all layers
        if (m == 0) bad = 0;
        GroupMemoryBarrierWithGroupSync();
        if (active) {
            for (int r = 0; r < qi; r++) {
                int c0 = lStart[r];
            int nc0 = lCount[r];
            int nc = nc0 + 2;
                int par = 0;
                for (int j = 0; j < nc; j++) par ^= LP(pBase + vidx(j, nc0, c0, r, m)) < 0.0f ? 1 : 0;
                if (par != 0) { uint old; InterlockedOr(bad, 1u, old); break; }
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (active && bad == 0) { active = false; firstOk = c + 1; }
        GroupMemoryBarrierWithGroupSync(); // everyone has read `bad` before the next round resets it
    }
    if (m == 0) {
        infoB[blk * 2] = firstOk != 0 ? 1u : 0u;
        infoB[blk * 2 + 1] = iterBase + (firstOk != 0 ? firstOk : chunk);
    }
}

[numthreads(360, 1, 1)]
void finish(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    int m = (int)gtid.x;
    uint blk = activeList[gid.x];
    uint pBase = blk * postPer, hB = blk * n;
    int qi = (int)q, Gi = (int)G, pb = Gi * Z;
    for (int i = m; i < Gi * Z; i += Z) hardAll[hB + i] = LP(pBase + i) < 0.0f ? 1u : 0u;
    for (int r = 0; r < qi; r++) hardAll[hB + k + r + qi * m] = LP(pBase + pb + r * (Z + 1) + 1 + m) < 0.0f ? 1u : 0u;
}
)HLSL";

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p(o.p) { o.p = nullptr; }
    Com& operator=(Com&& o) noexcept { if (this != &o) { reset(); p = o.p; o.p = nullptr; } return *this; }
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

constexpr int kChunkBlocks = 64;   // blocks per batch (keeps the message buffer at a size every GPU accepts)
constexpr int kIterPerDispatch = 5;

struct CBuf {
    uint32_t k, n, q, G;
    uint32_t chunk;
    float alpha;
    uint32_t msgPer, postPer;
    uint32_t first, nWords, iterBase, pad;
    uint32_t layerR[256];
};
static_assert(sizeof(CBuf) % 16 == 0, "constant buffer size must be a multiple of 16 bytes");

} // namespace

struct GpuLdpc::Impl {
    HMODULE hD3d = nullptr, hComp = nullptr;
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    Com<ID3D11ComputeShader> csLdpc, csFinish, csInit;
    Com<ID3D11Buffer> cbuf;
    std::string name = "none";
    std::mutex mu;
    bool ready = false;

    struct Gpu {   // buffers for one code, sized for `cap` blocks
        bool ok = true;
        uint32_t msgPer = 0, postPer = 0, nWords = 0;
        int rmax = 0;
        std::vector<uint32_t> layerR;
        Com<ID3D11Buffer> lStart, cGrp, cSh, mBase, cRank, lCount;
        Com<ID3D11ShaderResourceView> vLStart, vCGrp, vCSh, vMBase, vCRank, vLCount;
        int cap = 0;
        Com<ID3D11Buffer> post, msg, llr, info, hard, active, infoStage, hardStage;
        Com<ID3D11UnorderedAccessView> uPost, uMsg, uInfo, uHard;
        Com<ID3D11ShaderResourceView> vLlr, vActive;
    };
    std::map<const LdpcCode*, Gpu> codes;

    // read-only typed 32-bit buffer (int, uint or float) with a shader-resource view
    bool typedSrv(Com<ID3D11Buffer>& b, Com<ID3D11ShaderResourceView>& v, UINT count, DXGI_FORMAT fmt, const void* init = nullptr) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = std::max<UINT>(count, 1) * 4;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateBuffer(&d, nullptr, b.put()))) return false;
        if (init) ctx->UpdateSubresource(b.p, 0, nullptr, init, 0, 0);   // (filled after creation: some Direct3D layers ignore initial data)
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = fmt;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.FirstElement = 0;
        sv.Buffer.NumElements = std::max<UINT>(count, 1);
        return SUCCEEDED(dev->CreateShaderResourceView(b.p, &sv, v.put()));
    }
    // typed 32-bit buffer (float or uint) with an unordered-access view
    bool typedUav(Com<ID3D11Buffer>& b, Com<ID3D11UnorderedAccessView>& v, UINT words, DXGI_FORMAT fmt) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = std::max<UINT>(words, 1) * 4;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev->CreateBuffer(&d, nullptr, b.put()))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = fmt;
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.FirstElement = 0;
        u.Buffer.NumElements = std::max<UINT>(words, 1);
        return SUCCEEDED(dev->CreateUnorderedAccessView(b.p, &u, v.put()));
    }
    bool uav(Com<ID3D11UnorderedAccessView>& v, ID3D11Buffer* b, UINT count) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d.Buffer.FirstElement = 0;
        d.Buffer.NumElements = std::max<UINT>(count, 1);
        return SUCCEEDED(dev->CreateUnorderedAccessView(b, &d, v.put()));
    }
    // writes `bytes` bytes at the start of a buffer (a whole-resource update would read past the end of a smaller source)
    void write(ID3D11Buffer* b, const void* data, UINT bytes) {
        D3D11_BOX box{0, 0, 0, bytes, 1, 1};
        ctx->UpdateSubresource(b, 0, &box, data, 0, 0);
    }
    bool staging(Com<ID3D11Buffer>& b, UINT bytes) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = std::max<UINT>(bytes, 4);
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev->CreateBuffer(&d, nullptr, b.put()));
    }

    Gpu* state(const LdpcCode& c) {
        auto it = codes.find(&c);
        if (it != codes.end()) return &it->second;
        Gpu s;
        const auto& L = c.layers();
        std::vector<int> ls(1, 0), cg, cs, rk, lc;
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
            s.layerR.push_back((uint32_t)maxRank);
            if (l.size() + 2 > 64) s.ok = false;   // the kernel keeps one increment per edge in a fixed array
            ls.push_back((int)cg.size());
            lc.push_back((int)l.size());
            mb.push_back(off);
            off += (uint32_t)(l.size() + 2) * 360;
        }
        if (L.size() > 256) s.ok = false;           // layer table size in the constant buffer
        s.msgPer = off;
        s.postPer = (uint32_t)(c.groups() * 360 + c.q() * 361);
        s.nWords = (uint32_t)((c.n() + 31) / 32);
        bool good = s.ok;
        good = good && typedSrv(s.lStart, s.vLStart, (UINT)ls.size(), DXGI_FORMAT_R32_SINT, ls.data());
        good = good && typedSrv(s.cGrp, s.vCGrp, (UINT)cg.size(), DXGI_FORMAT_R32_SINT, cg.data());
        good = good && typedSrv(s.cSh, s.vCSh, (UINT)cs.size(), DXGI_FORMAT_R32_SINT, cs.data());
        good = good && typedSrv(s.mBase, s.vMBase, (UINT)mb.size(), DXGI_FORMAT_R32_UINT, mb.data());
        good = good && typedSrv(s.cRank, s.vCRank, (UINT)rk.size(), DXGI_FORMAT_R32_SINT, rk.data());
        good = good && typedSrv(s.lCount, s.vLCount, (UINT)lc.size(), DXGI_FORMAT_R32_SINT, lc.data());
        s.ok = good;
        return &codes.emplace(&c, std::move(s)).first->second;
    }

    bool ensure(Gpu& s, int cnt, int n) {
        if (s.cap >= cnt) return true;
        const UINT nb = (UINT)cnt;
        bool good = typedUav(s.post, s.uPost, nb * s.postPer, DXGI_FORMAT_R32_FLOAT);
        good = good && typedUav(s.msg, s.uMsg, nb * s.msgPer, DXGI_FORMAT_R32_UINT);
        good = good && typedSrv(s.llr, s.vLlr, nb * (UINT)n, DXGI_FORMAT_R32_FLOAT);
        good = good && typedUav(s.info, s.uInfo, nb * 2, DXGI_FORMAT_R32_UINT) && staging(s.infoStage, nb * 8);
        good = good && typedUav(s.hard, s.uHard, nb * (UINT)n, DXGI_FORMAT_R32_UINT) && staging(s.hardStage, nb * (UINT)n * 4);
        good = good && typedSrv(s.active, s.vActive, nb, DXGI_FORMAT_R32_UINT);
        if (good) s.cap = cnt; else s.cap = 0;
        return good;
    }

    void bind(Gpu& s, ID3D11ComputeShader* cs) {
        ID3D11ShaderResourceView* srvs[8] = {s.vLStart.p, s.vCGrp.p, s.vCSh.p, s.vMBase.p, s.vCRank.p, s.vLlr.p, s.vActive.p, s.vLCount.p};
        ID3D11UnorderedAccessView* uavs[4] = {s.uPost.p, s.uMsg.p, s.uInfo.p, s.uHard.p};
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 8, srvs);
        ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &cbuf.p);
    }
    void unbind() {
        ID3D11ShaderResourceView* nullS[8] = {};
        ID3D11UnorderedAccessView* nullU[4] = {};
        ctx->CSSetShaderResources(0, 8, nullS);
        ctx->CSSetUnorderedAccessViews(0, 4, nullU, nullptr);
    }
};

GpuLdpc::GpuLdpc() : p_(new Impl) {
    Impl& d = *p_;
    if (getenv("DECT2_NOGPU")) return;
    d.hD3d = LoadLibraryA("d3d11.dll");
    d.hComp = LoadLibraryA("d3dcompiler_47.dll");
    if (!d.hD3d || !d.hComp) return;
    auto create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d.hD3d, "D3D11CreateDevice");
    auto compile = (pD3DCompile)GetProcAddress(d.hComp, "D3DCompile");
    if (!create || !compile) return;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    // On laptops with two graphics chips the default adapter is often the weak integrated one: take the one with the most own video memory.
    Com<IDXGIAdapter> best;
    if (HMODULE hDxgi = LoadLibraryA("dxgi.dll")) {
        typedef HRESULT(WINAPI * PFN_FACTORY)(REFIID, void**);
        if (auto mk = (PFN_FACTORY)GetProcAddress(hDxgi, "CreateDXGIFactory1")) {
            Com<IDXGIFactory1> fac;
            if (SUCCEEDED(mk(__uuidof(IDXGIFactory1), (void**)fac.put()))) {
                SIZE_T bestMem = 0;
                for (UINT i = 0;; i++) {
                    Com<IDXGIAdapter> a;
                    if (fac->EnumAdapters(i, a.put()) != S_OK) break;
                    DXGI_ADAPTER_DESC ds{};
                    if (FAILED(a->GetDesc(&ds)) || ds.VendorId == 0x1414) continue;
                    fprintf(stderr, "GpuLdpc: adapter %u: %ls, %zu MB\n", i, ds.Description, (size_t)(ds.DedicatedVideoMemory >> 20));
                    if (!best || ds.DedicatedVideoMemory > bestMem) { best = std::move(a); bestMem = ds.DedicatedVideoMemory; }
                }
            }
        }
    }
    if (FAILED(create(best.p, best ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, d.dev.put(), &got, d.ctx.put()))) { fprintf(stderr, "GpuLdpc: no Direct3D 11 device\n"); return; }
    // a compute shader for 360 threads and structured buffers needs a real feature level 11 device
    if (got < D3D_FEATURE_LEVEL_11_0) { d.dev.reset(); d.ctx.reset(); return; }
    {
        Com<IDXGIDevice> dxgi;
        if (SUCCEEDED(d.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)dxgi.put()))) {
            Com<IDXGIAdapter> ad;
            if (SUCCEEDED(dxgi->GetAdapter(ad.put()))) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(ad->GetDesc(&desc))) {
                    char buf[256] = {0};
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof buf - 1, nullptr, nullptr);
                    d.name = buf;
                    // the software rasteriser ("Microsoft Basic Render Driver") is slower than the CPU decoder
                    if (desc.VendorId == 0x1414) { d.dev.reset(); d.ctx.reset(); return; }
                }
            }
        }
    }
    for (int which = 0; which < 3; which++) {
        Com<ID3DBlob> code, err;
        const HRESULT hr = compile(kHlsl, strlen(kHlsl), "ldpc.hlsl", nullptr, nullptr, which == 0 ? "ldpc" : which == 1 ? "finish" : "init", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), err.put());
        if (FAILED(hr) || !code) {
            fprintf(stderr, "GpuLdpc: shader compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
            d.dev.reset(); d.ctx.reset();
            return;
        }
        if (FAILED(d.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, (which == 0 ? d.csLdpc : which == 1 ? d.csFinish : d.csInit).put()))) { d.dev.reset(); d.ctx.reset(); return; }
    }
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = sizeof(CBuf);
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(d.dev->CreateBuffer(&cd, nullptr, d.cbuf.put()))) { d.dev.reset(); d.ctx.reset(); return; }
    d.ready = true;
    fprintf(stderr, "GpuLdpc: using \"%s\"\n", d.name.c_str());
    // Self-test: shader compilers and drivers differ, and a wrong decoder is far worse than a slow one. A few noisy blocks are decoded on the
    // GPU and the result must be the transmitted data; otherwise the GPU decoder switches itself off and the CPU decoder is used.
    if (!selfTest()) { d.ready = false; fprintf(stderr, "GpuLdpc: self-test failed on \"%s\", using the CPU decoder\n", d.name.c_str()); }
}

bool GpuLdpc::selfTest() {
    PlpFec f; f.shortFrame = true; f.rate = 2; f.mod = 2; f.rotation = false;   // 16200-bit frame, rate 2/3
    const LdpcCode& c = ldpcFor(f);
    const int n = c.n(), k = c.k(), nb = 4;
    std::mt19937 rng(12345);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double sigma = 0.55;
    std::vector<float> llr((size_t)nb * n);
    std::vector<std::vector<uint8_t>> cw((size_t)nb);
    for (int b = 0; b < nb; b++) {
        cw[(size_t)b].resize((size_t)n);
        for (int i = 0; i < k; i++) cw[(size_t)b][(size_t)i] = rng() & 1;
        c.encode(cw[(size_t)b]);
        for (int i = 0; i < n; i++) { const float x = (cw[(size_t)b][(size_t)i] ? -1.f : 1.f) + (float)sigma * nd(rng); llr[(size_t)b * n + i] = 2.f * x / (float)(sigma * sigma); }
    }
    std::vector<uint8_t> hard((size_t)nb * n), ok((size_t)nb);
    std::vector<int> it((size_t)nb);
    if (!decode(c, llr.data(), nb, 50, hard.data(), ok.data(), it.data())) return false;
    for (int b = 0; b < nb; b++) {
        if (!ok[(size_t)b]) return false;
        for (int i = 0; i < n; i++) if (hard[(size_t)b * n + i] != cw[(size_t)b][(size_t)i]) return false;
    }
    return true;
}

GpuLdpc::~GpuLdpc() = default;
GpuLdpc& GpuLdpc::instance() { static GpuLdpc g; return g; }
bool GpuLdpc::available() const { return p_->ready; }
const char* GpuLdpc::deviceName() const { return p_->name.c_str(); }

bool GpuLdpc::decode(const LdpcCode& code, const float* llr, int nb, int maxIter, uint8_t* hard, uint8_t* ok, int* iters) {
    if (!available() || code.q() * 360 != code.n() - code.k()) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& d = *p_;
    Impl::Gpu* sp = d.state(code);
    if (!sp || !sp->ok) return false;
    Impl::Gpu& s = *sp;
    const int n = code.n();
    CBuf cb{};
    cb.k = (uint32_t)code.k(); cb.n = (uint32_t)n; cb.q = (uint32_t)code.q(); cb.G = (uint32_t)code.groups();
    cb.alpha = 0.78f; cb.msgPer = s.msgPer; cb.postPer = s.postPer; cb.nWords = s.nWords;
    for (size_t i = 0; i < s.layerR.size() && i < 256; i++) cb.layerR[i] = s.layerR[i];

    for (int b0 = 0; b0 < nb; b0 += kChunkBlocks) {
        const int cnt = std::min(kChunkBlocks, nb - b0);
        if (!d.ensure(s, kChunkBlocks, n)) return false;
        d.write(s.llr.p, llr + (size_t)b0 * n, (UINT)((size_t)cnt * n * 4));
        std::vector<int> infoHost((size_t)cnt * 2, 0);
        for (int i = 0; i < cnt; i++) infoHost[(size_t)i * 2 + 1] = 0;
        d.write(s.info.p, infoHost.data(), (UINT)(infoHost.size() * 4));

        std::vector<uint32_t> act((size_t)cnt);
        for (int i = 0; i < cnt; i++) act[(size_t)i] = (uint32_t)i;
        std::vector<int> finalOk((size_t)cnt, 0), finalIt((size_t)cnt, maxIter);
        int done = 0;
        bool firstPass = true;
        while (!act.empty() && done < maxIter) {
            const int ch = std::min(kIterPerDispatch, maxIter - done);
            cb.chunk = (uint32_t)ch; cb.first = firstPass ? 1u : 0u; cb.iterBase = (uint32_t)done;
            d.ctx->UpdateSubresource(d.cbuf.p, 0, nullptr, &cb, 0, 0);
            d.write(s.active.p, act.data(), (UINT)(act.size() * 4));
            if (firstPass) {   // the starting values are written by a dispatch of their own, so they are complete before any layer reads them
                d.bind(s, d.csInit.p);
                d.ctx->Dispatch((UINT)act.size(), 1, 1);
                d.unbind();
            }
            d.bind(s, d.csLdpc.p);
            d.ctx->Dispatch((UINT)act.size(), 1, 1);
            d.unbind();
            d.ctx->CopyResource(s.infoStage.p, s.info.p);
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(d.ctx->Map(s.infoStage.p, 0, D3D11_MAP_READ, 0, &mp))) return false;
            const int* inf = (const int*)mp.pData;
            std::vector<uint32_t> next;
            for (uint32_t blk : act) {
                if (inf[blk * 2]) { finalOk[blk] = 1; finalIt[blk] = inf[blk * 2 + 1]; }
                else next.push_back(blk);
            }
            d.ctx->Unmap(s.infoStage.p, 0);
            act.swap(next);
            done += ch;
            firstPass = false;
        }
        // hard decisions of every block of the batch
        std::vector<uint32_t> all((size_t)cnt);
        for (int i = 0; i < cnt; i++) all[(size_t)i] = (uint32_t)i;
        cb.first = 0;
        d.ctx->UpdateSubresource(d.cbuf.p, 0, nullptr, &cb, 0, 0);
        d.write(s.active.p, all.data(), (UINT)(all.size() * 4));
        d.bind(s, d.csFinish.p);
        d.ctx->Dispatch((UINT)cnt, 1, 1);
        d.unbind();
        d.ctx->CopyResource(s.hardStage.p, s.hard.p);
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (FAILED(d.ctx->Map(s.hardStage.p, 0, D3D11_MAP_READ, 0, &mp))) return false;
        const uint32_t* w = (const uint32_t*)mp.pData;
        for (int i = 0; i < cnt; i++) {
            const uint32_t* bw = w + (size_t)i * n;
            uint8_t* out = hard + (size_t)(b0 + i) * n;
            for (int j = 0; j < n; j++) out[j] = (uint8_t)bw[j];
            ok[b0 + i] = (uint8_t)finalOk[(size_t)i];
            iters[b0 + i] = finalIt[(size_t)i];
        }
        d.ctx->Unmap(s.hardStage.p, 0);
    }
    return true;
}

} // namespace dect2
