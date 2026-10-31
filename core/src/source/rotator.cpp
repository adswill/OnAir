#include "dect2/common/thread_name.h"
#include "dect2/source/rotator.h"
#include "dect2/source/mount_driver.h"
#include "dect2/net/remote_api.h"
#include "../net/netcompat.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <condition_variable>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace dect2 {

namespace {

double normAz(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0) a += 360.0;
    return a;
}

double clampEl(double e) { return std::max(0.0, std::min(90.0, e)); }

double shortAzDelta(double from, double to) {
    double d = normAz(to - from);
    if (d > 180.0) d -= 360.0;
    return d;
}

double correctedAz(const RotatorCalibration& c, double mechAz) { return normAz(mechAz + c.azOffsetDeg); }
double correctedEl(const RotatorCalibration& c, double mechEl) { return mechEl + c.elOffsetDeg; }

void mechanicalFromCorrected(const RotatorCalibration& c, double corrAz, double corrEl, double curMechAz, bool haveCurAz,
                             double& outMechAz, double& outMechEl) {
    outMechAz = normAz(corrAz - c.azOffsetDeg);
    outMechEl = corrEl - c.elOffsetDeg;
    if (c.limitAz) outMechAz = std::max(c.azMin, std::min(c.azMax, outMechAz));
    if (c.limitEl) outMechEl = std::max(c.elMin, std::min(c.elMax, outMechEl));
    else outMechEl = clampEl(outMechEl);
    if (c.shortestPathAz && haveCurAz) {
        double best = outMechAz;
        double bestAbs = std::fabs(shortAzDelta(curMechAz, outMechAz));
        auto tryCand = [&](double cand) {
            double az = c.limitAz ? cand : normAz(cand);
            if (c.limitAz && (az < c.azMin || az > c.azMax)) return;
            const double d = std::fabs(shortAzDelta(curMechAz, az));
            if (d < bestAbs) { bestAbs = d; best = az; }
        };
        tryCand(outMechAz);
        tryCand(outMechAz + 360.0);
        tryCand(outMechAz - 360.0);
        outMechAz = c.limitAz ? best : normAz(best);
    }
}

void applyStatusCorrection(RotatorStatus& st, const RotatorCalibration& cal) {
    if (st.azValid) st.azCorrectedDeg = normAz(st.azDeg + cal.azOffsetDeg);
    if (st.elValid) st.elCorrectedDeg = st.elDeg + cal.elOffsetDeg;
}

double clampAzWindow(double az, const RotatorCalibration& cal) {
    az = normAz(az);
    if (!cal.limitAz) return az;
    if (cal.azMax <= cal.azMin + 0.01) return az;
    if (cal.azMax >= 359.9 && cal.azMin <= 0.1) return az;
    while (az < cal.azMin) az += 360;
    while (az > cal.azMax) az -= 360;
    if (az < cal.azMin) az = cal.azMin;
    if (az > cal.azMax) az = cal.azMax;
    return normAz(az);
}

double mechanicalAzForCorrected(double correctedAz, double currentMechAz, const RotatorCalibration& cal) {
    double m = normAz(correctedAz - cal.azOffsetDeg);
    if (!cal.shortestPathAz || !std::isfinite(currentMechAz)) return clampAzWindow(m, cal);
    const double a = normAz(m);
    const double b = normAz(m + 360);
    const double c = normAz(m - 360);
    auto dist = [&](double t) { double d = std::fabs(normAz(t - currentMechAz)); return d > 180 ? 360 - d : d; };
    double best = a;
    if (dist(b) < dist(best)) best = b;
    if (dist(c) < dist(best)) best = c;
    return clampAzWindow(best, cal);
}

double mechanicalElForCorrected(double correctedEl, const RotatorCalibration& cal) {
    double e = correctedEl - cal.elOffsetDeg;
    if (cal.limitEl) e = std::max(cal.elMin, std::min(cal.elMax, e));
    else e = clampEl(e);
    return e;
}

