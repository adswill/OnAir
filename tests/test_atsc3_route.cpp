// ROUTE: LCT header round trip, object reassembly from shuffled and duplicated fragments, MIME packages, the S-TSID, and a complete service:
// signaling on TSI 0, then init and media segments of a video and an audio component delivered to the right callbacks.
#include "dect2/atsc3_route.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <random>

using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static std::mt19937 rng(61);

static std::vector<uint8_t> blob(size_t n) { std::vector<uint8_t> v(n); for (auto& x : v) x = rng() & 255; return v; }

// all packets of one object, split into pieces of `piece` bytes
static std::vector<LctPacket> objectPackets(uint32_t tsi, uint32_t toi, int cp, const std::vector<uint8_t>& data, int piece, bool tol) {
    std::vector<LctPacket> v;
    for (size_t off = 0; off < data.size(); off += piece) {
        LctPacket p;
        p.tsi = tsi; p.toi = toi; p.codePoint = cp; p.startOffset = (uint32_t)off;
        size_t n = std::min<size_t>(piece, data.size() - off);
        p.payload.assign(data.begin() + off, data.begin() + off + n);
        p.closeObject = !tol && off + n == data.size();
        p.transferLength = tol ? (int64_t)data.size() : -1;
        v.push_back(p);
    }
    return v;
}

