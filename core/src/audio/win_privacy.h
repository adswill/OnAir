#pragma once
// Windows 10/11 privacy switches for the camera and the microphone (Settings > Privacy & security). Switched off, they do not hide the
// devices: they are still listed, but opening one fails (WASAPI: E_ACCESSDENIED; DirectShow: an open or frame error). The registry says
// which switch is off, so the error can tell the user where to look instead of only "could not be started".
#ifdef _WIN32
#include <string>
#include <string_view>
#include <windows.h>

namespace dect2 {

// what = L"webcam" or L"microphone". True when access is off for the whole PC (HKLM), for this user, or for desktop programs (NonPackaged).
inline bool winPrivacyDenied(const wchar_t* what) {
    const std::wstring base = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\") + what;
    auto deny = [](HKEY root, const std::wstring& key) {
        std::wstring v(32, L'\0');
        DWORD bytes = (DWORD)(v.size() * sizeof(wchar_t));
        if (RegGetValueW(root, key.c_str(), L"Value", RRF_RT_REG_SZ, nullptr, v.data(), &bytes) != ERROR_SUCCESS) return false;
        return std::wstring_view(v.c_str()) == L"Deny";
    };
    return deny(HKEY_LOCAL_MACHINE, base) || deny(HKEY_CURRENT_USER, base) || deny(HKEY_CURRENT_USER, base + L"\\NonPackaged");
}

// The advice to add to an error, or "" when the switches are on.
inline std::string winPrivacyCameraHint() {
    if (!winPrivacyDenied(L"webcam")) return {};
    return "; Windows blocks the camera: Settings > Privacy & security > Camera, switch on \"Camera access\" and \"Let desktop apps access your camera\"";
}
inline std::string winPrivacyMicHint() {
    if (!winPrivacyDenied(L"microphone")) return {};
    return "; Windows blocks the microphone: Settings > Privacy & security > Microphone, switch on \"Microphone access\" and \"Let desktop apps access your microphone\"";
}

} // namespace dect2
#endif