bool parsePosition(const std::string& line, double& az, double& el, bool& haveAz, bool& haveEl) {
    haveAz = haveEl = false;
    std::string s;
    for (char c : line) {
        if (c == '\r' || c == '\n') continue;
        if (c == '+' || c == '-' || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '/') s += c;
    }
    if (s.empty()) return false;
    // +120+045, +120-010, 120 045, 120/045
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        const char sign = s[i];
        size_t j = i + 1;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
        if (j > i + 1) {
            az = std::stod(s.substr(i, j - i));
            if (sign == '-') az = normAz(-az);
            else az = normAz(az);
            haveAz = true;
            i = j;
        }
    } else {
        size_t j = i;
        while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.')) j++;
        if (j > i) {
            az = normAz(std::stod(s.substr(i, j - i)));
            haveAz = true;
            i = j;
        }
    }
    while (i < s.size() && (s[i] == ' ' || s[i] == '/')) i++;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        const char sign = s[i];
        size_t j = i + 1;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
        if (j > i + 1) {
            el = std::stod(s.substr(i, j - i));
            if (sign == '-') el = -el;
            haveEl = true;
        }
    } else if (i < s.size()) {
        size_t j = i;
        while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.')) j++;
        if (j > i) { el = std::stod(s.substr(i, j - i)); haveEl = true; }
    }
    return haveAz || haveEl;
}

struct Io {
    enum Kind { None, Serial, Tcp } kind = None;
#ifndef _WIN32
    int fd = -1;
#else
    HANDLE h = INVALID_HANDLE_VALUE;
#endif
    sock_t sock = kBadSock;

    void closeAll() {
#ifndef _WIN32
        if (fd >= 0) { ::close(fd); fd = -1; }
#else
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
#endif
        if (sock != kBadSock) { sockClose(sock); sock = kBadSock; }
        kind = None;
    }

    bool writeLine(const std::string& cmd) {
        std::string line = cmd;
        if (line.empty() || (line.back() != '\n' && line.back() != '\r')) line += "\r";
        const char* p = line.c_str();
        size_t n = line.size();
        if (kind == Tcp && sock != kBadSock) {
            while (n) {
                const long long w = sockSend(sock, p, n);
                if (w <= 0) return false;
                p += w; n -= (size_t)w;
            }
            return true;
        }
#ifndef _WIN32
        if (kind == Serial && fd >= 0) return ::write(fd, line.c_str(), line.size()) == (ssize_t)line.size();
#else
        if (kind == Serial && h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            return WriteFile(h, line.c_str(), (DWORD)line.size(), &w, nullptr) && w == line.size();
        }
#endif
        return false;
    }

    bool readLine(std::string& out, int timeoutMs) {
        out.clear();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char buf[1];
        while (std::chrono::steady_clock::now() < deadline) {
            if (kind == Tcp && sock != kBadSock) {
                fd_set rd; FD_ZERO(&rd); FD_SET(sock, &rd);
                timeval tv{0, 200000};
                if (select((int)sock + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
                const long long r = sockRecv(sock, buf, 1);
                if (r <= 0) return false;
            } else if (kind == Serial) {
#ifndef _WIN32
                if (fd < 0) return false;
                fd_set rd; FD_ZERO(&rd); FD_SET(fd, &rd);
                timeval tv{0, 200000};
                if (select(fd + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
                if (::read(fd, buf, 1) != 1) return false;
#else
                if (h == INVALID_HANDLE_VALUE) return false;
                DWORD n = 0;
                COMMTIMEOUTS t{};
                t.ReadIntervalTimeout = 50;
                t.ReadTotalTimeoutConstant = 200;
                t.ReadTotalTimeoutMultiplier = 10;
                SetCommTimeouts(h, &t);
                if (!ReadFile(h, buf, 1, &n, nullptr) || n != 1) continue;
#endif
            } else return false;
            if (buf[0] == '\r' || buf[0] == '\n') {
                if (!out.empty()) return true;
                continue;
            }
            out += buf[0];
            if (out.size() > 128) return true;
        }
        return !out.empty();
    }
};

#ifndef _WIN32
bool openSerial(const std::string& dev, int baud, Io& io, std::string& err) {
    io.closeAll();
    const int fd = ::open(dev.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) { err = "cannot open " + dev; return false; }
    termios t{};
    if (tcgetattr(fd, &t) != 0) { ::close(fd); err = "cannot read serial settings for " + dev; return false; }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CSIZE; t.c_cflag |= CS8;
    t.c_cflag &= ~PARENB;
    t.c_cflag &= ~CSTOPB;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 5;
    speed_t spd = B9600;
    switch (baud) {
    case 1200: spd = B1200; break;
    case 2400: spd = B2400; break;
    case 4800: spd = B4800; break;
    case 19200: spd = B19200; break;
    case 38400: spd = B38400; break;
    case 57600: spd = B57600; break;
    case 115200: spd = B115200; break;
    default: spd = B9600; break;
    }
    cfsetispeed(&t, spd);
    cfsetospeed(&t, spd);
    if (tcsetattr(fd, TCSANOW, &t) != 0) { ::close(fd); err = "cannot configure " + dev; return false; }
    io.kind = Io::Serial;
    io.fd = fd;
    return true;
}
#else
bool openSerial(const std::string& dev, int baud, Io& io, std::string& err) {
    io.closeAll();
    std::string path = dev;
    if (path.rfind("\\\\.\\", 0) != 0) path = "\\\\.\\" + path;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot open " + dev; return false; }
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) { CloseHandle(h); err = "cannot read COM settings for " + dev; return false; }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); err = "cannot configure " + dev; return false; }
    io.kind = Io::Serial;
    io.h = h;
    return true;
}
#endif

bool openTcp(const std::string& host, int port, Io& io, std::string& err) {
    io.closeAll();
    netInit();
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) { err = "cannot open a socket"; return false; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        err = "invalid host address";
        sockClose(s);
        return false;
    }
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) {
        err = "cannot connect to " + host + ":" + std::to_string(port);
        sockClose(s);
        return false;
    }
    io.kind = Io::Tcp;
    io.sock = s;
    return true;
}

} // namespace

