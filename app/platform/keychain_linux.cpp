// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Linux provider for the voice key store (keychain.h): the Secret Service through libsecret, loaded with dlopen at run time so OnAir
// neither links nor requires it. When libsecret is missing, installKeychainProvider() returns "" and the passphrase fallback is used.
// Compiled on every system that is not Apple or Windows; the real code is only active on Linux. Only the 32-byte master key is stored.
#include "keychain.h"
#include "dect2/voicekeys/key_persist.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <cstring>
#include <vector>

namespace {

// libsecret's SecretSchema, laid out as in libsecret-1 (secret-schema.h), so no header is needed.
struct SchemaAttr { const char* name; int type; };
struct Schema {
    const char* name;
    int flags;
    SchemaAttr attributes[32];
    int reserved;
    void* r1; void* r2; void* r3; void* r4; void* r5; void* r6; void* r7;
};
Schema gSchema = {"dev.on-air.voicekeys", 0 /* SECRET_SCHEMA_NONE */, {{"account", 0 /* SECRET_SCHEMA_ATTRIBUTE_STRING */}, {nullptr, 0}}, 0,
                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

using StoreFn = int (*)(const Schema*, const char* collection, const char* label, const char* password, void* cancellable, void** error, ...);
using LookupFn = char* (*)(const Schema*, void* cancellable, void** error, ...);
using ClearFn = int (*)(const Schema*, void* cancellable, void** error, ...);
using FreeFn = void (*)(char*);
using ErrFreeFn = void (*)(void*);

struct Lib {
    void* h = nullptr;
    StoreFn store = nullptr; LookupFn lookup = nullptr; ClearFn clear = nullptr; FreeFn pfree = nullptr;
    void* glib = nullptr; ErrFreeFn errFree = nullptr;
} L;

bool load() {
    if (L.h) return true;
    void* h = dlopen("libsecret-1.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return false;
    L.store = (StoreFn)dlsym(h, "secret_password_store_sync");
    L.lookup = (LookupFn)dlsym(h, "secret_password_lookup_sync");
    L.clear = (ClearFn)dlsym(h, "secret_password_clear_sync");
    L.pfree = (FreeFn)dlsym(h, "secret_password_free");
    L.errFree = (ErrFreeFn)dlsym(RTLD_DEFAULT, "g_error_free");   // libsecret pulls in glib
    if (!L.store || !L.lookup || !L.clear || !L.pfree) { dlclose(h); return false; }
    L.h = h;
    return true;
}

void freeErr(void* e) { if (e && L.errFree) L.errFree(e); }

std::string toHex(const std::vector<uint8_t>& d) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) { s += x[b >> 4]; s += x[b & 15]; }
    return s;
}
bool fromHex(const char* s, std::vector<uint8_t>& out) {
    out.clear();
    const size_t n = std::strlen(s);
    if (n % 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < n; i += 2) {
        const int hi = v(s[i]), lo = v(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)(hi * 16 + lo));
    }
    return true;
}

bool secGet(const std::string& account, std::vector<uint8_t>& out) {
    if (!load()) return false;
    void* err = nullptr;
    char* pw = L.lookup(&gSchema, nullptr, &err, "account", account.c_str(), nullptr);
    freeErr(err);
    if (!pw) return false;
    const bool ok = fromHex(pw, out);
    std::memset(pw, 0, std::strlen(pw));
    L.pfree(pw);
    return ok;
}

bool secSet(const std::string& account, const std::vector<uint8_t>& data) {
    if (!load()) return false;
    std::string hex = toHex(data);
    void* err = nullptr;
    const int ok = L.store(&gSchema, "default", "OnAir voice keys", hex.c_str(), nullptr, &err, "account", account.c_str(), nullptr);
    freeErr(err);
    std::fill(hex.begin(), hex.end(), '\0');
    return ok != 0;
}

bool secDel(const std::string& account) {
    if (!load()) return false;
    void* err = nullptr;
    L.clear(&gSchema, nullptr, &err, "account", account.c_str(), nullptr);
    const bool ok = err == nullptr;
    freeErr(err);
    return ok;
}

}  // namespace

namespace plat {

std::string installKeychainProvider() {
    if (!load()) return std::string();
    dect2::voicekeys::setSecretProvider(secGet, secSet, secDel);
    return "Secret Service (libsecret)";
}

}  // namespace plat

#elif !defined(__APPLE__) && !defined(_WIN32)

namespace plat {
std::string installKeychainProvider() { return std::string(); }   // no keychain on this system: the passphrase fallback applies
}

#endif