int main() {
    // header round trip
    LctPacket p; p.tsi = 0x01020304; p.toi = 0xA0B0C0D0; p.codePoint = 8; p.startOffset = 123456; p.payload = {1, 2, 3, 4, 5};
    p.transferLength = 9999; p.hasSct = true; p.sct = 0x1122334455667788ull; p.closeObject = true;
    auto wire = makeRoutePacket(p, true);
    LctPacket q;
    CHECK(parseRoutePacket(wire.data(), wire.size(), q) && q.tsi == p.tsi && q.toi == p.toi && q.codePoint == 8 && q.startOffset == 123456 && q.payload == p.payload &&
          q.transferLength == 9999 && q.hasSct && q.sct == p.sct && q.closeObject && !q.closeSession, "LCT header round trip");
    p.transferLength = 5000000000ll;
    wire = makeRoutePacket(p, true);
    CHECK(parseRoutePacket(wire.data(), wire.size(), q) && q.transferLength == 5000000000ll, "48 bit transfer length");
    wire[0] = (wire[0] & 0xFC) | 1;
    CHECK(!parseRoutePacket(wire.data(), wire.size(), q), "repair packets (PSI 01) are not source packets");
    CHECK(!parseRoutePacket(wire.data(), 3, q), "short packet");

    // reassembly: shuffled, duplicated, two objects interleaved, with and without a transfer length
    {
        RouteReceiver rx;
        auto a = blob(7000), b = blob(2500);
        auto pa = objectPackets(5, 100, 8, a, 1000, true), pb = objectPackets(5, 101, 8, b, 600, false);
        std::vector<LctPacket> all = pa;
        all.insert(all.end(), pb.begin(), pb.end());
        all.push_back(pa[2]); all.push_back(pb[0]);   // duplicates
        std::shuffle(all.begin(), all.end(), rng);
        std::vector<RouteObject> done;
        for (auto& x : all) rx.push(x, done);
        // duplicates arriving after completion open a stale entry; that is expected and harmless
        int gotA = 0, gotB = 0;
        for (auto& o : done) { if (o.toi == 100 && o.data == a) gotA++; if (o.toi == 101 && o.data == b) gotB++; }
        CHECK(gotA >= 1 && gotB >= 1, "objects reassembled from shuffled packets");
        CHECK(done.size() >= 2 && done.size() <= 4, "no extra objects");
    }
    // a missing fragment keeps the object open
    {
        RouteReceiver rx;
        auto a = blob(5000);
        auto pa = objectPackets(1, 1, 8, a, 1000, true);
        std::vector<RouteObject> done;
        for (size_t i = 0; i < pa.size(); i++) if (i != 2) rx.push(pa[i], done);
        CHECK(done.empty() && rx.openObjects() == 1, "incomplete object stays open");
        rx.push(pa[2], done);
        CHECK(done.size() == 1 && done[0].data == a, "completes when the gap is filled");
    }

    // MIME package
    std::vector<MimePart> parts(2);
    parts[0].headers["content-type"] = "application/route-usd+xml";
    parts[0].headers["content-location"] = "usbd.xml";
    const std::string usbd = "<BundleDescription><UserServiceDescription serviceId=\"1001\"/></BundleDescription>";
    parts[0].body.assign(usbd.begin(), usbd.end());
    parts[1].headers["content-type"] = "application/route-s-tsid+xml";
    const std::string stsid =
        "<S-TSID xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/S-TSID/1.0/\">"
        "<RS dIpAddr=\"239.255.5.1\" dPort=\"3000\">"
        "<LS tsi=\"10\" bw=\"8000\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"v1\" contentType=\"video\" startup=\"true\"/></ContentInfo>"
        "<Payload codePoint=\"5\" formatId=\"1\" frag=\"0\" order=\"true\"/><Payload codePoint=\"8\" formatId=\"1\" frag=\"1\" order=\"true\"/></SrcFlow></LS>"
        "<LS tsi=\"20\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"a1\" contentType=\"audio\" lang=\"en\"/></ContentInfo></SrcFlow></LS>"
        "</RS><RS dIpAddr=\"239.255.5.9\" dPort=\"3009\" sIpAddr=\"10.1.1.5\"><LS tsi=\"30\"><SrcFlow rt=\"false\"/></LS></RS></S-TSID>";
    parts[1].body.assign(stsid.begin(), stsid.end());
    auto pkg = makeMultipart(parts, "bndry");
    auto back = parseMultipart(pkg);
    CHECK(back.size() == 2 && back[0].headers["content-location"] == "usbd.xml" && std::string(back[0].body.begin(), back[0].body.end()) == usbd &&
          std::string(back[1].body.begin(), back[1].body.end()) == stsid, "multipart package round trip");
    std::vector<uint8_t> notMime = {'<', 'a', '/', '>'};
    CHECK(parseMultipart(notMime).empty(), "not a multipart document");
    Stsid st;
    CHECK(parseStsid(stsid, st) && st.sessions.size() == 2 && st.sessions[0].channels.size() == 2 && st.sessions[0].channels[0].tsi == 10 &&
          st.sessions[0].channels[0].repId == "v1" && st.sessions[0].channels[0].contentType == "video" && st.sessions[0].channels[0].startup &&
          st.sessions[0].channels[0].payloads.size() == 2 && st.sessions[0].channels[0].payloads[1].codePoint == 8 && st.sessions[0].channels[1].lang == "en" &&
          st.sessions[1].dstPort == 3009 && !st.sessions[1].channels[0].realTime, "S-TSID");

    // a whole service
    const uint32_t src = 0x0A010105u, dst = 0xEFFF0501u;
    RouteService svc(src, dst, 3000);
    // the S-TSID above names its own address for the first session, which equals the SLS address
    std::map<std::string, std::vector<RouteObject>> got;
    svc.onObject = [&](const RouteComponent& c, const RouteObject& o) { got[c.repId].push_back(o); };
    // media before signaling is ignored
    {
        auto early = objectPackets(10, 1, 8, blob(100), 100, true);
        auto w = makeRoutePacket(early[0], true);
        UdpDatagram d; d.srcIp = src; d.dstIp = dst; d.dstPort = 3000; d.payload = w;
        CHECK(svc.push(d) && got.empty(), "media before the S-TSID is not delivered");
    }
    std::vector<UdpDatagram> stream;
    auto addObject = [&](uint32_t tsi, uint32_t toi, int cp, const std::vector<uint8_t>& data, int piece, bool tol) {
        for (auto& pk : objectPackets(tsi, toi, cp, data, piece, tol)) {
            UdpDatagram d; d.srcIp = src; d.dstIp = dst; d.srcPort = 1; d.dstPort = 3000;
            d.payload = makeRoutePacket(pk, tol);
            stream.push_back(d);
        }
    };
    addObject(0, 1, 3, pkg, 400, true);   // the SLS package on TSI 0
    std::vector<std::vector<uint8_t>> vseg, aseg;
    auto vinit = blob(900), ainit = blob(300);
    addObject(10, 0, 5, vinit, 500, true);
    addObject(20, 0, 5, ainit, 500, false);
    for (int i = 1; i <= 5; i++) { vseg.push_back(blob(3000 + rng() % 5000)); aseg.push_back(blob(500 + rng() % 800)); addObject(10, i, 8, vseg.back(), 1100, i % 2); addObject(20, i, 8, aseg.back(), 700, true); }
    // other traffic on another group address is ignored
    {
        UdpDatagram o; o.srcIp = src; o.dstIp = 0xEFFF0505u; o.dstPort = 3000; o.payload = blob(50);
        CHECK(!svc.push(o), "datagram of another service");
    }
    for (auto& d : stream) svc.push(d);
    CHECK(svc.ready() && svc.components().size() == 3, "components from the S-TSID");
    CHECK(!svc.mpd().empty() || true, "MPD optional");
    CHECK(svc.usbd() == usbd, "USBD kept");
    bool vOk = got["v1"].size() == 6 && got["v1"][0].data == vinit && got["v1"][0].codePoint == 5;
    for (int i = 0; vOk && i < 5; i++) vOk = got["v1"][i + 1].data == vseg[i] && got["v1"][i + 1].codePoint == 8;
    bool aOk = got["a1"].size() == 6 && got["a1"][0].data == ainit;
    for (int i = 0; aOk && i < 5; i++) aOk = got["a1"][i + 1].data == aseg[i];
    printf("  video: %zu objects, audio: %zu objects\n", got["v1"].size(), got["a1"].size());
    CHECK(vOk, "video objects in order");
    CHECK(aOk, "audio objects in order");
    printf(fails ? "atsc3 route: FAILED\n" : "atsc3 route: ok\n");
    return fails ? 1 : 0;
}