double rotatorAzOffsetForTrueBearing(double trueAzDeg, double mechanicalAzDeg) {
    auto norm = [](double a) {
        a = std::fmod(a, 360.0);
        if (a < 0) a += 360.0;
        return a;
    };
    double o = norm(trueAzDeg) - norm(mechanicalAzDeg);
    while (o > 180.0) o -= 360.0;
    while (o < -180.0) o += 360.0;
    return o;
}

double rotatorElOffsetForTrueElevation(double trueElDeg, double mechanicalElDeg) {
    return trueElDeg - mechanicalElDeg;
}

struct Rotator::Impl {
    mutable std::mutex mu;
    RotatorConfig cfg;
    RotatorCalibration cal;
    RotatorStatus st;
    Io io;
    std::thread th;
    std::atomic<bool> quit{false};
    std::deque<std::string> cmds;
    double pollAt = 0;
    std::unique_ptr<MountDriver> drv;   // rotctld / Alpaca; null for GS-232
    double rateTargetAz = 0, rateTargetEl = 0, rateAz = 0, rateEl = 0;   // wanted mechanical aim and speeds of a track command
    std::chrono::steady_clock::time_point rateUntil{};
    bool ratesLive = false;
    int pollFails = 0;
    bool canRates = false;

    std::condition_variable wakeCv;   // wakes run() when a command is queued or the thread must stop (no 50 ms polling)
    void stopThread() {
        quit = true;
        wakeCv.notify_all();
        if (th.joinable()) th.join();
        quit = false;
        std::lock_guard<std::mutex> lk(mu);
        io.closeAll();
        if (drv) { drv->close(); drv.reset(); }
        st.connected = false;
    }

    void setErr(const std::string& e) {
        std::lock_guard<std::mutex> lk(mu);
        st.lastError = e;
        st.connected = false;
        io.closeAll();
        if (drv) { drv->close(); drv.reset(); }
        ratesLive = false;
    }

    bool connectNow() {
        std::string err;
        RotatorConfig c;
        { std::lock_guard<std::mutex> lk(mu); c = cfg; }
        bool ok = false;
        if ((drv = makeMountDriver(c)) != nullptr) { ok = drv->connect(err); canRates = ok && drv->supportsRates(); }
        else if (c.link == RotatorLink::Serial) ok = openSerial(c.serialDevice, c.baud, io, err);
        else if (c.link == RotatorLink::Tcp) ok = openTcp(c.host, c.tcpPort, io, err);
        else { err = "no rotator configured"; }
        std::lock_guard<std::mutex> lk(mu);
        st.connected = ok;
        st.lastError = ok ? "" : err;
        if (!ok) { io.closeAll(); if (drv) { drv->close(); drv.reset(); } }
        return ok;
    }

    void enqueue(const std::string& cmd) {
        std::lock_guard<std::mutex> lk(mu);
        cmds.push_back(cmd);
        wakeCv.notify_all();
    }

