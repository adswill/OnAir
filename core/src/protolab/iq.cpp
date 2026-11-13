// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab: reading IQ files, finding bursts, demodulating one burst.
#include "dect2/protolab/protolab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace dect2::protolab {

const char* demodName(Demod d) {
    switch (d) {
        case Demod::Auto: return "Auto";
        case Demod::OOK: return "OOK/ASK";
        case Demod::FSK2: return "2FSK/GFSK";
        case Demod::FSK4: return "4FSK";
        case Demod::PSK2: return "BPSK";
        case Demod::PSK4: return "QPSK";
        case Demod::FM: return "FM";
        case Demod::AM: return "AM";
    }
    return "?";
}

bool loadIq(const std::string& path, IqFormat fmt, std::vector<cf>& out, std::string& err, size_t maxSamples) {
    out.clear();
    std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!f) { err = "cannot open the file"; return false; }
    const size_t bytesPer = fmt == IqFormat::Cf32 ? 8 : fmt == IqFormat::Cs16 ? 4 : 2;
    std::vector<char> buf(bytesPer * 65536);
    while (out.size() < maxSamples) {
        f.read(buf.data(), (std::streamsize)buf.size());
        const size_t n = (size_t)f.gcount() / bytesPer;
        if (n == 0) break;
        for (size_t i = 0; i < n && out.size() < maxSamples; i++) {
            const char* p = buf.data() + i * bytesPer;
            switch (fmt) {
                case IqFormat::Cf32: { float v[2]; std::copy(p, p + 8, reinterpret_cast<char*>(v)); out.emplace_back(v[0], v[1]); break; }
                case IqFormat::Cs16: { int16_t v[2]; std::copy(p, p + 4, reinterpret_cast<char*>(v)); out.emplace_back(v[0] / 32768.f, v[1] / 32768.f); break; }
                case IqFormat::Cu8: out.emplace_back(((uint8_t)p[0] - 127.5f) / 127.5f, ((uint8_t)p[1] - 127.5f) / 127.5f); break;
                case IqFormat::Cs8: out.emplace_back((int8_t)p[0] / 128.f, (int8_t)p[1] / 128.f); break;
            }
        }
        if (f.eof()) break;
    }
    if (out.empty()) { err = "the file holds no samples"; return false; }
    return true;
}

std::vector<cf> channelise(const std::vector<cf>& iq, double fs, double offsetHz, double bandwidthHz, double& outRate) {
    const double pi = 3.14159265358979323846;
    outRate = fs;
    if (iq.empty() || fs <= 0) return {};
    const double target = std::max(40000.0, 4.0 * std::max(bandwidthHz, 1.0));
    const size_t D = (size_t)std::max(1.0, std::floor(fs / target));
    std::vector<cf> x(iq.size());
    if (offsetHz != 0) {   // mixer: a rotating phasor, renormalised now and then
        std::complex<double> ph = 1, step = std::polar(1.0, -2 * pi * offsetHz / fs);
        for (size_t i = 0; i < iq.size(); i++) {
            x[i] = cf((float)(iq[i].real() * ph.real() - iq[i].imag() * ph.imag()), (float)(iq[i].real() * ph.imag() + iq[i].imag() * ph.real()));
            ph *= step;
            if ((i & 1023) == 1023) ph /= std::abs(ph);
        }
    } else x = iq;
    if (D > 1) {
        for (int stage = 0; stage < 3; stage++) {
            std::vector<cf> y(x.size());
            std::complex<double> acc = 0;
            for (size_t i = 0; i < x.size(); i++) {
                acc += std::complex<double>(x[i]);
                if (i >= D) acc -= std::complex<double>(x[i - D]);
                y[i] = cf((float)(acc.real() / (double)D), (float)(acc.imag() / (double)D));
            }
            x.swap(y);
        }
        std::vector<cf> o;
        o.reserve(x.size() / D + 1);
        for (size_t i = D; i < x.size(); i += D) o.push_back(x[i]);
        x.swap(o);
    }
    outRate = fs / (double)D;
    return x;
}

