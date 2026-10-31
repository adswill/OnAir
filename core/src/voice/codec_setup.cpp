#include "dect2/voice/codec_setup.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/codec/speech_codec.h"

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
            it.detail = "not installed: needs " + it.fileName;
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
