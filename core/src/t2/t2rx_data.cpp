// The data stage of the DVB-T2 receiver: channel estimation from the scattered pilots and handing cells to the error correction.
#include "t2rx_impl.h"
#include "dect2/dsp/simd.h"

namespace dect2 {

namespace {
// h = a + dl * b over n floats (the channel of a symbol from the frame's middle value and slope)
DECT2_SIMD_BODY void axpyBody(const float* a, const float* b, float dl, float* h, int n) {
    for (int i = 0; i < n; i++) h[i] = a[i] + dl * b[i];
}
DECT2_AVX2_DISPATCH(void, axpy, (const float* a, const float* b, float dl, float* h, int n), (a, b, dl, h, n))
// Equalised cells and their noise variance for the K carriers of a symbol: eq = cell conj(H) / |H|^2, n0 = noise scale / (2 |H|^2)
DECT2_SIMD_BODY void equaliseBody(const float* cell, const float* h, const float* noise, float scale, float* eq, float* n0, int K) {
    for (int k = 0; k < K; k++) {
        const float hr = h[2 * k], hi = h[2 * k + 1], cr = cell[2 * k], ci = cell[2 * k + 1];
        const float g2 = std::max(1e-12f, hr * hr + hi * hi);
        eq[2 * k] = (cr * hr + ci * hi) / g2;
        eq[2 * k + 1] = (ci * hr - cr * hi) / g2;
        n0[k] = noise[k] * scale / (2.0f * g2);
    }
}
DECT2_AVX2_DISPATCH(void, equalise, (const float* cell, const float* h, const float* noise, float scale, float* eq, float* n0, int K), (cell, h, noise, scale, eq, n0, K))
} // namespace

void T2Receiver::Impl::submitPlp(const std::vector<cf32>& dstream, const std::vector<float>& dn0, double sigma2) {
    const int cP2 = (int)p2Sym[0].size();
    const int nPre = 1840, nPost = l1pre.postSize;
    // pick the data PLP (first non-common PLP, else the first)
    int sel = -1;
    const int want = plpSelect.load();
    if (want >= 0) for (size_t i = 0; i < l1post.plps.size(); i++) if (l1post.plps[i].id == want) { sel = (int)i; break; }
    if (sel < 0) { sel = 0; for (size_t i = 0; i < l1post.plps.size(); i++) if (l1post.plps[i].type == 1) { sel = (int)i; break; } }
    if (sel < 0 || sel >= (int)l1post.plps.size()) return;
    const L1PlpConf& pc = l1post.plps[sel];
    if (sel >= (int)l1post.dyn.size()) return;
    const L1PlpDyn& dy = l1post.dyn[sel];
    selectedPlpId = pc.id;
    if (pc.timeIlType != 0 || pc.type == 2 || l1post.subSlices > 1) { plpSkipped++; return; } // inter-frame interleaving and sub-slicing are not implemented
    PlpJob job;
    job.frameNo = ++frameCounter;
    job.t2Frame = l1post.frameIdx;
    job.numT2Frames = l1pre.numFrames;
    job.frameInterval = std::max(1, pc.frameInterval);
    job.frameSec = frameMsv / 1e3;
    job.plpId = pc.id;
    job.fec.shortFrame = pc.fecType == 0;
    job.fec.rate = pc.cod;
    job.fec.mod = pc.mod;
    job.fec.rotation = pc.rotation != 0;
    job.numBlocks = dy.numBlocks;
    job.tiBlocks = pc.timeIlLength;
    FecDims d = fecDims(job.fec);
    if (!d.ok || job.numBlocks <= 0) { plpSkipped++; return; }
    std::vector<cf32> stream, g2s;
    p2Gather(p2Sym, nP2, cP2, nPre, nPost, stream);
    {
        std::vector<std::vector<cf32>> gs(nP2);
        for (int l = 0; l < nP2; l++) { gs[l].resize(cP2); for (int j = 0; j < cP2; j++) gs[l][j] = p2G2[l][j]; }
        p2Gather(gs, nP2, cP2, nPre, nPost, g2s);
    }
    std::vector<float> n0s(stream.size());
    for (size_t i = 0; i < stream.size(); i++) n0s[i] = (float)(sigma2 / (2.0 * std::max(1e-12f, g2s[i].real())));
    stream.insert(stream.end(), dstream.begin(), dstream.end());
    n0s.insert(n0s.end(), dn0.begin(), dn0.end());
    const size_t start = (size_t)nPre + nPost + dy.start;
    const size_t need = (size_t)job.numBlocks * d.cellsPerBlock;
    if (start + need > stream.size()) { plpSkipped++; return; }
    job.cells.assign(stream.begin() + start, stream.begin() + start + need);
    job.n0.assign(n0s.begin() + start, n0s.begin() + start + need);
    plpValid = true; plpId = pc.id; plpFec = job.fec; plpBlocks = job.numBlocks;
    plpDec.submit(std::move(job));
}

void T2Receiver::Impl::pollPlp() {
    PlpResult r;
    while (plpDec.poll(r)) {
        plpFrames++;
        blocksOk += r.blocksOk; blocksBad += r.bchFailed; headersOk += r.headerOk; plpBchCorr += r.bchCorrected;
        plpMer = r.blocksOk ? r.merDb : plpMer;
        plpPre = r.preBer; plpIters = r.avgLdpcIters; plpMs = r.decodeMs; plpGpu = r.usedGpu;
        {
            std::vector<uint8_t> bm(r.frames.size(), 0);
            for (size_t i = 0; i < r.frames.size(); i++) bm[i] = r.frames[i].bits.empty() ? 0 : 1;
            blockMaps.push_back(std::move(bm));
            if (blockMaps.size() > 90) blockMaps.pop_front();
        }
        if (!r.constellation.empty()) { plpConst = r.constellation; plpConstErr = r.constErr; plpConstTx = r.constTx; plpConstSeq++; }
        for (auto& f : r.frames) if (f.header.crcOk) { hUpl = f.header.upl; hDfl = f.header.dfl; hSyncd = f.header.syncd; break; }
        if (plpCb) plpCb(r);
    }
}

void T2Receiver::Impl::runDataStage() {
    StageClock sc(5);
    const FftMode* fm = fftModeFromS2(fftCode);
    if (!fm || !l1preOk) return;
    const int L = frameSyms, N = fftN, G = guard;
    for (int l = 0; l < L; l++) if ((int)frameCells[l].size() != kMax) { if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] data stage skipped: symbol %d of %d has no cells\n", l, L); return; }
    PilotConfig pc;
    pc.fftCode = fftCode; pc.ext = l1pre.bwtExt != 0; pc.pp = l1pre.pilotPattern; pc.tr = (l1pre.papr & 2) != 0; pc.giIdx = giIdx;
    {
        const PilotConfig& c0 = dsCache.pc;
        if (!dsCache.pm || dsCache.L != L || c0.fftCode != pc.fftCode || c0.ext != pc.ext || c0.pp != pc.pp || c0.tr != pc.tr || c0.giIdx != pc.giIdx) {
            dsCache.pm.reset(new PilotMap(pc));
            dsCache.pc = pc; dsCache.L = L;
            dsCache.types.assign(L, {});
            dsCache.dataIdx.assign(L, {});
            if (dsCache.pm->valid()) for (int l = 0; l < L; l++) {
                dsCache.pm->symbolTypes(l, L, dsCache.types[l]);
                auto& di = dsCache.dataIdx[(size_t)l];
                for (int kk = 0; kk < (int)dsCache.types[l].size(); kk++) if (dsCache.types[l][kk] == kCellData) di.push_back(kk);
            }
        }
    }
    const PilotMap& pm = *dsCache.pm;
    if (!pm.valid()) { dataValid = false; return; }
    const int K = pm.carriers(), off = (kMax - K) / 2, dx = pm.dx(), dy = pm.dy();
    const int M = (K - 1) / dx + 1;
    struct Obs { int l; cf32 v; uint8_t type; };
    std::vector<std::vector<Obs>> obs(M);
    const std::vector<std::vector<uint8_t>>& types = dsCache.types;
    auto erased = [&](int l) { return l < (int)symErased.size() && symErased[(size_t)l]; };
    for (int l = 0; l < L; l++) {
        if (erased(l)) continue;
        for (int n = 0; n < M; n++) {
            int k = dx * n;
            uint8_t t = types[l][k];
            if (t != kCellP2Pilot && t != kCellScattered && t != kCellContinual) continue;
            cf32 pv = pm.pilot(l, k, t);
            obs[n].push_back({l, frameCells[l][off + k] / pv.real(), t});
        }
    }
    if (getenv("DECT2_DEBUG")) {
        // which pilot configuration explains the received cells? coherence of consecutive symbols on that config's continual pilots
        for (int e2 = 0; e2 < 2; e2++) for (int ppc = 0; ppc < 8; ppc++) {
            PilotConfig c2 = pc; c2.pp = ppc; c2.ext = e2;
            PilotMap m2(c2);
            if (!m2.valid()) continue;
            int K2 = m2.carriers(), off2 = (kMax - K2) / 2;
            cd tot = 0; double mag = 0; int n2 = 0;
            std::vector<uint8_t> ta, tb;
            for (int l = nP2 + 1; l < std::min(L - 1, nP2 + 12); l++) {
                m2.symbolTypes(l, L, ta); m2.symbolTypes(l - 1, L, tb);
                for (int kk = 0; kk < K2; kk++) {
                    if (ta[kk] != kCellContinual || tb[kk] != kCellContinual) continue;
                    cf32 za = frameCells[l][off2 + kk] / m2.pilot(l, kk, kCellContinual).real();
                    cf32 zb = frameCells[l - 1][off2 + kk] / m2.pilot(l - 1, kk, kCellContinual).real();
                    cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                    tot += pr; mag += std::abs(pr); n2++;
                }
            }
            // scattered pilots, symbols 16 apart / dy apart, same carriers: coherence over dy symbols
            cd tot2 = 0; double mag2 = 0; int n3 = 0;
            int dy2 = m2.dy();
            for (int l = nP2; l + dy2 < std::min(L - 1, nP2 + 3 * dy2); l++) {
                m2.symbolTypes(l, L, ta); m2.symbolTypes(l + dy2, L, tb);
                for (int kk = 0; kk < K2; kk++) {
                    if (ta[kk] != kCellScattered || tb[kk] != kCellScattered) continue;
                    cf32 za = frameCells[l][off2 + kk] / m2.pilot(l, kk, kCellScattered).real();
                    cf32 zb = frameCells[l + dy2][off2 + kk] / m2.pilot(l + dy2, kk, kCellScattered).real();
                    cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                    tot2 += pr; mag2 += std::abs(pr); n3++;
                }
            }
            fprintf(stderr, "  [dbg] ext %d PP%d: continual coherence %.3f (%d)  scattered(dy) coherence %.3f (%d) phase %.2f\n", e2, ppc + 1, mag > 0 ? std::abs(tot) / mag : 0.0, n2, mag2 > 0 ? std::abs(tot2) / mag2 : 0.0, n3, std::arg(tot2));
        }
    }
    // Common phase error and residual timing: compare each symbol with the previous one on the continual pilots.
    // The symbol grid has integer-sample resolution, so the sub-sample timing error shows up as a phase slope
    // across the carriers (up to ~1 rad at the band edge for 32K); it is estimated and removed here.
    {
        double cumA = 0, cumB = 0;
        std::vector<int> cont;
        std::vector<cd> prod;
        // raw (uncorrected) cells of the previous symbol, kept at its pilot carriers only (the only ones that are compared)
        std::vector<cf32> prevRaw(K), curRaw(K);
        auto isPilot = [](uint8_t t) { return t == kCellContinual || t == kCellScattered; };
        for (int kk = 0; kk < K; kk++) if (isPilot(types[nP2][kk])) prevRaw[kk] = frameCells[nP2][off + kk];
        int pl = nP2;   // the last symbol compared (symbols lost with dropped samples are stepped over)
        for (int l = nP2 + 1; l < L; l++) {
            if (erased(l)) continue;
            cont.clear(); prod.clear();
            for (int kk = 0; kk < K; kk++) if (isPilot(types[l][kk])) curRaw[kk] = frameCells[l][off + kk];
            // Continual pilots on both symbols as a rule. The frame-closing symbol has none: there the carriers where both symbols have a
            // pilot of any kind are used (every dx * dy-th). Skipping it left the closing symbol without the phase and timing that the
            // symbols before it had accumulated (several radians with a few tens of Hz of residual frequency offset): its pilots then
            // spoiled the frame-wide channel fit, and every cell of the frame with it (a real 32K PP4 multiplex: MER 10-15 dB, no PLP).
            for (int pass = 0; pass < 2 && cont.size() < 4; pass++) {
                cont.clear(); prod.clear();
                for (int kk = 0; kk < K; kk++) {
                    const uint8_t ta = types[l][kk], tb = types[pl][kk];
                    if (pass == 0 ? (ta != kCellContinual || tb != kCellContinual) : (!isPilot(ta) || !isPilot(tb))) continue;
                    cf32 za = frameCells[l][off + kk] / pm.pilot(l, kk, ta).real();
                    cf32 zb = prevRaw[kk] / pm.pilot(pl, kk, tb).real();
                    cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                    cont.push_back(kk);
                    prod.push_back(pr);
                }
            }
            std::swap(prevRaw, curRaw);
            pl = l;
            if (cont.size() < 4) continue;
            // slope from phase differences of neighbouring pilots
            cd sl = 0;
            double wsum = 0, dsum = 0;
            for (size_t i = 0; i + 1 < cont.size(); i++) {
                double w = std::min(std::abs(prod[i]), std::abs(prod[i + 1]));
                sl += prod[i + 1] * std::conj(prod[i]) / std::max(1e-12, std::abs(prod[i]) * std::abs(prod[i + 1])) * w;
                dsum += w * (cont[i + 1] - cont[i]);
                wsum += w;
            }
            double beta = (wsum > 0 && dsum > 0) ? std::arg(sl) / (dsum / wsum) : 0.0;
            cd a = 0;
            for (size_t i = 0; i < cont.size(); i++) a += prod[i] * std::polar(1.0, -beta * (cont[i] - (K - 1) / 2.0));
            double alpha = std::arg(a);
            cumA += alpha;
            cumB += beta;
            if (getenv("DECT2_DEBUG") && l < nP2 + 14) fprintf(stderr, "  [dbg] sym %d: alpha %+.3f beta*10k %+.3f (cum %+.2f, %+.2f) pilots %zu\n", l, alpha, beta * 1e4, cumA, cumB * 1e4, cont.size());
            // rotate by exp(-j (cumA + cumB (kk - (K-1)/2))): a phasor stepped along the carriers, re-anchored every 256 (a sin and cos per cell is slow)
            const cd stepR(std::cos(-cumB), std::sin(-cumB));
            cf32* row = &frameCells[l][off];
            // four blocks side by side (four independent phasor chains instead of one; every cell gets the phasor it got before)
            int k0 = 0;
            for (; k0 + 4 * 256 <= K; k0 += 4 * 256) {
                cd z[4];
                for (int q = 0; q < 4; q++) { const double ph = -(cumA + cumB * (k0 + 256 * q - (K - 1) / 2.0)); z[q] = cd(std::cos(ph), std::sin(ph)); }
                for (int j = 0; j < 256; j++)
                    for (int q = 0; q < 4; q++) { row[k0 + 256 * q + j] *= cf32((float)z[q].real(), (float)z[q].imag()); z[q] *= stepR; }
            }
            for (; k0 < K; k0 += 256) {
                const double ph = -(cumA + cumB * (k0 - (K - 1) / 2.0));
                cd z(std::cos(ph), std::sin(ph));
                const int k1 = std::min(K, k0 + 256);
                for (int kk = k0; kk < k1; kk++) { row[kk] *= cf32((float)z.real(), (float)z.imag()); z *= stepR; }
            }
        }
        if (getenv("DECT2_DEBUG")) {
            std::vector<uint8_t> ta2, tb2;
            cd tot2 = 0; double mag2 = 0; int n3 = 0;
            for (int l = nP2; l + dy < std::min(L - 1, nP2 + 3 * dy); l++) {
                for (int kk = 0; kk < K; kk++) {
                    if (types[l][kk] != kCellScattered || types[l + dy][kk] != kCellScattered) continue;
                    cf32 za = frameCells[l][off + kk] / pm.pilot(l, kk, kCellScattered).real();
                    cf32 zb = frameCells[l + dy][off + kk] / pm.pilot(l + dy, kk, kCellScattered).real();
                    cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                    tot2 += pr; mag2 += std::abs(pr); n3++;
                }
            }
            for (int lag : {1, 4, 16}) {
                cd tc = 0; double mc = 0; int nc = 0;
                for (int l = nP2; l + lag < std::min(L, nP2 + 40); l++)
                    for (int kk = 0; kk < K; kk++) {
                        if (types[l][kk] != kCellContinual || types[l + lag][kk] != kCellContinual) continue;
                        cf32 za = frameCells[l][off + kk] / pm.pilot(l, kk, kCellContinual).real();
                        cf32 zb = frameCells[l + lag][off + kk] / pm.pilot(l + lag, kk, kCellContinual).real();
                        cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                        tc += pr; mc += std::abs(pr); nc++;
                    }
                fprintf(stderr, "  [dbg] continual coherence at lag %d: %.3f (%d)\n", lag, mc > 0 ? std::abs(tc) / mc : 0.0, nc);
            }
            fprintf(stderr, "  [dbg] AFTER correction: scattered(dy) coherence %.3f (%d) phase %.2f\n", mag2 > 0 ? std::abs(tot2) / mag2 : 0.0, n3, std::arg(tot2));
        }
        // rebuild the pilot observations from the phase-corrected cells
        for (int n = 0; n < M; n++) obs[n].clear();
        for (int l = 0; l < L; l++)
            for (int n = 0; n < M; n++) {
                if (erased(l)) break;
                int k = dx * n;
                uint8_t t = types[l][k];
                if (t != kCellP2Pilot && t != kCellScattered && t != kCellContinual) continue;
                obs[n].push_back({l, frameCells[l][off + k] / pm.pilot(l, k, t).real(), t});
            }
    }
    // per-grid-point noise from scattered-pilot second differences
    std::vector<float> varN(M, 0.f), pw(M, 0.f);
    double sumSig = 0, sumNoise = 0;
    for (int n = 0; n < M; n++) {
        std::vector<const Obs*> sc;
        for (auto& o : obs[n]) if (o.type == kCellScattered) sc.push_back(&o);
        double acc = 0, p = 0;
        int c = 0;
        for (size_t j = 1; j + 1 < sc.size(); j++) {
            double w = (double)(sc[j]->l - sc[j - 1]->l) / (double)(sc[j + 1]->l - sc[j - 1]->l);
            cf32 pred = sc[j - 1]->v + (sc[j + 1]->v - sc[j - 1]->v) * (float)w;
            double e = std::norm(sc[j]->v - pred) / (1.0 + w * w + (1 - w) * (1 - w));
            acc += e;
            p += std::norm(sc[j]->v);
            c++;
        }
        if (c) { varN[n] = (float)(acc / c); pw[n] = (float)(p / c); sumSig += pw[n]; sumNoise += varN[n]; }
    }
    const float amp2 = [&] { cf32 pv = pm.pilot(nP2, 0, kCellScattered); return pv.real() * pv.real(); }();
    const float cpAmp2 = [&] { for (int kk = 0; kk < K; kk++) if (types[nP2][kk] == kCellContinual) { cf32 pv = pm.pilot(nP2, kk, kCellContinual); return pv.real() * pv.real(); } return amp2; }();
    dataSnrCar.clear();
    const int Wd = 10;
    // noise power of the cells per grid point (over 2 Wd + 1 neighbours), for the LDPC input: the noise is not flat across the band on
    // real signals (a spur, an adjacent channel folded in, a filter edge), and a band-average would make the cells in the worst part
    // over-confident and those in the best part timid
    std::vector<float> nGrid(M);
    for (int n = 0; n < M; n++) {
        double sa = 0, na = 0; int c = 0;
        for (int j = std::max(0, n - Wd); j <= std::min(M - 1, n + Wd); j++) { sa += pw[j]; na += varN[j]; c += varN[j] > 0; }
        dataSnrCar.push_back((float)(10 * std::log10(std::max(1e-9, sa) / std::max(1e-12, na * amp2))));
        nGrid[n] = c ? (float)(na / c * amp2) : 0.f;
    }
    dataSnr = (float)(10 * std::log10(std::max(1e-9, sumSig) / std::max(1e-12, sumNoise * amp2)));
    if (getenv("DECT2_DEBUG")) {
        int cntN = 0; for (int n = 0; n < M; n++) cntN += varN[n] > 0;
        double ratioMed = 0; std::vector<double> rr; for (int n = 0; n < M; n++) if (varN[n] > 0 && pw[n] > 0) rr.push_back(10 * std::log10(pw[n] / varN[n]));
        std::sort(rr.begin(), rr.end()); if (!rr.empty()) ratioMed = rr[rr.size() / 2];
        fprintf(stderr, "  [dbg] data SNR: grid %d with noise est %d, sumSig %.3g sumNoise %.3g amp2 %.2f median pilot-SNR %.1f dB => %.1f dB\n", M, cntN, sumSig, sumNoise, amp2, ratioMed, dataSnr);
    }

    // channel per data symbol: time-interpolate each grid point, then frequency-interpolate
    std::vector<size_t> ptr(M, 0);
    std::vector<cf32> grid(M), H;
    const int back = std::min(G / 4, 32);
    std::vector<cf32> all;
    const int firstData = nP2;
    // noise power of the raw FFT cells: variance of the pilot observations times the pilot power
    double sigma2 = 0;
    {
        double sn = 0; int cn = 0;
        for (int n = 0; n < M; n++) if (varN[n] > 0) { sn += varN[n]; cn++; }
        sigma2 = cn ? sn / cn * amp2 : 0;
    }
    std::vector<cf32> dstream; std::vector<float> dn0;
    dstream.reserve((size_t)std::max(0, L - firstData) * (size_t)K); dn0.reserve((size_t)std::max(0, L - firstData) * (size_t)K);
    // per carrier, linear between the grid points (never below a tenth of the average: a lucky quiet stretch is no reason for certainty)
    std::vector<float> noiseCar(K, 0.f);
    for (int kk = 0; kk < K; kk++) {
        const int n0i = std::min(M - 1, kk / dx), n1i = std::min(M - 1, n0i + 1);
        const float w = (float)(kk - n0i * dx) / (float)dx;
        noiseCar[kk] = std::max((float)(0.1 * sigma2), nGrid[n0i] + w * (nGrid[n1i] - nGrid[n0i]));
    }
    const bool wantPlp = l1postOk && !l1post.plps.empty() && !p2Sym.empty() && sigma2 > 0;
    // Per grid carrier: weighted least-squares fit of H(l) = a + b (l - lbar) over every pilot observation in the frame
    // (P2 symbols excluded: they carry a different phase/timing reference). The slope is shrunk towards zero when it is
    // not significant, which averages the pilot noise over the whole frame for a quasi-static channel.
    std::vector<cf32> fa(M), fb(M);
    std::vector<float> flbar(M, 0.f);
    for (int n = 0; n < M; n++) {
        double sw = 0, sl = 0;
        cd sv = 0;
        int cnt = 0;
        for (auto& o : obs[n]) {
            if (o.type == kCellP2Pilot) continue;
            const double w = (o.type == kCellContinual ? cpAmp2 : amp2);
            sw += w; sl += w * o.l; sv += w * cd(o.v.real(), o.v.imag()); cnt++;
        }
        if (cnt == 0) { fa[n] = n ? fa[n - 1] : cf32(1, 0); fb[n] = 0; continue; }
        const double lbar = sl / sw;
        const cd a0 = sv / sw;
        double stt = 0;
        cd stv = 0;
        for (auto& o : obs[n]) {
            if (o.type == kCellP2Pilot) continue;
            const double w = (o.type == kCellContinual ? cpAmp2 : amp2), t = o.l - lbar;
            stt += w * t * t;
            stv += w * t * cd(o.v.real(), o.v.imag());
        }
        cd b0 = 0;
        if (cnt >= 3 && stt > 1e-9) {
            b0 = stv / stt;
            double snr = stt * std::norm(b0) / std::max(1e-18, (double)varN[n] * amp2 * 0 + 1.0 / 1.0 * 0 + (varN[n] > 0 ? varN[n] * amp2 : 1e-9));
            // James-Stein style shrinkage of the slope: keep it only if it stands out of the noise
            double shrink = std::max(0.0, 1.0 - 1.0 / std::max(1e-9, snr));
            b0 *= shrink;
        }
        fa[n] = cf32((float)a0.real(), (float)a0.imag());
        fb[n] = cf32((float)b0.real(), (float)b0.imag());
        flbar[n] = (float)lbar;
    }
    // A moving receiver: the channel is not a straight line over the frame (at 20 Hz Doppler a path turns by a whole radian in 60 ms). How far
    // the pilots stray from the frame's line, against their noise (from the second differences, varN), says how short the pieces must be: the
    // misfit of a line falls with the fourth power of its length, so each piece is cut to leave a quarter of the noise. Each piece gets its own
    // weighted line from the pilots within one piece length of its centre (so neighbouring pieces overlap and the noise is still averaged).
    int nSeg = 1, segLen = L - firstData;
    std::vector<std::vector<cf32>> sa, sb;   // per piece: the line's value at its centre and its slope, per grid point
    std::vector<int> segC;
    // weighted line per piece from the pilots within one piece length of its centre; returns the pilots' misfit against the pieces
    auto fitSegs = [&](int ns, int sl) {
        sa.assign((size_t)ns, std::vector<cf32>(M)); sb.assign((size_t)ns, std::vector<cf32>(M)); segC.assign((size_t)ns, 0);
        for (int sg = 0; sg < ns; sg++) {
            const int l0 = firstData + sg * sl, l1 = std::min(L, l0 + sl);
            segC[(size_t)sg] = (int)std::lround(0.5 * (l0 + l1 - 1));
            const double W = sl + 0.5 * dy;   // half-width of the window of pilots used
            for (int n = 0; n < M; n++) {
                double sw = 0, st = 0, stt = 0; cd sv = 0, stv = 0;
                for (auto& o : obs[n]) {
                    if (o.type == kCellP2Pilot || o.l < firstData) continue;
                    const double t = o.l - segC[(size_t)sg];
                    if (std::fabs(t) >= W) continue;
                    const double w = (o.type == kCellContinual ? cpAmp2 : amp2) * (1.0 - std::fabs(t) / W);
                    const cd v(o.v.real(), o.v.imag());
                    sw += w; st += w * t; stt += w * t * t; sv += w * v; stv += w * t * v;
                }
                if (sw <= 0) { sa[(size_t)sg][n] = fa[n] + fb[n] * ((float)segC[(size_t)sg] - flbar[n]); sb[(size_t)sg][n] = fb[n]; continue; }
                const double det = sw * stt - st * st;
                cd a, b;
                if (det > 1e-9 * sw * sw) { b = (sw * stv - st * sv) / det; a = (sv - b * st) / sw; }
                else { b = cd(fb[n].real(), fb[n].imag()); a = sv / sw - b * (st / sw); }
                sa[(size_t)sg][n] = cf32((float)a.real(), (float)a.imag()); sb[(size_t)sg][n] = cf32((float)b.real(), (float)b.imag());
            }
        }
        double r = 0; long c = 0;
        for (int n = 0; n < M; n++)
            for (auto& o : obs[n]) {
                if (o.type == kCellP2Pilot || o.l < firstData) continue;
                const int sg = std::min(ns - 1, std::max(0, (o.l - firstData) / sl));
                r += std::norm(o.v - (sa[(size_t)sg][n] + sb[(size_t)sg][n] * (float)(o.l - segC[(size_t)sg]))); c++;
            }
        return c ? r / (double)c : 0.0;
    };
    if (!getenv("DECT2_T2_CE_OLD")) {
        double rs = 0, nv = 0; long rc = 0; int nvc = 0;
        for (int n = 0; n < M; n++) {
            if (varN[n] > 0) { nv += varN[n]; nvc++; }
            for (auto& o : obs[n]) {
                if (o.type == kCellP2Pilot || o.l < firstData) continue;
                rs += std::norm(o.v - (fa[n] + fb[n] * ((float)o.l - flbar[n]))); rc++;
            }
        }
        if (rc > 0 && nvc > 0) {
            rs /= (double)rc; nv /= nvc;
            if (rs > 3.0 * nv) {
                const int span = L - firstData;
                const int minLen = std::max(2 * dy, 4);
                segLen = std::max(minLen, std::min(span, (int)std::floor(span * std::pow(0.25 * nv / (rs - nv), 0.25))));
                // a channel that turns by several radians over the frame does not follow the fourth-power rule: cut again until the
                // pieces fit (at most three times)
                for (int it = 0; it < 4; it++) {
                    nSeg = (span + segLen - 1) / segLen;
                    if (nSeg < 2) { nSeg = 1; segLen = span; break; }
                    const double r2 = fitSegs(nSeg, segLen);
                    // pieces that do not fit clearly better are only noisier (a still channel with deep notches: each piece averages
                    // fewer pilots): one line then
                    if (r2 > 0.6 * rs) { nSeg = 1; segLen = span; break; }
                    if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] channel over the frame: misfit of one line %.2f x noise, of %d pieces of %d symbols %.2f\n", rs / nv, nSeg, segLen, r2 / nv);
                    if (r2 <= 1.5 * nv || segLen <= minLen || it == 3) break;
                    segLen = std::max(minLen, (int)std::floor(segLen * std::pow(0.25 * nv / std::max(1e-12, r2 - nv), 0.25)));
                }
            }
        }
    }
    auto segOf = [&](int l) { return nSeg > 1 ? std::min(nSeg - 1, std::max(0, (l - firstData) / segLen)) : 0; };
    // Noise power of every data symbol: residual of the pilots against the fitted channel. Interference that comes and goes
    // (a few frames of raised noise, a burst) hits some symbols harder than the frame average that sigma2 describes; scaling the
    // cell noise variance per symbol keeps the LDPC input honest (over-confident wrong bits are far worse than weak ones).
    std::vector<double> symScale(L, 1.0);
    {
        std::vector<double> rp(L, 0.0); std::vector<int> rc(L, 0);
        for (int n = 0; n < M; n++)
            for (auto& o : obs[n]) {
                if (o.type == kCellP2Pilot || o.l < firstData) continue;
                const int sg = segOf(o.l);
                const cf32 fit = nSeg > 1 ? sa[(size_t)sg][n] + sb[(size_t)sg][n] * (float)(o.l - segC[(size_t)sg]) : fa[n] + fb[n] * ((float)o.l - flbar[n]);
                rp[(size_t)o.l] += std::norm(o.v - fit); rc[(size_t)o.l]++;
            }
        double tot = 0; long cnt = 0;
        for (int l = firstData; l < L; l++) { tot += rp[(size_t)l]; cnt += rc[(size_t)l]; }
        if (cnt > 0 && tot > 0) {
            const double mean = tot / (double)cnt;
            std::vector<double> raw(L, 1.0);
            for (int l = firstData; l < L; l++) raw[(size_t)l] = rc[(size_t)l] ? (rp[(size_t)l] / rc[(size_t)l]) / mean : 1.0;
            for (int l = firstData; l < L; l++) {   // light smoothing over neighbouring symbols, then limits
                const double a0 = raw[(size_t)std::max(firstData, l - 1)], a1 = raw[(size_t)l], a2 = raw[(size_t)std::min(L - 1, l + 1)];
                symScale[(size_t)l] = std::min(8.0, std::max(0.25, 0.25 * a0 + 0.5 * a1 + 0.25 * a2));
            }
        }
        if (getenv("DECT2_SYMNOISE")) {   // pilot residual against the frame's channel fit per data symbol, dB below the pilot power
            fprintf(stderr, "SYMNOISE frame %llu:", (unsigned long long)(frameCounter + 1));
            for (int l = firstData; l < L; l++) fprintf(stderr, " %.1f", rc[(size_t)l] ? 10 * std::log10(rp[(size_t)l] / rc[(size_t)l] / (amp2 * std::max(1e-12, (double)sumSig / M))) : 99.0);
            fprintf(stderr, "\n");
        }
    }
    // The grid is linear in the symbol index (a + b (l - lbar) per grid point) and the interpolator is linear, so it is run twice per
    // frame, on the grid at the middle data symbol and on the slopes, instead of once per symbol: H(l) = H(lc) + (l - lc) * Hslope.
    const int lc = firstData + (L - firstData) / 2;
    std::vector<cf32> Hmid, Hslope;
    std::vector<std::vector<cf32>> segMid((size_t)nSeg), segSlope((size_t)nSeg);
    {
        const double tau0 = (double)back + (chDelayKnown ? chDelayCentre : G / 2.0);
        const double cut = chDelayKnown ? std::min(1.0, chDelayHalf / ((double)N / (2.0 * dx))) : 1.0;
        if (nSeg > 1) {
            for (int sg = 0; sg < nSeg; sg++) {
                interp.run(sa[(size_t)sg], dx, K, N, tau0, segMid[(size_t)sg], cut);
                interp.run(sb[(size_t)sg], dx, K, N, tau0, segSlope[(size_t)sg], cut);
            }
        } else {
            for (int n = 0; n < M; n++) grid[n] = fa[n] + fb[n] * ((float)lc - flbar[n]);
            interp.run(grid, dx, K, N, tau0, Hmid, cut);
            interp.run(fb, dx, K, N, tau0, Hslope, cut);
        }
    }
    // the equalised cells are only kept for the display, which thins them to about 6000: store every step-th one
    DECT2_STAGE("t2.eqloop");
    size_t totalData = 0;
    for (int l = firstData; l < L; l++) totalData += dsCache.dataIdx[(size_t)l].size();
    const size_t dispStep = std::max<size_t>(1, totalData / 6000);
    size_t dispIdx = 0;
    H.resize(K);
    std::vector<cf32> eqv((size_t)K);
    std::vector<float> n0v((size_t)K);
    std::vector<cf32> ceE; std::vector<float> ceN;
    for (int l = firstData; l < L; l++) {
        const float* ha; const float* hb; float dl;
        if (nSeg > 1) {
            const int sg = segOf(l);
            dl = (float)(l - segC[(size_t)sg]);
            ha = reinterpret_cast<const float*>(segMid[(size_t)sg].data()); hb = reinterpret_cast<const float*>(segSlope[(size_t)sg].data());
        } else {
            dl = (float)(l - lc);
            ha = reinterpret_cast<const float*>(Hmid.data()); hb = reinterpret_cast<const float*>(Hslope.data());
        }
        axpy(ha, hb, dl, reinterpret_cast<float*>(H.data()), 2 * K);
        const std::vector<int>& di = dsCache.dataIdx[(size_t)l];
        const int cD = (int)di.size();
        {
            const size_t first = (dispStep - dispIdx % dispStep) % dispStep;   // the first data cell of this symbol that the display takes
            for (size_t jj = first; jj < (size_t)cD; jj += dispStep) { const int kk = di[jj]; all.push_back(frameCells[l][off + kk] / H[kk]); }
            dispIdx += (size_t)cD;
        }
        if (wantPlp) {
            // straight into the end of the PLP stream (every one of the cD places is written: Hi is a permutation), with the cached
            // interleaver sequence by reference: no three vectors (a third of a megabyte at 32K) allocated, zeroed and copied per symbol
            const std::vector<int>& Hi = freqInterleaverSeqRef(fftCode, cD, (l & 1) != 0);
            const size_t d0 = dstream.size();
            dstream.resize(d0 + (size_t)cD); dn0.resize(d0 + (size_t)cD);
            cf32* orig = dstream.data() + d0;
            float* on0 = dn0.data() + d0;
            if (erased(l)) {
                for (int jj = 0; jj < cD; jj++) { orig[Hi[(size_t)jj]] = cf32(0, 0); on0[Hi[(size_t)jj]] = 1e6f; }
            } else {
                equalise(reinterpret_cast<const float*>(&frameCells[l][off]), reinterpret_cast<const float*>(H.data()), noiseCar.data(), (float)symScale[(size_t)l],
                         reinterpret_cast<float*>(eqv.data()), n0v.data(), K);
                if (flatN0) { double m = 0; for (int jj = 0; jj < cD; jj++) m += n0v[(size_t)di[(size_t)jj]]; m /= std::max(1, cD); for (int jj = 0; jj < cD; jj++) n0v[(size_t)di[(size_t)jj]] = (float)m; }   // A/B: the mean for all
                if (carrierErase.enabled()) {   // only the data carriers (pilots have another power); gathered, tracked per carrier, scattered back
                    ceE.resize((size_t)cD); ceN.resize((size_t)cD);
                    for (int jj = 0; jj < cD; jj++) { ceE[(size_t)jj] = eqv[(size_t)di[(size_t)jj]]; ceN[(size_t)jj] = n0v[(size_t)di[(size_t)jj]]; }
                    carrierErase.apply(ceE.data(), ceN.data(), di.data(), cD, K);
                    for (int jj = 0; jj < cD; jj++) n0v[(size_t)di[(size_t)jj]] = ceN[(size_t)jj];
                }
                for (int jj = 0; jj < cD; jj++) { const int kk = di[(size_t)jj]; orig[Hi[(size_t)jj]] = eqv[(size_t)kk]; on0[Hi[(size_t)jj]] = n0v[(size_t)kk]; }
            }
        }
        if (l == firstData + (L - firstData) / 2) { chH = H; chK = K; } // keep a mid-frame channel snapshot for display
    }
    if (wantPlp) submitPlp(dstream, dn0, sigma2);
    eqData.clear();
    eqData = std::move(all);
    dataValid = true;
    dataDx = dx; dataDy = dy; dataPp = pc.pp;
    dataFrames++;
    // refresh channel-derived displays from the data-symbol estimate
    chMag.clear(); chPh.clear();
    int dec = std::max(1, K / 4096);
    for (int i = 0; i < K; i += dec) {
        chMag.push_back(20 * std::log10(std::max(1e-9f, std::abs(chH[i]))));
        chPh.push_back(std::arg(chH[i]));
    }
    irMin = -std::max(8, G / 4);
    impulseResponse(chH, N, irMin, G + std::max(8, G / 4), back, irDb);
    snrDbv = dataSnrCar;
    p2Snr = dataSnr;
    (void)fm;
}

} // namespace dect2
