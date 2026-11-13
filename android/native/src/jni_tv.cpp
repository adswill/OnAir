// TV picture and sound on the phone (docs/ANDROID-4.0.md): the transport stream of the TV modes, cut down to the packets of the service
// the user picked, for the app's own demultiplexer (tv/TsDemux.kt) and the phone's decoders (MediaCodec). Nothing is decoded here and
// no codec is in this library: the elementary streams go to the decoders Android itself provides.
//
// THREADING: unlike the other bridge calls these may run on the app's TV thread. They touch only the packet ring below (its own lock)
// and the engine's packet tap and service snapshot, which the engine guards itself (tsMu_).
#include <jni.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "dect2/engine/engine.h"
#include "jni_radio.h"

using namespace dect2;

namespace {

constexpr size_t kPkt = 188;
constexpr size_t kRingPackets = 40000;   // ~7.5 MB: about 6 s of a 10 Mbit/s service, so a slow decoder start loses nothing

struct TvRing {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> buf = std::vector<uint8_t>(kRingPackets * kPkt);
    size_t head = 0, count = 0;           // packets: oldest at head
    std::vector<uint8_t> want = std::vector<uint8_t>(8192, 0);
    bool all = false;                     // every PID (no service picked yet: nothing is kept)
    bool open = false;
    uint64_t seen = 0, kept = 0, dropped = 0;
    double streamSecs = 0;

    void push(const uint8_t* pk, size_t n, double secs) {
        std::lock_guard<std::mutex> lk(mu);
        seen += n;
        streamSecs += secs;
        size_t added = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t* p = pk + i * kPkt;
            if (p[0] != 0x47) continue;
            const int pid = ((p[1] & 0x1F) << 8) | p[2];
            if (!all && !want[(size_t)pid]) continue;
            if (count == kRingPackets) { head = (head + 1) % kRingPackets; count--; dropped++; }   // the reader is too slow: lose the oldest
            memcpy(&buf[((head + count) % kRingPackets) * kPkt], p, kPkt);
            count++; kept++; added++;
        }
        if (added) cv.notify_one();
    }
    void clear() { head = 0; count = 0; }
};

TvRing gRing;

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }
std::string num(double v) { char b[32]; snprintf(b, sizeof b, "%.6g", v); return b; }

jstring mkAscii(JNIEnv* e, const std::string& s) {
    // the service names may hold any bytes: build UTF-16 from UTF-8 by hand (a malformed byte becomes U+FFFD) instead of NewStringUTF
    std::u16string u;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xFFFD; int n = 1;
        auto cont = [&](size_t k) { return k < s.size() && ((unsigned char)s[k] & 0xC0) == 0x80; };
        if (c < 0x80) cp = c;
        else if (c >= 0xC2 && c < 0xE0 && cont(i + 1)) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3F); n = 2; }
        else if (c >= 0xE0 && c < 0xF0 && cont(i + 1) && cont(i + 2)) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); n = 3; }
        else if (c >= 0xF0 && c < 0xF5 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); n = 4;
        }
        i += (size_t)n;
        if (cp >= 0x10000) { cp -= 0x10000; u.push_back((char16_t)(0xD800 + (cp >> 10))); u.push_back((char16_t)(0xDC00 + (cp & 0x3FF))); }
        else u.push_back((char16_t)cp);
    }
    return e->NewString(reinterpret_cast<const jchar*>(u.data()), (jsize)u.size());
}

void installTap(bool on) {
    Engine* eng = onair::bridgeEngine();
    if (!eng) return;
    if (on) eng->setPacketTap([](const uint8_t* pk, size_t n, const TsSnapshot*, double secs) { gRing.push(pk, n, secs); });
    else eng->setPacketTap(nullptr);
}

} // namespace

#define TFN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_io_github_adswill_onair_tv_TvNative_##name

