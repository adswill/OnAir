// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Windows provider for the voice key store (keychain.h): Credential Manager generic credentials, which Windows protects with DPAPI under
// the signed-in user's account. Compiled only on Windows (CMake). Only the 32-byte master key is stored, never a voice key.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include "keychain.h"
#include "dect2/voicekeys/key_persist.h"

namespace {

std::wstring target(const std::string& account) {
    const std::string full = "OnAir voice keys/" + account;
    const int n = MultiByteToWideChar(CP_UTF8, 0, full.c_str(), (int)full.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, full.c_str(), (int)full.size(), w.data(), n);
    return w;
}

bool credGet(const std::string& account, std::vector<uint8_t>& out) {
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target(account).c_str(), CRED_TYPE_GENERIC, 0, &c) || !c) return false;
    out.assign(c->CredentialBlob, c->CredentialBlob + c->CredentialBlobSize);
    SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return true;
}

bool credSet(const std::string& account, const std::vector<uint8_t>& data) {
    const std::wstring t = target(account);
    CREDENTIALW c = {};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = const_cast<LPWSTR>(t.c_str());
    c.CredentialBlobSize = (DWORD)data.size();
    c.CredentialBlob = const_cast<LPBYTE>(data.data());
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;   // kept for this user on this computer; never roams
    c.UserName = const_cast<LPWSTR>(L"OnAir");
    return CredWriteW(&c, 0) != FALSE;
}

bool credDel(const std::string& account) {
    if (CredDeleteW(target(account).c_str(), CRED_TYPE_GENERIC, 0)) return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

}  // namespace

namespace plat {

std::string installKeychainProvider() {
    dect2::voicekeys::setSecretProvider(credGet, credSet, credDel);
    return "Windows Credential Manager";
}

}  // namespace plat
#endif  // _WIN32
