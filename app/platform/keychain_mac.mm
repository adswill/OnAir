// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// macOS Keychain provider for the voice key store (keychain.h). Generic-password items, service "OnAir voice keys".
// Compiled only on Apple systems (CMake). Only the 32-byte master key is stored here, never a voice key.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include "keychain.h"
#include "dect2/voicekeys/key_persist.h"

namespace {

NSMutableDictionary* baseQuery(const std::string& account) {
    NSMutableDictionary* q = [NSMutableDictionary dictionary];
    q[(__bridge id)kSecClass] = (__bridge id)kSecClassGenericPassword;
    q[(__bridge id)kSecAttrService] = @"OnAir voice keys";
    q[(__bridge id)kSecAttrAccount] = [NSString stringWithUTF8String:account.c_str()];
    return q;
}

bool keychainGet(const std::string& account, std::vector<uint8_t>& out) {
    @autoreleasepool {
        NSMutableDictionary* q = baseQuery(account);
        q[(__bridge id)kSecReturnData] = @YES;
        q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
        CFTypeRef res = nullptr;
        const OSStatus st = SecItemCopyMatching((__bridge CFDictionaryRef)q, &res);
        if (st != errSecSuccess || !res) return false;
        NSData* d = (__bridge_transfer NSData*)res;
        out.assign((const uint8_t*)d.bytes, (const uint8_t*)d.bytes + d.length);
        return true;
    }
}

bool keychainSet(const std::string& account, const std::vector<uint8_t>& data) {
    @autoreleasepool {
        SecItemDelete((__bridge CFDictionaryRef)baseQuery(account));   // replace any older value
        NSMutableDictionary* q = baseQuery(account);
        q[(__bridge id)kSecValueData] = [NSData dataWithBytes:data.data() length:data.size()];
        return SecItemAdd((__bridge CFDictionaryRef)q, nullptr) == errSecSuccess;
    }
}

bool keychainDel(const std::string& account) {
    @autoreleasepool {
        const OSStatus st = SecItemDelete((__bridge CFDictionaryRef)baseQuery(account));
        return st == errSecSuccess || st == errSecItemNotFound;
    }
}

}  // namespace

namespace plat {

std::string installKeychainProvider() {
    dect2::voicekeys::setSecretProvider(keychainGet, keychainSet, keychainDel);
    return "macOS Keychain";
}

}  // namespace plat
