// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/es_save.h"

namespace dect2 {

bool EsSink::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    if (f_) { fclose(f_); f_ = nullptr; }
    f_ = fopen(path.c_str(), "wb");
    n_ = 0; path_ = f_ ? path : std::string();
    return f_ != nullptr;
}
void EsSink::close() { std::lock_guard<std::mutex> lk(mu_); if (f_) { fclose(f_); f_ = nullptr; } }
bool EsSink::isOpen() const { std::lock_guard<std::mutex> lk(mu_); return f_ != nullptr; }
void EsSink::write(const uint8_t* p, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!f_ || !n) return;
    n_ += fwrite(p, 1, n, f_);
}
uint64_t EsSink::bytes() const { std::lock_guard<std::mutex> lk(mu_); return n_; }
std::string EsSink::path() const { std::lock_guard<std::mutex> lk(mu_); return path_; }

void TsEsExtractor::feed(const uint8_t* pk, EsSink& sink) {
    if (pid_ < 0 || pk[0] != 0x47 || (pk[1] & 0x80)) return;
    if ((((pk[1] & 0x1F) << 8) | pk[2]) != pid_) return;
    const int afc = (pk[3] >> 4) & 3;
    if (!(afc & 1)) return;
    int off = 4;
    if (afc & 2) off += 1 + pk[4];
    if (off >= 188) return;
    const uint8_t* d = pk + off;
    int n = 188 - off;
    if (pk[1] & 0x40) { inPes_ = true; hdrN_ = 0; skip_ = -1; }   // a new PES packet: read its header first
    if (!inPes_) return;
    while (skip_ < 0 && n > 0) {                                   // the 9 fixed bytes, then the optional header
        hdr_[hdrN_++] = *d++; n--;
        if (hdrN_ == 9) {
            if (hdr_[0] || hdr_[1] || hdr_[2] != 1) { inPes_ = false; return; }
            skip_ = hdr_[8];
        }
    }
    if (skip_ < 0) return;
    const int s = skip_ < n ? skip_ : n;
    skip_ -= s; d += s; n -= s;
    if (n > 0) sink.write(d, (size_t)n);
}

const char* esExtension(const std::string& codec) { return codec == "DRA" ? "dra" : "mp2"; }

} // namespace dect2
