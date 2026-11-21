// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MIDI input ports: see midi_in.h. One section per system.
#include "midi_in.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#  include <windows.h>
#  include <mmsystem.h>
#elif defined(__APPLE__)
#  include <CoreMIDI/CoreMIDI.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#  include <dirent.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <cerrno>
#  include <fstream>

#endif

namespace midi {

namespace {
// bytes handed from a driver thread to the UI thread
struct Queue {
    std::mutex mu;
    std::vector<uint8_t> bytes;
    void push(const uint8_t* p, size_t n) { std::lock_guard<std::mutex> lk(mu); bytes.insert(bytes.end(), p, p + n); }
    void take(std::vector<uint8_t>& out) { std::lock_guard<std::mutex> lk(mu); out.swap(bytes); bytes.clear(); }
};
} // namespace

// ---------------------------------------------------------------------------------------------------------------------------- Windows
#if defined(_WIN32)

std::vector<PortInfo> listPorts() {
    std::vector<PortInfo> v;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        MIDIINCAPSA caps;
        if (midiInGetDevCapsA(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        v.push_back({std::to_string(i), caps.szPname});
    }
    return v;
}

struct Input::Impl {
    HMIDIIN h = nullptr;
    Queue q;
    std::string id, err;
    static void CALLBACK cb(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
        if (msg != MIM_DATA) return;   // short messages: status in the low byte, then two data bytes (system exclusive is not used)
        Queue* q = reinterpret_cast<Queue*>(inst);
        const uint8_t b[3] = {(uint8_t)(p1 & 0xFF), (uint8_t)((p1 >> 8) & 0xFF), (uint8_t)((p1 >> 16) & 0xFF)};
        const uint8_t st = b[0];
        const size_t len = st >= 0xF8 ? 1 : (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 || st == 0xF1 || st == 0xF3 ? 2 : st == 0xF6 ? 1 : 3;   // the bytes the message really has
        q->push(b, len);
    }
};
Input::Input() : p_(std::make_unique<Impl>()) {}
Input::~Input() { close(); }
bool Input::open(const PortInfo& port) {
    close();
    char* end = nullptr;
    const unsigned long devL = std::strtoul(port.id.c_str(), &end, 10);
    if (port.id.empty() || *end != 0 || devL > 255) { p_->err = "Not a MIDI port."; return false; }
    const UINT dev = (UINT)devL;
    if (midiInOpen(&p_->h, dev, reinterpret_cast<DWORD_PTR>(&Impl::cb), reinterpret_cast<DWORD_PTR>(&p_->q), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        p_->h = nullptr; p_->err = "The MIDI port could not be opened (another program may be using it)."; return false;
    }
    midiInStart(p_->h);
    p_->id = port.id; p_->err.clear();
    return true;
}
void Input::close() { if (p_->h) { midiInStop(p_->h); midiInReset(p_->h); midiInClose(p_->h); p_->h = nullptr; } p_->id.clear(); }
bool Input::isOpen() const { return p_->h != nullptr; }

// ---------------------------------------------------------------------------------------------------------------------------- macOS
#elif defined(__APPLE__)

namespace {
std::string cfString(CFStringRef s) {
    char b[256] = {};
    if (!s || !CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8)) return std::string();
    return b;
}
}

std::vector<PortInfo> listPorts() {
    std::vector<PortInfo> v;
    const ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef e = MIDIGetSource(i);
        CFStringRef nm = nullptr;
        MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &nm);
        v.push_back({std::to_string((unsigned long)i), cfString(nm)});
        if (nm) CFRelease(nm);
    }
    return v;
}

struct Input::Impl {
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    MIDIEndpointRef src = 0;
    Queue q;
    std::string id, err;
    static void readProc(const MIDIPacketList* list, void* refCon, void*) {
        Queue* q = static_cast<Queue*>(refCon);
        const MIDIPacket* pk = &list->packet[0];
        for (UInt32 i = 0; i < list->numPackets; i++) { q->push(pk->data, pk->length); pk = MIDIPacketNext(pk); }
    }
};
Input::Input() : p_(std::make_unique<Impl>()) {}
Input::~Input() { close(); }
bool Input::open(const PortInfo& pi) {
    close();
    char* end = nullptr;
    const unsigned long idxL = std::strtoul(pi.id.c_str(), &end, 10);
    if (pi.id.empty() || *end != 0) { p_->err = "Not a MIDI port."; return false; }
    const ItemCount idx = (ItemCount)idxL;
    if (idx >= MIDIGetNumberOfSources()) { p_->err = "The MIDI port is gone."; return false; }
    if (MIDIClientCreate(CFSTR("OnAir"), nullptr, nullptr, &p_->client) != noErr ||
        MIDIInputPortCreate(p_->client, CFSTR("OnAir in"), &Impl::readProc, &p_->q, &p_->port) != noErr) { p_->err = "CoreMIDI could not open an input."; close(); return false; }
    p_->src = MIDIGetSource(idx);
    if (MIDIPortConnectSource(p_->port, p_->src, nullptr) != noErr) { p_->err = "The MIDI port could not be connected."; close(); return false; }
    p_->id = pi.id; p_->err.clear();
    return true;
}
void Input::close() {
    if (p_->port && p_->src) MIDIPortDisconnectSource(p_->port, p_->src);
    if (p_->port) MIDIPortDispose(p_->port);
    if (p_->client) MIDIClientDispose(p_->client);
    p_->port = 0; p_->client = 0; p_->src = 0; p_->id.clear();
}
bool Input::isOpen() const { return p_->port != 0; }

// ---------------------------------------------------------------------------------------------------------------------------- Linux
#elif defined(__linux__) && !defined(__ANDROID__)

std::vector<PortInfo> listPorts() {
    std::vector<PortInfo> v;
    DIR* d = opendir("/dev/snd");
    if (!d) return v;
    while (const dirent* e = readdir(d)) {
        // "midiC<card>D<device>": parsed by hand, digits only, bounded
        const std::string fn = e->d_name;
        const size_t d0 = fn.find('D', 5);
        if (fn.compare(0, 5, "midiC") != 0 || d0 == std::string::npos || d0 == 5 || d0 + 1 >= fn.size() || fn.size() > 20) continue;
        const std::string cs = fn.substr(5, d0 - 5), ds = fn.substr(d0 + 1);
        if (cs.find_first_not_of("0123456789") != std::string::npos || ds.find_first_not_of("0123456789") != std::string::npos) continue;
        const std::string path = "/dev/snd/" + fn;
        std::string cardId;
        {
            std::ifstream f("/proc/asound/card" + cs + "/id");
            if (f) std::getline(f, cardId);
        }
        if (cardId.size() > 40) cardId.resize(40);
        const std::string name = (cardId.empty() ? "MIDI" : cardId) + " (card " + cs + ", device " + ds + ")";
        v.push_back({path, name});
    }
    closedir(d);
    std::sort(v.begin(), v.end(), [](const PortInfo& a, const PortInfo& b) { return a.id < b.id; });
    return v;
}

struct Input::Impl {
    int fd = -1;
    std::string id, err;
};
Input::Input() : p_(std::make_unique<Impl>()) {}
Input::~Input() { close(); }
bool Input::open(const PortInfo& port) {
    close();
    p_->fd = ::open(port.id.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (p_->fd < 0) {
        p_->err = errno == EACCES ? "No permission to read the MIDI device (the user needs to be in the audio group)." : "The MIDI device could not be opened.";
        return false;
    }
    p_->id = port.id; p_->err.clear();
    return true;
}
void Input::close() { if (p_->fd >= 0) { ::close(p_->fd); p_->fd = -1; } p_->id.clear(); }
bool Input::isOpen() const { return p_->fd >= 0; }

// ---------------------------------------------------------------------------------------------------------------------------- no ports
#else

std::vector<PortInfo> listPorts() { return {}; }
struct Input::Impl { std::string id, err; };
Input::Input() : p_(std::make_unique<Impl>()) {}
Input::~Input() = default;
bool Input::open(const PortInfo&) { p_->err = "MIDI input is not available on this system."; return false; }
void Input::close() {}
bool Input::isOpen() const { return false; }
#endif

// ---------------------------------------------------------------------------------------------------------------------------- shared
const std::string& Input::portId() const { return p_->id; }
const std::string& Input::error() const { return p_->err; }

void Input::poll(std::vector<Msg>& out) {
#if defined(_WIN32) || defined(__APPLE__)
    std::vector<uint8_t> b;
    p_->q.take(b);
    static thread_local Parser parser;   // the UI thread is the only caller
    parser.feed(b.data(), b.size(), out);
#elif defined(__linux__) && !defined(__ANDROID__)
    if (p_->fd < 0) return;
    uint8_t buf[256];
    static thread_local Parser parser;
    for (;;) {
        const ssize_t n = ::read(p_->fd, buf, sizeof buf);
        if (n > 0) { parser.feed(buf, (size_t)n, out); continue; }
        if (n < 0 && errno != EAGAIN && errno != EINTR) { p_->err = "The MIDI device was unplugged."; close(); }
        break;
    }
#else
    (void)out;
#endif
}

} // namespace midi