// The services of the transport stream (PAT, PMT, SDT of the core's demultiplexer) as JSON:
// {"packets":N,"services":[{"id","name","provider","type","pmt","pcr","ca","scrambled","caState","lcn","now","next","streams":[...]}]}
TFN(jstring, nativeTvServices)(JNIEnv* e, jobject) {
    Engine* eng = onair::bridgeEngine();
    if (!eng) return mkAscii(e, "{\"packets\":0,\"services\":[]}");
    const TsSnapshot s = eng->tsSnapshot();
    std::string o = "{\"packets\":" + std::to_string(s.totalPackets) + ",\"network\":" + q(s.networkName) + ",\"tsid\":" + std::to_string(s.tsid) +
                    ",\"onid\":" + std::to_string(s.onid) + ",\"services\":[";
    bool first = true;
    for (const TsService& sv : s.services) {
        if (!first) o += ','; first = false;
        o += "{\"id\":" + std::to_string(sv.id) + ",\"name\":" + q(sv.name) + ",\"provider\":" + q(sv.provider) + ",\"type\":" + std::to_string(sv.type) +
             ",\"pmt\":" + std::to_string(sv.pmtPid) + ",\"pcr\":" + std::to_string(sv.pcrPid) + ",\"ca\":" + (sv.caFlag ? "true" : "false") +
             ",\"scrambled\":" + (sv.scrambled ? "true" : "false") + ",\"caState\":" + std::to_string((int)sv.caState) + ",\"lcn\":" + std::to_string(sv.lcn) +
             ",\"havePmt\":" + (sv.havePmt ? "true" : "false") + ",\"now\":" + q(sv.now) + ",\"next\":" + q(sv.next) + ",\"streams\":[";
        for (size_t i = 0; i < sv.streams.size(); i++) {
            const TsStream& st = sv.streams[i];
            o += std::string(i ? "," : "") + "{\"pid\":" + std::to_string(st.pid) + ",\"type\":" + std::to_string(st.streamType) + ",\"codec\":" + q(st.codec) +
                 ",\"kind\":" + q(st.kind) + ",\"lang\":" + q(st.lang) + ",\"kbps\":" + num(st.kbps) + ",\"scrambled\":" + (st.scrambled ? "true" : "false") + "}";
        }
        o += "]}";
    }
    return mkAscii(e, o + "]}");
}

// Keep the packets of these PIDs (comma separated, e.g. "256,257,4096") from now on; "" = keep nothing and take the tap off.
// The ring is emptied: what was kept for an earlier service is not handed out.
TFN(jboolean, nativeTvSelect)(JNIEnv* e, jobject, jstring pids) {
    std::string s;
    if (pids) { const char* c = e->GetStringUTFChars(pids, nullptr); if (c) { s = c; e->ReleaseStringUTFChars(pids, c); } }
    bool any = false;
    {
        std::lock_guard<std::mutex> lk(gRing.mu);
        std::fill(gRing.want.begin(), gRing.want.end(), 0);
        gRing.all = false;
        size_t i = 0;
        while (i < s.size()) {
            size_t j = s.find(',', i);
            if (j == std::string::npos) j = s.size();
            const int pid = atoi(s.substr(i, j - i).c_str());
            if (pid >= 0 && pid < 8192 && j > i) { gRing.want[(size_t)pid] = 1; any = true; }
            i = j + 1;
        }
        gRing.clear();
        gRing.open = any;
        gRing.cv.notify_all();
    }
    installTap(any);
    return any ? JNI_TRUE : JNI_FALSE;
}

// Copies whole packets (188 bytes each) into buf; waits up to timeoutMs for the first one. Returns the bytes copied (0 = none yet,
// -1 = no service selected).
TFN(jint, nativeTvRead)(JNIEnv* e, jobject, jbyteArray out, jint timeoutMs) {
    const jsize cap = e->GetArrayLength(out);
    const size_t want = (size_t)std::max<jsize>(0, cap) / kPkt;
    if (want == 0) return 0;
    std::vector<uint8_t> tmp;
    {
        std::unique_lock<std::mutex> lk(gRing.mu);
        if (!gRing.open) return -1;
        if (gRing.count == 0 && timeoutMs > 0)
            gRing.cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [] { return gRing.count > 0 || !gRing.open; });
        if (!gRing.open) return -1;
        const size_t n = std::min(want, gRing.count);
        tmp.resize(n * kPkt);
        for (size_t k = 0; k < n; k++) memcpy(&tmp[k * kPkt], &gRing.buf[((gRing.head + k) % kRingPackets) * kPkt], kPkt);
        gRing.head = (gRing.head + n) % kRingPackets;
        gRing.count -= n;
    }
    if (!tmp.empty()) e->SetByteArrayRegion(out, 0, (jsize)tmp.size(), reinterpret_cast<const jbyte*>(tmp.data()));
    return (jint)tmp.size();
}

// "seen\tkept\tdropped\tqueued\tstreamSeconds": packets of the whole stream, packets kept for the service, packets lost because the app
// read too slowly, packets waiting now, seconds of stream the receiver delivered since the start
TFN(jstring, nativeTvStats)(JNIEnv* e, jobject) {
    std::lock_guard<std::mutex> lk(gRing.mu);
    return mkAscii(e, std::to_string(gRing.seen) + "\t" + std::to_string(gRing.kept) + "\t" + std::to_string(gRing.dropped) + "\t" +
                          std::to_string(gRing.count) + "\t" + num(gRing.streamSecs));
}