    void updateMoving() {
        if (!st.hasTarget) { st.moving = false; return; }
        if (!st.azValid && !st.elValid) { st.moving = true; return; }
        const double dAz = st.azValid ? std::fabs(shortAzDelta(st.azDeg, st.targetAz)) : 0;
        const double dEl = st.elValid ? std::fabs(st.targetEl - st.elDeg) : 0;
        const double tol = 1.5;
        st.moving = (st.azValid && std::fabs(dAz) > tol) || (st.elValid && dEl > tol);
        if (!st.moving) st.hasTarget = false;
    }

    void pollPosition() {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!st.connected) return;
        }
        if (drv) {
            MountPos mp;
            std::string err;
            if (!drv->readPosition(mp, err)) {
                std::lock_guard<std::mutex> lk(mu);
                if (++pollFails >= 3) { st.lastError = err; st.connected = false; if (drv) { drv->close(); drv.reset(); } }
                return;
            }
            pollFails = 0;
            std::lock_guard<std::mutex> lk(mu);
            st.azDeg = mp.azDeg; st.azValid = mp.azValid;
            st.elDeg = mp.elDeg; st.elValid = mp.elValid;
            applyStatusCorrection(st, cal);
            if (mp.slewingValid && !ratesLive) {
                st.moving = mp.slewing;
                if (!mp.slewing) st.hasTarget = false;
            } else updateMoving();
            return;
        }
        if (!io.writeLine(";")) { setErr("write failed while polling"); return; }
        std::string line;
        if (!io.readLine(line, 800)) return;
        double az = 0, el = 0;
        bool haveAz = false, haveEl = false;
        if (parsePosition(line, az, el, haveAz, haveEl)) {
            std::lock_guard<std::mutex> lk(mu);
            if (haveAz) { st.azDeg = az; st.azValid = true; }
            if (haveEl) { st.elDeg = el; st.elValid = true; }
            applyStatusCorrection(st, cal);
            updateMoving();
        }
        if (!io.writeLine("V")) return;
        if (io.readLine(line, 400) && !line.empty()) {
            double az2 = 0, el2 = 0;
            bool ha = false, he = false;
            if (parsePosition(line, az2, el2, ha, he) && he) {
                std::lock_guard<std::mutex> lk(mu);
                st.elDeg = el2;
                st.elValid = true;
                applyStatusCorrection(st, cal);
                updateMoving();
            }
        }
    }

    bool sendGoto(double az, double el, std::string& err) {   // mechanical degrees
        ratesLive = false;
        if (drv) {
            return drv->gotoAzEl(az, el, err);
        }
        char buf[32];
        std::snprintf(buf, sizeof buf, "W%03d %03d", (int)std::lround(az) % 360, (int)std::lround(el));
        if (!io.writeLine(buf)) { err = "move command failed"; return false; }
        return true;
    }

    // One step of the rate loop of a tracking mount (speed per axis from the pointing error and the target's own speed).
    void rateStep() {
        using clock = std::chrono::steady_clock;
        if (!ratesLive || !drv) return;
        RotatorStatus s;
        double tAz, tEl, fAz, fEl;
        {
            std::lock_guard<std::mutex> lk(mu);
            s = st; tAz = rateTargetAz; tEl = rateTargetEl; fAz = rateAz; fEl = rateEl;
        }
        std::string err;
        if (clock::now() > rateUntil || !s.azValid || !s.elValid) {
            ratesLive = false;
            drv->moveAxes(0, 0, err);   // the target stopped being refreshed (or position is unknown): stand still
            return;
        }
        const double ra = mountRateForError(shortAzDelta(s.azDeg, tAz), fAz, 6.0);
        const double re = mountRateForError(tEl - s.elDeg, fEl, 6.0);
        if (!drv->moveAxes(ra, re, err)) { ratesLive = false; std::lock_guard<std::mutex> lk(mu); st.lastError = err; }
    }

    void run() {
        using clock = std::chrono::steady_clock;
        auto nextPoll = clock::now();
        while (!quit) {
            std::deque<std::string> todo;
            RotatorConfig c;
            bool linked = false;
            {
                std::lock_guard<std::mutex> lk(mu);
                todo.swap(cmds);
                c = cfg;
                linked = st.connected;
            }
            if (c.link == RotatorLink::None) {
                std::unique_lock<std::mutex> lk(mu);   // no link configured: sleep until a command or stop arrives
                wakeCv.wait_for(lk, std::chrono::seconds(2), [&] { return quit.load() || !cmds.empty(); });
                continue;
            }
            if (!linked) {
                if (!connectNow()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            for (const std::string& cmd : todo) {
                if (cmd == "disconnect") {
                    std::lock_guard<std::mutex> lk(mu);
                    io.closeAll();
                    st.connected = false;
                    st.moving = false;
                    st.hasTarget = false;
                    continue;
                }
                if (cmd == "stop") {
                    std::string serr;
                    bool sok = true;
                    if (drv) { ratesLive = false; sok = drv->stop(serr); }
                    else sok = io.writeLine("C");
                    if (!sok) setErr(serr.empty() ? "stop command failed" : serr);
                    else {
                        std::lock_guard<std::mutex> lk(mu);
                        st.moving = false;
                        st.hasTarget = false;
                    }
                    continue;
                }
                if (cmd == "poll") {
                    pollPosition();
                    continue;
                }
                if (cmd.rfind("goto ", 0) == 0) {
                    double az = 0, el = 0;
                    int corrected = 0;
                    if (std::sscanf(cmd.c_str(), "goto %d %lf %lf", &corrected, &az, &el) == 3) {
                        RotatorCalibration cal;
                        double curAz = 0;
                        double corrAz = 0, corrEl = 0;
                        {
                            std::lock_guard<std::mutex> lk(mu);
                            cal = this->cal;
                            if (st.azValid) curAz = st.azDeg;
                        }
                        if (corrected) {
                            corrAz = normAz(az);
                            corrEl = el;
                            az = mechanicalAzForCorrected(az, curAz, cal);
                            el = mechanicalElForCorrected(el, cal);
                        } else {
                            az = normAz(az);
                            el = clampEl(el);
                        }
                        std::string gerr;
                        if (!sendGoto(az, el, gerr)) setErr(gerr);
                        else {
                            std::lock_guard<std::mutex> lk(mu);
                            st.targetAz = az;
                            st.targetEl = el;
                            st.hasTarget = true;
                            st.moving = true;
                        }
                    }
                }
                if (cmd.rfind("track ", 0) == 0) {
                    double az = 0, el = 0, ra = 0, re = 0;
                    if (std::sscanf(cmd.c_str(), "track %lf %lf %lf %lf", &az, &el, &ra, &re) == 4) {
                        RotatorCalibration cal;
                        double curAz = 0;
                        {
                            std::lock_guard<std::mutex> lk(mu);
                            cal = this->cal;
                            if (st.azValid) curAz = st.azDeg;
                        }
                        const double mAz = mechanicalAzForCorrected(az, curAz, cal);
                        const double mEl = mechanicalElForCorrected(el, cal);
                        if (drv && drv->supportsRates()) {
                            {
                                std::lock_guard<std::mutex> lk(mu);
                                rateTargetAz = mAz; rateTargetEl = mEl; rateAz = ra; rateEl = re;
                                st.targetAz = mAz; st.targetEl = mEl; st.hasTarget = true; st.moving = true;
                            }
                            ratesLive = true;
                            rateUntil = clock::now() + std::chrono::seconds(3);
                        } else {
                            std::string gerr;
                            if (!sendGoto(mAz, mEl, gerr)) setErr(gerr);
                            else {
                                std::lock_guard<std::mutex> lk(mu);
                                st.targetAz = mAz; st.targetEl = mEl; st.hasTarget = true; st.moving = true;
                            }
                        }
                    }
                    continue;
                }
                if (cmd == "park") {
                    RotatorCalibration cal;
                    double curAz = 0;
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        cal = this->cal;
                        if (st.azValid) curAz = st.azDeg;
                    }
                    const double az = mechanicalAzForCorrected(cal.parkAz, curAz, cal);
                    const double el = mechanicalElForCorrected(cal.parkEl, cal);
                    std::string gerr;
                    if (!sendGoto(az, el, gerr)) setErr(gerr.empty() ? "park command failed" : gerr);
                    else {
                        std::lock_guard<std::mutex> lk(mu);
                        st.targetAz = az;
                        st.targetEl = el;
                        st.hasTarget = true;
                        st.moving = true;
                    }
                }
            }
            if (clock::now() >= nextPoll) {
                pollPosition();
                nextPoll = clock::now() + std::chrono::milliseconds(ratesLive ? 250 : 500);
                rateStep();
            }
            {   // until the next position poll, a queued command or stop
                std::unique_lock<std::mutex> lk(mu);
                wakeCv.wait_until(lk, nextPoll, [&] { return quit.load() || !cmds.empty(); });
            }
        }
    }
};

