// See tls_conn.h. OpenSSL is a system library (Apache-2.0 from 3.0), linked only when DECT2_TLS is set; no OpenSSL code is copied.
#include "tls_conn.h"
#ifdef DECT2_TLS
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
// wincrypt.h defines names that OpenSSL uses for its own types
#undef X509_NAME
#undef X509_EXTENSIONS
#undef X509_CERT_PAIR
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE
#endif
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <cstdlib>
#include <sys/stat.h>

namespace dect2::tls {
struct Ctx { SSL_CTX* c = nullptr; };
struct Conn { SSL* s = nullptr; };

static std::string sslErr() {
    char b[256];
    const unsigned long e = ERR_get_error();
    if (!e) return "unknown error";
    ERR_error_string_n(e, b, sizeof b);
    return b;
}
bool available() { return true; }
Ctx* makeCtx(const std::string& cert, const std::string& key, std::string& err) {
    SSL_CTX* c = SSL_CTX_new(TLS_server_method());
    if (!c) { err = "TLS: " + sslErr(); return nullptr; }
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_chain_file(c, cert.c_str()) != 1) { err = "TLS certificate \"" + cert + "\" cannot be read: " + sslErr(); SSL_CTX_free(c); return nullptr; }
    if (SSL_CTX_use_PrivateKey_file(c, key.c_str(), SSL_FILETYPE_PEM) != 1) { err = "TLS key \"" + key + "\" cannot be read: " + sslErr(); SSL_CTX_free(c); return nullptr; }
    if (SSL_CTX_check_private_key(c) != 1) { err = "TLS key does not belong to the certificate"; SSL_CTX_free(c); return nullptr; }
    return new Ctx{c};
}
void freeCtx(Ctx* c) { if (c) { SSL_CTX_free(c->c); delete c; } }
Conn* accept(Ctx* c, sock_t s, std::string& err) {
    SSL* ssl = SSL_new(c->c);
    if (!ssl) { err = sslErr(); return nullptr; }
    SSL_set_fd(ssl, (int)s);
    if (SSL_accept(ssl) != 1) { err = sslErr(); SSL_free(ssl); return nullptr; }
    return new Conn{ssl};
}
// Where the certificate authorities (the "trusted roots") come from, so that the HTTPS check works out of the box on every system:
//  - SSL_CERT_FILE / SSL_CERT_DIR when the user set them (OpenSSL's own default paths),
//  - Windows: the system "Root" certificate store (the one Edge and Chrome use), read through crypt32,
//  - macOS: /etc/ssl/cert.pem, which macOS itself keeps up to date (a Homebrew or bundled OpenSSL has no store of its own),
//  - Linux: the bundle of the distribution (Debian/Ubuntu, Fedora/RHEL, openSUSE, Alpine, Arch). A bundled libssl looks only in one default place.
static int loadSystemRoots(SSL_CTX* c) {
    int n = 0;
    X509_STORE* st = SSL_CTX_get_cert_store(c);
#ifdef _WIN32
    if (HCERTSTORE h = CertOpenSystemStoreA(0, "ROOT")) {
        for (PCCERT_CONTEXT cc = CertEnumCertificatesInStore(h, nullptr); cc; cc = CertEnumCertificatesInStore(h, cc)) {
            const unsigned char* p = cc->pbCertEncoded;
            if (X509* x = d2i_X509(nullptr, &p, (long)cc->cbCertEncoded)) { if (X509_STORE_add_cert(st, x) == 1) ++n; X509_free(x); }
        }
        CertCloseStore(h, 0);
    }
#else
    static const char* const files[] = {"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/ca-bundle.pem", "/etc/ssl/certs/ca-bundle.crt",
                                        "/opt/homebrew/etc/ca-certificates/cert.pem", "/usr/local/etc/ca-certificates/cert.pem"};
    for (const char* f : files) {
        struct stat sb;
        if (stat(f, &sb) == 0 && S_ISREG(sb.st_mode) && X509_STORE_load_file(st, f) == 1) { ++n; break; }
    }
#endif
    return n;
}
Ctx* makeClientCtx(std::string& err, bool insecure) {
    SSL_CTX* c = SSL_CTX_new(TLS_client_method());
    if (!c) { err = "TLS: " + sslErr(); return nullptr; }
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    SSL_CTX_set_verify(c, insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, nullptr);   // insecure: only when the user ticked the Advanced box for one stream
    if (insecure) return new Ctx{c};
    int roots = 0;
    if (std::getenv("SSL_CERT_FILE") || std::getenv("SSL_CERT_DIR")) { if (SSL_CTX_set_default_verify_paths(c) == 1) ++roots; }   // the user chose the store
    if (!roots) { roots = loadSystemRoots(c); if (!roots && SSL_CTX_set_default_verify_paths(c) == 1) ++roots; }
    if (!roots) { err = "TLS: no trusted certificate authorities found on this computer (set SSL_CERT_FILE to a CA bundle .pem file)"; SSL_CTX_free(c); return nullptr; }
    return new Ctx{c};
}
Conn* connect(Ctx* c, sock_t s, const std::string& host, std::string& err) {
    SSL* ssl = SSL_new(c->c);
    if (!ssl) { err = sslErr(); return nullptr; }
    SSL_set_fd(ssl, (int)s);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_CTX_get_verify_mode(c->c) != SSL_VERIFY_NONE) SSL_set1_host(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) { { long vr = SSL_get_verify_result(ssl); err = vr != X509_V_OK ? std::string("certificate problem: ") + X509_verify_cert_error_string(vr) : "TLS handshake failed: " + sslErr(); } SSL_free(ssl); return nullptr; }
    return new Conn{ssl};
}
long read(Conn* c, void* d, size_t n) { return SSL_read(c->s, d, (int)n); }
long write(Conn* c, const void* d, size_t n) { return SSL_write(c->s, d, (int)n); }
bool pending(Conn* c) { return SSL_pending(c->s) > 0; }
void close(Conn* c) { if (!c) return; SSL_shutdown(c->s); SSL_free(c->s); delete c; }
}
#else
namespace dect2::tls {
bool available() { return false; }
Ctx* makeCtx(const std::string&, const std::string&, std::string& err) { err = "this build has no TLS support (built without OpenSSL): use a reverse proxy such as Caddy or nginx in front of the server"; return nullptr; }
void freeCtx(Ctx*) {}
Conn* accept(Ctx*, sock_t, std::string& err) { err = "no TLS"; return nullptr; }
Ctx* makeClientCtx(std::string& err, bool) { err = "this build has no TLS support (built without OpenSSL)"; return nullptr; }
Conn* connect(Ctx*, sock_t, const std::string&, std::string& err) { err = "no TLS"; return nullptr; }
long read(Conn*, void*, size_t) { return -1; }
long write(Conn*, const void*, size_t) { return -1; }
bool pending(Conn*) { return false; }
void close(Conn*) {}
}
#endif
