// TLS for the remote-control API server (headless mode): a thin layer over OpenSSL, compiled only when the build has it (DECT2_TLS). Without it available()
// is false and the server refuses a certificate in its config with a clear message (put a reverse proxy in front instead). Mostly server side (makeClientCtx / connect give the small HTTPS client of the OnAir Web directory heartbeat): one context
// from a certificate and key file, one Conn per accepted socket, used by the connection's own thread.
#pragma once
#include "netcompat.h"
#include <cstddef>
#include <string>

namespace dect2::tls {
struct Ctx;
struct Conn;
bool available();
Ctx* makeCtx(const std::string& certFile, const std::string& keyFile, std::string& err);   // TLS 1.2 and up, nullptr and err when a file does not load
void freeCtx(Ctx*);
Conn* accept(Ctx*, sock_t s, std::string& err);   // the handshake on a blocking socket (its receive timeout applies); nullptr when it fails
long read(Conn*, void* d, size_t n);               // bytes, or <= 0 (closed / error / timeout)
long write(Conn*, const void* d, size_t n);
bool pending(Conn*);                               // decrypted bytes already waiting (select() does not see them)
Ctx* makeClientCtx(std::string& err, bool insecure = false);   // verifies the server certificate against the system CA store (SSL_CERT_FILE / SSL_CERT_DIR also work); TLS 1.2 and up
Conn* connect(Ctx*, sock_t s, const std::string& host, std::string& err);   // client handshake on a connected blocking socket; host is checked against the certificate and sent as SNI
void close(Conn*);                                 // close_notify, then frees (the socket stays the caller's)
}
