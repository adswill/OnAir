// LZHUF (B2 framing) for Winlink. Ported from wl2k-go lzhuf (Copyright 2016 Martin Hebnes Pedersen LA5NTA, MIT licence), a port of
// lzhuf.c of JNOS 2 (LZSS by Haruhiko Okumura, adaptive Huffman coding by Haruyasu Yoshizaki, edited by Kenji Rikitake; "use, distribute,
// and modify this program freely"). The order of tree updates follows wl2k-go so that the output matches it byte for byte.
//
// Parts ported from wl2k-go (https://github.com/la5nta/wl2k-go), under this notice:
//
// The MIT License (MIT)
// Copyright (c) 2014-2017 Martin Hebnes Pedersen (LA5NTA)
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#include "dect2/winlink/lzhuf.h"

#include <array>
#include <memory>

namespace dect2 {
namespace winlink {

namespace {

constexpr int N = 2048;                 // window
constexpr int F = 60;                   // lookahead
constexpr int THRESHOLD = 2;
constexpr int NIL = N;
constexpr int NUM_CHAR = 256 - THRESHOLD + F;
constexpr int T = NUM_CHAR * 2 - 1;
constexpr int R = T - 1;
constexpr unsigned MAX_FREQ = 0x8000;

const uint8_t kPCode[64] = {
    0x00, 0x20, 0x30, 0x40, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x94, 0x98, 0x9C,
    0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC, 0xC0, 0xC2, 0xC4, 0xC6, 0xC8, 0xCA, 0xCC, 0xCE,
    0xD0, 0xD2, 0xD4, 0xD6, 0xD8, 0xDA, 0xDC, 0xDE, 0xE0, 0xE2, 0xE4, 0xE6, 0xE8, 0xEA, 0xEC, 0xEE,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};
const uint8_t kPLen[64] = {
    3, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};

// the decode tables follow from the encode tables: dCode[b] = the index whose code is the top bits of b, dLen[b] = its length
struct DTables {
    uint8_t code[256], len[256];
    DTables() {
        for (int i = 0; i < 64; i++) {
            const int l = kPLen[i];
            const int span = 1 << (8 - l);
            for (int k = 0; k < span; k++) { code[kPCode[i] + k] = (uint8_t)i; len[kPCode[i] + k] = (uint8_t)l; }
        }
    }
};
const DTables& dt() { static const DTables t; return t; }

const std::array<uint16_t, 256>& crcTab() {
    static const std::array<uint16_t, 256> t = [] {
        std::array<uint16_t, 256> a{};
        for (int i = 0; i < 256; i++) {
            uint16_t c = (uint16_t)(i << 8);
            for (int k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
            a[(size_t)i] = c;
        }
        return a;
    }();
    return t;
}

inline uint16_t crcStep(uint16_t sum, uint8_t c) { return (uint16_t)(((sum << 8) & 0xFF00) ^ crcTab()[(sum >> 8) & 0xFF] ^ c); }

struct Tree {
    unsigned freq[T + 1];
    int prnt[T + NUM_CHAR];
    int son[T];
    int dad[N + 1], lson[N + 1], rson[N + 257];
    uint8_t text[N + F - 1];
    int matchLength = 0, matchPosition = 0;

    Tree() {
        for (int i = 0; i < N + F - 1; i++) text[i] = 0;
        for (int i = 0; i < NUM_CHAR; i++) { freq[i] = 1; son[i] = i + T; prnt[i + T] = i; }
        for (int i = 0, j = NUM_CHAR; j <= R; i += 2, j++) {
            freq[j] = freq[i] + freq[i + 1];
            son[j] = i;
            prnt[i] = j; prnt[i + 1] = j;
        }
        freq[T] = 0xFFFF;
        prnt[R] = 0;
    }
    void initTree() {
        for (int i = N + 1; i <= N + 256; i++) rson[i] = NIL;
        for (int i = 0; i < N; i++) dad[i] = NIL;
    }
    void deleteNode(int p) {
        if (dad[p] == NIL) return;
        int q;
        if (rson[p] == NIL) q = lson[p];
        else if (lson[p] == NIL) q = rson[p];
        else {
            q = lson[p];
            if (rson[q] != NIL) {
                while (rson[q] != NIL) q = rson[q];
                rson[dad[q]] = lson[q];
                dad[lson[q]] = dad[q];
                lson[q] = lson[p];
                dad[lson[p]] = q;
            }
            rson[q] = rson[p];
            dad[rson[p]] = q;
        }
        dad[q] = dad[p];
        if (rson[dad[p]] == p) rson[dad[p]] = q; else lson[dad[p]] = q;
        dad[p] = NIL;
    }
    void insertNode(int r) {
        int cmp = 1;
        const uint8_t* key = text + r;
        int p = N + 1 + key[0];
        rson[r] = lson[r] = NIL;
        matchLength = 0;
        for (;;) {
            if (cmp >= 0) {
                if (rson[p] != NIL) p = rson[p];
                else { rson[p] = r; dad[r] = p; return; }
            } else {
                if (lson[p] != NIL) p = lson[p];
                else { lson[p] = r; dad[r] = p; return; }
            }
            int i;
            for (i = 1; i < F; i++) { cmp = (int)key[i] - (int)text[p + i]; if (cmp != 0) break; }
            if (i > THRESHOLD) {
                if (i > matchLength) {
                    matchPosition = ((r - p) & (N - 1)) - 1;
                    matchLength = i;
                    if (matchLength >= F) break;
                }
                if (i == matchLength) {
                    const int c = ((r - p) & (N - 1)) - 1;
                    if (c < matchPosition) matchPosition = c;
                }
            }
        }
        dad[r] = dad[p]; lson[r] = lson[p]; rson[r] = rson[p];
        dad[lson[p]] = r; dad[rson[p]] = r;
        if (rson[dad[p]] == p) rson[dad[p]] = r; else lson[dad[p]] = r;
        dad[p] = NIL;
    }
    void reconst() {
        int j = 0;
        for (int i = 0; i < T; i++)
            if (son[i] >= T) { freq[j] = (freq[i] + 1) / 2; son[j] = son[i]; j++; }
        for (int i = 0, jj = NUM_CHAR; jj < T; i += 2, jj++) {
            const unsigned f = freq[jj] = freq[i] + freq[i + 1];
            int k = jj;
            while (f < freq[k - 1]) k--;
            for (int m = jj; m > k; m--) { freq[m] = freq[m - 1]; son[m] = son[m - 1]; }
            freq[k] = f;
            son[k] = i;
        }
        for (int i = 0; i < T; i++) {
            const int k = son[i];
            if (k >= T) prnt[k] = i; else prnt[k] = prnt[k + 1] = i;
        }
    }
    void update(int c) {
        if (freq[R] == MAX_FREQ) reconst();
        c = prnt[c + T];
        for (;;) {
            freq[c]++;
            const unsigned k = freq[c];
            if (k <= freq[c + 1]) {
                if ((c = prnt[c]) == 0) break;
                continue;
            }
            int l = c + 1;
            while (k > freq[l + 1]) l++;
            freq[c] = freq[l];
            freq[l] = k;
            const int i = son[c];
            prnt[i] = l;
            if (i < T) prnt[i + 1] = l;
            const int jn = son[l];
            son[l] = i;
            prnt[jn] = c;
            if (jn < T) prnt[jn + 1] = c;
            son[c] = jn;
            if ((c = prnt[l]) == 0) break;
        }
    }
};

struct Encoder {
    std::unique_ptr<Tree> z = std::make_unique<Tree>();
    std::vector<uint8_t> out;
    uint32_t putbuf = 0;
    int putlen = 0;
    int len = 0, r = N - F, s = 0, lastMatchLength = 0;
    bool preFilled = false;

    Encoder() {
        z->initTree();
        for (int i = 0; i < r; i++) z->text[i] = ' ';
    }
    void putCode(int l, uint32_t c) {
        putbuf |= c >> putlen;
        putbuf &= 0xFFFF;
        putlen += l;
        if (putlen < 8) return;
        out.push_back((uint8_t)(putbuf >> 8));
        putlen -= 8;
        if (putlen >= 8) {
            out.push_back((uint8_t)putbuf);
            putlen -= 8;
            putbuf = (c << (l - putlen)) & 0xFFFF;
        } else putbuf = (putbuf << 8) & 0xFFFF;
    }
    void encodeChar(int c) {
        uint32_t i = 0;
        int j = 0;
        int k = z->prnt[c + T];
        for (;;) {
            i >>= 1;
            j++;
            if (k & 1) i += 0x8000;
            if ((k = z->prnt[k]) == R) break;
        }
        putCode(j, i);
        z->update(c);
    }
    void encodePosition(int c) {
        const int i = c >> 6;
        putCode(kPLen[i], (uint32_t)kPCode[i] << 8);
        putCode(6, (uint32_t)(c & 0x3F) << 10);
    }
    void encode() {
        if (len == 0) return;
        if (z->matchLength > len) z->matchLength = len;
        if (z->matchLength <= THRESHOLD) {
            z->matchLength = 1;
            encodeChar(z->text[r]);
        } else {
            encodeChar(255 - THRESHOLD + z->matchLength);
            encodePosition(z->matchPosition);
        }
        lastMatchLength = z->matchLength;
    }
    void advance(const uint8_t* c) {
        if (c) {
            z->text[s] = *c;
            if (s < F - 1) z->text[s + N] = *c;
            len++;
        }
        z->insertNode(r);
        lastMatchLength--;
        if (lastMatchLength == 0) encode();
        z->deleteNode(s);
        s = (s + 1) & (N - 1);
        r = (r + 1) & (N - 1);
        len--;
    }
    void write(const uint8_t* p, size_t n) {
        size_t i = 0;
        while (!preFilled && i < n) {
            z->text[r + len] = p[i++];
            len++;
            z->insertNode(r - len);
            lastMatchLength = 1;
            preFilled = len == F;
        }
        for (; i < n; i++) advance(&p[i]);
    }
    void close() {
        while (len > 0) advance(nullptr);
        encode();
        if (putlen != 0) out.push_back((uint8_t)(putbuf >> 8));
    }
};

struct BitReader {
    const uint8_t* d; size_t n, pos = 0;
    uint64_t acc = 0; int bits = 0;
    bool eof = false;
    int read(int k) {
        while (k > bits) {
            if (pos >= n) { eof = true; return 0; }
            acc = (acc << 8) | d[pos++];
            bits += 8;
        }
        const int v = (int)((acc >> (bits - k)) & ((1u << k) - 1));
        bits -= k;
        return v;
    }
};

} // namespace

uint16_t b2Crc16(const uint8_t* d, size_t n) {
    uint16_t s = 0;
    for (size_t i = 0; i < n; i++) s = crcStep(s, d[i]);
    s = crcStep(s, 0);
    s = crcStep(s, 0);
    return s;
}

std::vector<uint8_t> lzhufCompress(const uint8_t* d, size_t n, bool b2) {
    Encoder e;
    e.write(d, n);
    e.close();
    std::vector<uint8_t> body;
    body.reserve(e.out.size() + 4);
    const uint32_t sz = (uint32_t)n;
    for (int i = 0; i < 4; i++) body.push_back((uint8_t)(sz >> (8 * i)));
    body.insert(body.end(), e.out.begin(), e.out.end());
    if (!b2) return body;
    const uint16_t c = b2Crc16(body.data(), body.size());
    std::vector<uint8_t> v;
    v.reserve(body.size() + 2);
    v.push_back((uint8_t)(c & 0xFF));
    v.push_back((uint8_t)(c >> 8));
    v.insert(v.end(), body.begin(), body.end());
    return v;
}

bool lzhufDecompress(const uint8_t* d, size_t n, std::vector<uint8_t>& out, bool b2, std::string* err, size_t maxLen) {
    out.clear();
    auto fail = [&](const char* w) { if (err) *err = w; return false; };
    size_t p = 0;
    uint16_t crc = 0;
    if (b2) {
        if (n < 2) return fail("short header");
        crc = (uint16_t)(d[0] | (d[1] << 8));
        p = 2;
        if (b2Crc16(d + 2, n - 2) != crc) return fail("CRC mismatch");
    }
    if (n < p + 4) return fail("short header");
    const uint32_t size = (uint32_t)d[p] | ((uint32_t)d[p + 1] << 8) | ((uint32_t)d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24);
    p += 4;
    if (size > maxLen) return fail("implausible length");
    auto z = std::make_unique<Tree>();
    for (int i = 0; i < N - F; i++) z->text[i] = ' ';
    int r = N - F;
    BitReader br{d + p, n - p};
    const DTables& t = dt();
    out.reserve(size);
    while (out.size() < size) {
        int c = z->son[R];
        while (c < T) { c += br.read(1); c = z->son[c]; }
        c -= T;
        z->update(c);
        if (br.eof) return fail("unexpected end of data");
        if (c < 256) {
            out.push_back((uint8_t)c);
            z->text[r] = (uint8_t)c;
            r = (r + 1) & (N - 1);
            continue;
        }
        int i = br.read(8);
        const int hi = (int)t.code[i] << 6;
        int j = t.len[i] - 2;
        for (; j > 0; j--) i = (i << 1) + br.read(1);
        const int pos = hi | (i & 0x3F);
        if (br.eof) return fail("unexpected end of data");
        const int start = (r - pos - 1) & (N - 1);
        const int cnt = c - 255 + THRESHOLD;
        for (int k = 0; k < cnt && out.size() < size; k++) {
            const uint8_t b = z->text[(start + k) & (N - 1)];
            out.push_back(b);
            z->text[r] = b;
            r = (r + 1) & (N - 1);
        }
    }
    return true;
}

} // namespace winlink
} // namespace dect2