Rotator& Rotator::get() { static Rotator r; return r; }
Rotator::Rotator() : p_(new Impl) {}
Rotator::~Rotator() { disconnect(); }

bool Rotator::configure(const RotatorConfig& cfg) {
    p_->stopThread();
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        const RotatorCalibration cal = p_->cal;
        p_->cfg = cfg;
        p_->st = RotatorStatus{};
        p_->cal = cal;
        p_->cmds.clear();
    }
    connected_.store(cfg.link != RotatorLink::None, std::memory_order_relaxed);
    if (cfg.link == RotatorLink::None) return true;
    p_->quit = false;
    p_->th = dect2::namedThread("onair-rotator", [this] { p_->run(); });
    return true;
}

RotatorConfig Rotator::config() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->cfg;
}

void Rotator::disconnect() {
    connected_.store(false, std::memory_order_relaxed);
    p_->enqueue("disconnect");
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->cfg.link = RotatorLink::None;
    }
    p_->stopThread();
}

void Rotator::stop() { p_->enqueue("stop"); }

void Rotator::requestPoll() { p_->enqueue("poll"); }

void Rotator::park() { p_->enqueue("park"); }

void Rotator::trackAzEl(double azDeg, double elDeg, double azRate, double elRate) {
    char buf[120];
    std::snprintf(buf, sizeof buf, "track %.3f %.3f %.5f %.5f", azDeg, elDeg, azRate, elRate);
    {   // newest track order wins: do not let a queue of them build up behind a slow mount
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto it = p_->cmds.begin(); it != p_->cmds.end();) it = (it->rfind("track ", 0) == 0) ? p_->cmds.erase(it) : it + 1;
    }
    p_->enqueue(buf);
}

