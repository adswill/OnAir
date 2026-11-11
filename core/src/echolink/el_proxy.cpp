// EchoLink proxy protocol: see el_proxy.h. Layout after SvxLink's echolib EchoLinkProxy.cpp (GPL-2.0-or-later, Tobias Blomberg SM0SVX).
#include "dect2/echolink/el_proxy.h"
#include "dect2/winlink/b2msg.h"   // md5
#include <cctype>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace echolink {

std::vector<uint8_t> proxyAuthReply(const std::string& call, const std::string& password, const uint8_t nonce[kProxyNonceBytes]) {
    std::string pw = password.empty() ? std::string("PUBLIC") : password;
    for (char& c : pw) c = (char)std::toupper((unsigned char)c);
    std::string cs = call;
    for (char& c : cs) c = (char)std::toupper((unsigned char)c);
    std::vector<uint8_t> m(pw.begin(), pw.end());
    m.insert(m.end(), nonce, nonce + kProxyNonceBytes);
    uint8_t d[16];
    winlink::md5(m.data(), m.size(), d);
    std::vector<uint8_t> out(cs.begin(), cs.end());
    out.push_back('\n');
    out.insert(out.end(), d, d + 16);
    return out;
}

bool ipv4Bytes(const std::string& ip, uint8_t out[4]) {
    unsigned a[4];
    char tail = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u%c", &a[0], &a[1], &a[2], &a[3], &tail) != 4) return false;
    for (int i = 0; i < 4; i++) { if (a[i] > 255) return false; out[i] = (uint8_t)a[i]; }
    return true;
}

std::string ipv4Text(const uint8_t b[4]) {
    char s[20];
    std::snprintf(s, sizeof s, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return s;
}

std::vector<uint8_t> makeProxyBlock(ProxyType t, const std::string& ip, const uint8_t* data, size_t n) {
    std::vector<uint8_t> m(kProxyHeaderBytes + n);
    m[0] = (uint8_t)t;
    uint8_t a[4] = {0, 0, 0, 0};
    if (!ip.empty()) ipv4Bytes(ip, a);
    std::memcpy(&m[1], a, 4);
    for (int i = 0; i < 4; i++) m[5 + (size_t)i] = (uint8_t)((uint32_t)n >> (8 * i));
    if (n) std::memcpy(&m[kProxyHeaderBytes], data, n);
    return m;
}

void ProxyParser::reset() { buf_.clear(); pos_ = 0; haveNonce_ = failed_ = false; }

void ProxyParser::feed(const uint8_t* p, size_t n) {
    if (failed_ || !n) return;
    if (pos_ > 65536) { buf_.erase(buf_.begin(), buf_.begin() + (long)pos_); pos_ = 0; }
    buf_.insert(buf_.end(), p, p + n);
    if (!haveNonce_ && buf_.size() - pos_ >= (size_t)kProxyNonceBytes) {
        std::memcpy(nonce_, &buf_[pos_], kProxyNonceBytes);
        pos_ += kProxyNonceBytes;
        haveNonce_ = true;
    }
}

bool ProxyParser::next(ProxyBlock& out) {
    if (failed_ || !haveNonce_ || buf_.size() - pos_ < (size_t)kProxyHeaderBytes) return false;
    const uint8_t* h = &buf_[pos_];
    if (h[0] < 1 || h[0] > 7) { failed_ = true; return false; }
    uint32_t len = 0;
    for (int i = 0; i < 4; i++) len |= (uint32_t)h[5 + i] << (8 * i);
    if (len > kProxyMaxBlock) { failed_ = true; return false; }
    if (buf_.size() - pos_ < (size_t)kProxyHeaderBytes + len) return false;
    out.type = (ProxyType)h[0];
    out.ip = ipv4Text(h + 1);
    out.data.assign(h + kProxyHeaderBytes, h + kProxyHeaderBytes + len);
    pos_ += (size_t)kProxyHeaderBytes + len;
    return true;
}

const char* proxySystemText(uint8_t code) {
    switch (code) {
    case kProxyBadPassword: return "bad proxy password";
    case kProxyAccessDenied: return "access to the proxy denied";
    default: return "unknown proxy message";
    }
}

} // namespace echolink
} // namespace dect2
