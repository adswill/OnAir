// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/kris/kris_fec.h"
#include "dect2/rs/kris.h"

namespace dect2::kris {

int ldpcK(int n, LdpcRate r) { return onair_kris_ldpc_k(n, static_cast<int>(r)); }

bool ldpcEncode(int n, LdpcRate r, const uint8_t* info, uint8_t* codeword) { return onair_kris_ldpc_encode(n, static_cast<int>(r), info, codeword) == 0; }

int ldpcDecode(int n, LdpcRate r, const float* llr, uint8_t* infoBits, int maxIter) { return onair_kris_ldpc_decode(n, static_cast<int>(r), llr, infoBits, maxIter); }

static int outPerBit(int kind) { return kind == 0 ? 2 : kind == 1 ? 4 : 0; }

std::vector<uint8_t> convEncode(int kind, const std::vector<uint8_t>& bits, bool terminate) {
    const int no = outPerBit(kind);
    if (!no || bits.size() > 0x3fffffff) return {};
    std::vector<uint8_t> out((bits.size() + (terminate ? 6 : 0)) * no);
    const int w = onair_kris_conv_encode(kind, bits.data(), static_cast<int32_t>(bits.size()), terminate ? 1 : 0, out.data());
    if (w < 0) return {};
    out.resize(static_cast<size_t>(w));
    return out;
}

std::vector<uint8_t> viterbiDecode(int kind, const std::vector<float>& llr, bool terminated) {
    const int no = outPerBit(kind);
    if (!no) return {};
    const size_t nsym = llr.size() / no;
    if (nsym > 0x3fffffff || (terminated && nsym < 6)) return {};
    std::vector<uint8_t> out(nsym);
    const int w = onair_kris_viterbi(kind, llr.data(), static_cast<int32_t>(nsym), terminated ? 1 : 0, out.data());
    if (w < 0) return {};
    out.resize(static_cast<size_t>(w));
    return out;
}

}  // namespace dect2::kris