bool Rotator::supportsRates() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->canRates && p_->st.connected;
}

void Rotator::setCalibration(const RotatorCalibration& c) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->cal = c;
    applyStatusCorrection(p_->st, p_->cal);
}

RotatorCalibration Rotator::calibration() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->cal;
}

void Rotator::gotoAzEl(double azDeg, double elDeg) {
    char buf[80];
    std::snprintf(buf, sizeof buf, "goto 1 %.3f %.3f", azDeg, elDeg);
    p_->enqueue(buf);
}

void Rotator::gotoMechanicalAzEl(double azDeg, double elDeg) {
    char buf[80];
    std::snprintf(buf, sizeof buf, "goto 0 %.3f %.3f", azDeg, elDeg);
    p_->enqueue(buf);
}

RotatorStatus Rotator::status() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    RotatorStatus s = p_->st;
    applyStatusCorrection(s, p_->cal);
    return s;
}

std::string Rotator::apiStatusJson() const {
    const RotatorStatus s = status();
    const RotatorConfig c = config();
    const RotatorCalibration cal = calibration();
    const char* link = "off";
    if (c.link == RotatorLink::Serial) link = "serial";
    else if (c.link == RotatorLink::Tcp) link = c.protocol == RotatorProtocol::Rotctld ? "rotctld" : c.protocol == RotatorProtocol::Alpaca ? "alpaca" : "tcp";
    char b[640];
    std::snprintf(b, sizeof b,
                  "{\"connected\":%s,\"moving\":%s,\"link\":\"%s\","
                  "\"mechanical\":{\"az\":%s,\"el\":%s,\"azValid\":%s,\"elValid\":%s},"
                  "\"corrected\":{\"az\":%s,\"el\":%s},"
                  "\"offset\":{\"azDeg\":%s,\"elDeg\":%s},\"shortestPathAz\":%s,"
                  "\"park\":{\"az\":%s,\"el\":%s}}",
                  s.connected ? "true" : "false", s.moving ? "true" : "false", link,
                  jsonNum(s.azDeg).c_str(), jsonNum(s.elDeg).c_str(), s.azValid ? "true" : "false", s.elValid ? "true" : "false",
                  jsonNum(s.azValid ? s.azCorrectedDeg : 0).c_str(), jsonNum(s.elValid ? s.elCorrectedDeg : 0).c_str(),
                  jsonNum(cal.azOffsetDeg).c_str(), jsonNum(cal.elOffsetDeg).c_str(), cal.shortestPathAz ? "true" : "false",
                  jsonNum(cal.parkAz).c_str(), jsonNum(cal.parkEl).c_str());
    std::string o = b;
    if (!s.lastError.empty()) o += ",\"error\":\"" + jsonEscape(s.lastError) + "\"";
    return o + "}";
}

} // namespace dect2