namespace {
float pct(std::vector<float> v, double p) {
    if (v.empty()) return 0;
    size_t k = (size_t)(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}
}   // namespace

std::vector<Burst> findBursts(const std::vector<cf>& iq, double fs, const BurstParams& p) {
    std::vector<Burst> out;
    const size_t n = iq.size();
    if (n < 16 || fs <= 0) return out;
    const size_t w = std::max<size_t>(1, (size_t)(fs * 0.0001));
    std::vector<float> env(n);
    {   // moving average of |x|
        double acc = 0;
        std::vector<float> mag(n);
        for (size_t i = 0; i < n; i++) mag[i] = std::abs(iq[i]);
        for (size_t i = 0; i < n; i++) {
            acc += mag[i];
            if (i >= w) acc -= mag[i - w];
            env[i] = (float)(acc / (double)std::min(i + 1, w));
        }
    }
    std::vector<float> sub;
    const size_t step = std::max<size_t>(1, n / 200000);
    for (size_t i = 0; i < n; i += step) sub.push_back(env[i]);
    const float noise = pct(sub, 0.30), high = pct(sub, 0.995);
    if (!(high > noise * 1.6f)) return out;   // nothing stands out of the noise
    const float thr = noise + (float)p.threshold * (high - noise);
    const size_t gap = (size_t)(p.minGapSec * fs), minLen = (size_t)(p.minLenSec * fs);
    bool in = false;
    size_t st = 0, last = 0;
    for (size_t i = 0; i < n; i++) {
        if (env[i] >= thr) {
            if (!in) { in = true; st = i; }
            last = i;
        } else if (in && i - last > gap) {
            in = false;
            if (last + 1 - st >= minLen) out.push_back({st > w ? st - w / 2 : st, std::min(n, last + 1 - w / 2)});
        }
    }
    if (in && last + 1 - st >= minLen) out.push_back({st, last + 1});
    return out;
}

namespace {
float midrange(const std::vector<float>& v) {
    if (v.empty()) return 0;
    return 0.5f * (pct(v, 0.05) + pct(v, 0.95));
}
}   // namespace

Soft demodulate(const cf* x, size_t n, Demod d, double fs) {
    Soft s;
    if (n < 4) return s;
    const double pi = 3.14159265358979323846;
    switch (d) {
        case Demod::OOK:
        case Demod::AM:
        case Demod::Auto: {
            s.a.resize(n);
            for (size_t i = 0; i < n; i++) s.a[i] = std::abs(x[i]);
            if (d == Demod::AM) {   // remove the carrier level
                double m = 0;
                for (float v : s.a) m += v;
                m /= (double)n;
                for (float& v : s.a) v -= (float)m;
            }
            break;
        }
        case Demod::FM:
        case Demod::FSK2:
        case Demod::FSK4: {
            // mean frequency out, then a low-pass sized from the deviation: the discriminator turns the noise of the whole input
            // bandwidth into noise on the data, so cutting the bandwidth to the signal is worth many dB
            std::vector<cf> y(x, x + n);
            if (d != Demod::FM) {
                std::complex<double> acc = 0;
                for (size_t i = 1; i < n; i++) acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i - 1]));
                const double w0 = std::arg(acc);
                for (size_t i = 0; i < n; i++) {
                    const std::complex<double> r = std::polar(1.0, -w0 * (double)i);
                    y[i] = cf((float)(x[i].real() * r.real() - x[i].imag() * r.imag()), (float)(x[i].real() * r.imag() + x[i].imag() * r.real()));
                }
                std::vector<float> z;
                for (size_t i = 4; i < n; i++) z.push_back((float)(std::arg(y[i] * std::conj(y[i - 4])) * fs / (2 * pi * 4)));
                const double dev = std::max(300.0, 0.5 * (pct(z, 0.95) - pct(z, 0.05)));
                const size_t M = (size_t)std::clamp(std::lround(0.443 * fs / (3.0 * dev)), 1L, 64L);
                if (M > 1) {
                    std::vector<cf> f(n);
                    std::complex<double> sum = 0;
                    const size_t h = M / 2;
                    for (size_t t = 0; t < M; t++) sum += std::complex<double>(y[t > h ? std::min(n - 1, t - h) : 0]);   // the window of sample 0, the first sample repeated before the start
                    for (size_t i = 0; i < n; i++) {
                        f[i] = cf((float)(sum.real() / (double)M), (float)(sum.imag() / (double)M));
                        const size_t add = i + M - h, sub = i >= h ? i - h : 0;
                        sum += std::complex<double>(y[std::min(n - 1, add)]) - std::complex<double>(y[sub]);
                    }
                    y.swap(f);
                }
            }
            std::vector<float> f(n);
            for (size_t i = 1; i < n; i++) f[i] = (float)(std::arg(y[i] * std::conj(y[i - 1])) * fs / (2 * pi));
            f[0] = f[1];
            const float mid = midrange(f);
            for (float& v : f) v -= mid;
            s.a.resize(n);
            for (size_t i = 0; i < n; i++) {   // three-tap smoothing against single-sample noise
                const float p0 = f[i > 0 ? i - 1 : 0], p2 = f[i + 1 < n ? i + 1 : n - 1];
                s.a[i] = 0.25f * p0 + 0.5f * f[i] + 0.25f * p2;
            }
            break;
        }
        case Demod::PSK2:
        case Demod::PSK4: {
            const int m = d == Demod::PSK2 ? 2 : 4;
            // carrier from the m-th power: the modulation vanishes, only the offset is left
            std::complex<double> acc = 0;
            for (size_t i = 1; i < n; i++) {
                std::complex<double> a = std::pow(std::complex<double>(x[i]), m), b = std::pow(std::complex<double>(x[i - 1]), m);
                const double na = std::abs(a), nb = std::abs(b);
                if (na > 0 && nb > 0) acc += (a / na) * std::conj(b / nb);
            }
            const double w = std::arg(acc) / m;
            std::vector<cf> z(n);
            std::complex<double> sum = 0;
            for (size_t i = 0; i < n; i++) {
                const std::complex<double> r = std::polar(1.0, -w * (double)i);
                z[i] = cf((float)(x[i].real() * r.real() - x[i].imag() * r.imag()), (float)(x[i].real() * r.imag() + x[i].imag() * r.real()));
                sum += std::pow(std::complex<double>(z[i]), m) / (std::abs(z[i]) + 1e-9);
            }
            double ph = std::arg(sum) / m;
            if (m == 4) ph -= pi / 4;   // put the four points on the diagonals so I and Q are independent bits
            double freq = 0;
            const double alpha = 0.03, beta = alpha * alpha / 4;
            s.a.resize(n);
            if (m == 4) s.b.resize(n);
            for (size_t i = 0; i < n; i++) {
                const std::complex<double> r = std::polar(1.0, -ph);
                const double re = z[i].real() * r.real() - z[i].imag() * r.imag(), im = z[i].real() * r.imag() + z[i].imag() * r.real();
                s.a[i] = (float)re;
                if (m == 4) s.b[i] = (float)im;
                const double mag2 = re * re + im * im + 1e-12;
                double err = m == 2 ? re * im / mag2 : ((re > 0 ? 1 : -1) * im - (im > 0 ? 1 : -1) * re) / std::sqrt(mag2);
                err = std::clamp(err, -1.0, 1.0);
                freq += beta * err;
                ph += freq + alpha * err;
            }
            break;
        }
    }
    return s;
}

}   // namespace dect2::protolab
