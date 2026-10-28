// Internet TV: https on Windows through WinHTTP, which uses the system's own TLS (Schannel) and certificate store. Written from the
// documented WinHTTP API; needs a test on Windows (the CI build compiles and links it).
#ifdef _WIN32
#include "dect2/tv/iptv.h"
#include <windows.h>
#include <winhttp.h>
#include <chrono>

namespace dect2::iptv {
namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
class WinBody : public HttpBody {
public:
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    std::atomic<bool>* cancel = nullptr;
    ~WinBody() override { if (req) WinHttpCloseHandle(req); if (con) WinHttpCloseHandle(con); if (ses) WinHttpCloseHandle(ses); }
    long read(void* b, size_t n) override {
        if (cancel && cancel->load()) return -2;
        DWORD got = 0;
        if (!WinHttpReadData(req, b, (DWORD)n, &got)) return -1;
        return (long)got;   // 0 = end
    }
};
}

std::unique_ptr<HttpBody> httpOpenPlatform(const std::string& url, const HttpOptions& o, HttpResult& r, bool& handled) {
    handled = true;
    const std::wstring wu = wide(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[4096];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(wu.c_str(), 0, 0, &uc)) { r.error = "not a valid address"; return nullptr; }
    auto b = std::make_unique<WinBody>();
    b->cancel = o.cancel;
    b->ses = WinHttpOpen(wide(o.userAgent.empty() ? "OnAir" : o.userAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!b->ses) { r.error = "WinHTTP is not available"; return nullptr; }
    WinHttpSetTimeouts(b->ses, o.connectTimeoutMs, o.connectTimeoutMs, o.connectTimeoutMs, o.readTimeoutMs);
    b->con = WinHttpConnect(b->ses, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    if (!b->con) { r.error = "could not connect"; return nullptr; }
    std::wstring p(path, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) p += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    b->req = WinHttpOpenRequest(b->con, L"GET", p.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!b->req) { r.error = "could not start the request"; return nullptr; }
    if (o.insecureTls) {   // Advanced > Ignore certificate errors
        DWORD fl = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(b->req, WINHTTP_OPTION_SECURITY_FLAGS, &fl, sizeof fl);
    }
    std::string hdr = "Accept: */*\r\n";
    if (!o.referer.empty()) hdr += "Referer: " + o.referer + "\r\n";
    for (auto& h : o.headers) hdr += h.first + ": " + h.second + "\r\n";
    const std::wstring wh = wide(hdr);
    if (!WinHttpSendRequest(b->req, wh.c_str(), (DWORD)-1L, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(b->req, nullptr)) {
        const DWORD e = GetLastError();
        r.error = e == ERROR_WINHTTP_SECURE_FAILURE || e == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID || e == ERROR_WINHTTP_SECURE_INVALID_CA || e == ERROR_WINHTTP_SECURE_CERT_CN_INVALID
            ? "certificate problem: the server's certificate is not trusted (Advanced > Ignore certificate errors can be ticked for a stream you trust)"
            : e == ERROR_WINHTTP_TIMEOUT ? "the server did not answer in time" : e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "cannot find the address " + url : "could not reach the server (Windows error " + std::to_string((unsigned long)e) + ")";
        return nullptr;
    }
    DWORD st = 0, sz = sizeof st;
    WinHttpQueryHeaders(b->req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)st; r.finalUrl = url;
    wchar_t ct[256]; sz = sizeof ct;
    if (WinHttpQueryHeaders(b->req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct, &sz, WINHTTP_NO_HEADER_INDEX)) {
        std::wstring w(ct); r.contentType.assign(w.begin(), w.end());
    }
    if (st < 200 || st >= 300) {
        r.error = "HTTP " + std::to_string(st) + (st == 403 ? " Forbidden: the server refuses this request (try another User-Agent or Referer under Advanced, the link may have expired, or the stream is not available in your country)" : st == 404 ? " Not Found: the stream no longer exists at this address" : "");
        return nullptr;
    }
    return b;
}
} // namespace dect2::iptv
#endif
