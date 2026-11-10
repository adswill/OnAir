// onair-extcap: makes OnAir appear as capture interfaces inside Wireshark (the extcap interface, see the wireshark "extcap" manual page).
// Wireshark runs this program; it connects to OnAir on this computer (127.0.0.1) and copies the packets of one mode into Wireshark's pipe as a pcap stream.
// In OnAir the mode's switch in the Wireshark window must be on. docs/WIRESHARK-3.8.md.
#include "dect2/wirecap/wire_chan.h"
#include "dect2/wirecap/wire_format.h"
#include "../core/src/net/netcompat.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/select.h>
#endif
#include <vector>
#ifndef ONAIR_VERSION_TEXT
#define ONAIR_VERSION_TEXT "3.8"
#endif

using namespace dect2;
using namespace dect2::wire;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

const ChanInfo* findIface(const std::string& name) {
    const std::string prefix = "onair_";
    if (name.compare(0, prefix.size(), prefix) != 0) return nullptr;
    const int c = chanById(name.c_str() + prefix.size());
    return c < 0 ? nullptr : &kChans[c];
}

// Wireshark's pipe: a FIFO, or a named pipe on Windows
#ifdef _WIN32
struct Pipe {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool open(const std::string& p) { h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr); return h != INVALID_HANDLE_VALUE; }
    bool write(const void* d, size_t n) { DWORD w = 0; return WriteFile(h, d, (DWORD)n, &w, nullptr) && w == n; }
};
#else
struct Pipe {
    FILE* f = nullptr;
    bool open(const std::string& p) { f = fopen(p.c_str(), "wb"); return f != nullptr; }
    bool write(const void* d, size_t n) { return fwrite(d, 1, n, f) == n && fflush(f) == 0; }
};
#endif

// -1: nothing there (not running), 0: the switch is off, 1: connected
int connectToApp(int port, const char* id, uint32_t linktype, sock_t& out) {
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { sockClose(s); return -1; }
    const std::string hello = std::string("ONAIR-WIRE 1 ") + id + "\n";
    if (sockSend(s, hello.data(), hello.size()) != (long long)hello.size()) { sockClose(s); return -1; }
    std::string line;
    char c;
    while (line.size() < 32) {
        const long long r = sockRecv(s, &c, 1);
        if (r <= 0) { sockClose(s); return -1; }
        if (c == '\n') break;
        line += c;
    }
    if (line != "OK " + std::to_string(linktype)) { sockClose(s); return 0; }
    out = s;
    return 1;
}

int capture(const ChanInfo& ci, const std::string& fifo, int port, int waitSec) {
    Pipe pipe;
    if (!pipe.open(fifo)) { fprintf(stderr, "onair-extcap: cannot open the capture pipe %s\n", fifo.c_str()); return 1; }
    const Bytes hdr = pcapFileHeader(ci.linktype);
    if (!pipe.write(hdr.data(), hdr.size())) return 0;
    netInit();
    const auto started = std::chrono::steady_clock::now();
    bool everConnected = false;
    std::vector<char> buf(1 << 16);
    while (!g_stop) {
        sock_t s = kBadSock;
        const int rc = connectToApp(port, ci.id, ci.linktype, s);
        if (rc != 1) {
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (!everConnected && waitSec > 0 && el > waitSec) {
                if (rc == 0) fprintf(stderr, "OnAir is running, but its Wireshark switch for %s is off. Turn it on in OnAir: View > Wireshark.\n", ci.display);
                else fprintf(stderr, "OnAir is not running, or no Wireshark switch is on (port %d). Start OnAir and turn on %s in View > Wireshark.\n", port, ci.display);
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        everConnected = true;
        while (!g_stop) {
            fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
            timeval tv{0, 250000};   // wake up to see a stop request: a signal does not interrupt a blocking recv (restarted)
            const int sel = select((int)s + 1, &rd, nullptr, nullptr, &tv);
            if (sel == 0) continue;
            if (sel < 0) { if (g_stop) break; continue; }
            const long long r = sockRecv(s, buf.data(), buf.size());
            if (r <= 0) break;                           // OnAir stopped or the switch went off: wait for it to come back
            if (!pipe.write(buf.data(), (size_t)r)) { sockClose(s); return 0; }   // Wireshark closed the capture
        }
        sockClose(s);
    }
    return 0;
}

void listInterfaces() {
    printf("extcap {version=%s}{help=https://github.com/adswill/OnAir/blob/main/docs/WIRESHARK-3.8.md}\n", ONAIR_VERSION_TEXT);
    for (const ChanInfo& c : kChans) printf("interface {value=onair_%s}{display=OnAir: %s}\n", c.id, c.display);
}

} // namespace

int main(int argc, char** argv) {
    bool wantIfaces = false, wantDlts = false, wantConfig = false, wantCapture = false;
    std::string iface, fifo;
    int port = kDefaultStreamPort, waitSec = 20;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--extcap-interfaces") wantIfaces = true;
        else if (a == "--extcap-dlts") wantDlts = true;
        else if (a == "--extcap-config") wantConfig = true;
        else if (a == "--capture") wantCapture = true;
        else if (a == "--extcap-interface" && next) { iface = next; i++; }
        else if (a == "--fifo" && next) { fifo = next; i++; }
        else if (a == "--port" && next) { port = atoi(next); i++; }
        else if (a == "--connect-wait" && next) { waitSec = atoi(next); i++; }
        else if (a == "--help" || a == "-h") {
            puts("onair-extcap: OnAir as a Wireshark capture interface. Wireshark runs this program itself; copy it into Wireshark's personal extcap folder\n"
                 "(About Wireshark > Folders), or press \"Add OnAir to Wireshark\" in OnAir: View > Wireshark.");
            return 0;
        }
        // anything else (--extcap-version, --extcap-capture-filter ...) is not needed here
    }
    if (wantIfaces) { listInterfaces(); return 0; }
    const ChanInfo* ci = findIface(iface);
    if (!ci) { fprintf(stderr, "onair-extcap: unknown interface \"%s\"\n", iface.c_str()); return 1; }
    if (wantDlts) { printf("dlt {number=%u}{name=%s}{display=%s}\n", ci->linktype, ci->linkName, ci->display); return 0; }
    if (wantConfig) {
        printf("arg {number=0}{call=--port}{display=OnAir local port}{type=integer}{range=1,65535}{default=%d}{tooltip=The port on this computer, as set in OnAir (View > Wireshark, Advanced)}\n", kDefaultStreamPort);
        printf("arg {number=1}{call=--connect-wait}{display=Give up after (seconds, 0 never)}{type=integer}{range=0,86400}{default=20}{tooltip=How long to wait for OnAir before the capture stops with a message}\n");
        return 0;
    }
    if (wantCapture) {
        if (fifo.empty()) { fprintf(stderr, "onair-extcap: --fifo is missing\n"); return 1; }
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
#ifndef _WIN32
        std::signal(SIGPIPE, SIG_IGN);
#endif
        return capture(*ci, fifo, port, waitSec);
    }
    fprintf(stderr, "onair-extcap: run by Wireshark (--extcap-interfaces, --capture ...)\n");
    return 1;
}
