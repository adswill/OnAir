// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/voice/codec_setup.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/codec/speech_codec.h"
#include <cctype>

namespace dect2::voice {

SetupOs currentSetupOs() {
#if defined(_WIN32)
    return SetupOs::Windows;
#elif defined(__APPLE__)
    return SetupOs::MacOS;
#else
    return SetupOs::Linux;
#endif
}

const char* setupOsName(SetupOs os) {
    switch (os) {
    case SetupOs::Windows: return "Windows";
    case SetupOs::MacOS: return "macOS";
    default: return "Linux";
    }
}

namespace {
std::string lowerS(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
}

// How a plug-in gets made: the build helper (tools/codec-builder) only knows these ids.
bool setupHelperBuilds(const std::string& id) { return id == "tetra" || id == "ambe2" || id == "ambe" || id == "imbe" || id == "ysf"; }

std::string setupHowToGet(const std::string& id) {
    if (setupHelperBuilds(id)) return "build it with the build helper (type " + id + " or all)";
    if (id == "aeroambe") return "the build helper does not make it: build tools/addons/aeroambe against libaeroambe's mbelib (docs/CODECS.md, AMBE (Inmarsat Aero voice))";
    if (id == "iridium" || id == "tetrapol") return "the build helper does not make it and OnAir has no source for it: you need a plug-in of your own with the four onair_speech_* functions (docs/CODECS.md), or a file somebody gave you";
    return "see docs/CODECS.md";
}

std::vector<SetupItem> codecSetupStatus(const std::vector<codec::CodecDir>& dirs, const SetupProbe& p) {
    std::vector<SetupItem> out;
    for (const auto& ci : codec::codecTable()) {
        if (ci.kind != codec::CodecKind::Patented || !ci.id || !*ci.id) continue;
        SetupItem it;
        it.id = ci.id; it.name = ci.name; it.modes = ci.modes;
        it.fileName = codec::speechCodecFileName(it.id);
        std::string path;
        std::vector<codec::CodecDir> hits;
        if (p.fileExists) hits = codec::dirsWithFile(dirs, it.fileName, p.fileExists);
        for (size_t i = 1; i < hits.size(); ++i) it.ignoredCopies.push_back(hits[i].path);
        if (p.loaded && p.loaded(it.id, &path)) {
            it.state = SetupState::Loaded; it.where = path;
            it.detail = "installed and working";
        } else if (!hits.empty()) {
            it.where = hits[0].path;
            const std::string arch = p.archOfFile ? p.archOfFile(codec::joinPath(hits[0].path, it.fileName)) : std::string();
            if (!arch.empty() && !p.processArch.empty() && arch.find(p.processArch) == std::string::npos) {
                it.state = SetupState::WrongArch;
                it.detail = "built for " + arch + " but OnAir runs as " + p.processArch + ": build it again on this computer";
            } else {
                it.state = SetupState::Ready;
                it.detail = "installed; it loads when you open a voice mode that uses it (accept the notice once)";
            }
        } else {
            it.state = SetupState::Missing;
            std::string list;
            for (const auto& d : dirs) list += (list.empty() ? "" : ", ") + d.path;
            it.detail = "not installed: OnAir looked for exactly " + it.fileName + " in " + (list.empty() ? std::string("(no folder)") : list) + "; " + setupHowToGet(it.id);
            // a file that is nearly right (Windows hides extensions: "x.dll.dll", a browser adds " (1)", the dll is in a sub-folder name...)
            if (p.listDir) {
                const std::string stem = "onair-codec-" + it.id;
                for (const auto& d : dirs) {
                    for (const auto& f : p.listDir(d.path)) {
                        const std::string lf = lowerS(f);
                        if (lf.find(lowerS(it.id)) == std::string::npos || lf == lowerS(it.fileName)) continue;
                        if (lf.find("onair") == std::string::npos && lf.find("codec") == std::string::npos && lf.find(stem) == std::string::npos) continue;
                        it.detail += ". Found a file with a different name in " + d.path + ": \"" + f + "\" (rename it to exactly " + it.fileName + ")";
                        break;
                    }
                }
            }
        }
        out.push_back(it);
    }
    return out;
}

std::string setupSummary(const std::vector<SetupItem>& items) {
    size_t ok = 0;
    for (const auto& i : items) if (i.state == SetupState::Loaded || i.state == SetupState::Ready) ++ok;
    return std::to_string(ok) + " of " + std::to_string(items.size()) + " voice codecs installed";
}

bool setupComplete(const std::vector<SetupItem>& items) {
    for (const auto& i : items) if (i.state == SetupState::Missing || i.state == SetupState::WrongArch) return false;
    return !items.empty();
}

std::string setupBuildCommand(SetupOs os) {
    if (os == SetupOs::Windows) return "powershell -ExecutionPolicy Bypass -File tools\\codec-builder\\build-codecs.ps1";
    return "bash tools/codec-builder/build-codecs.sh";
}

std::vector<std::string> setupSteps(SetupOs os) {
    std::vector<std::string> s;
    switch (os) {
    case SetupOs::MacOS:
        s.push_back("Install the build tools once: run  xcode-select --install  and  brew install cmake git  in Terminal.");
        s.push_back("Get the OnAir source (git clone https://github.com/adswill/OnAir, or the Download ZIP button) and open Terminal in that folder.");
        break;
    case SetupOs::Windows:
        s.push_back("Install MSYS2 from msys2.org. In its MINGW64 shell run  pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make git");
        s.push_back("Get the OnAir source (git clone https://github.com/adswill/OnAir, or the Download ZIP button) and open PowerShell in that folder.");
        break;
    default:
        s.push_back("Install the build tools once, for example  sudo apt install git cmake build-essential  (or your distribution's equivalent).");
        s.push_back("Get the OnAir source (git clone https://github.com/adswill/OnAir, or the Download ZIP button) and open a terminal in that folder.");
        break;
    }
    s.push_back("Run  " + setupBuildCommand(os) + "  then type  all  (or the ids you want) and read the patent and licence notice.");
    s.push_back("Type  yes  to accept it. The helper downloads the open-source mbelib-neo from its authors, builds the plug-ins for THIS computer and copies them into the OnAir codecs folder.");
    s.push_back("Come back here and press Rescan. Open a voice mode once and accept the notice for each codec. Encrypted calls are never decoded.");
    return s;
}

} // namespace dect2::voice
