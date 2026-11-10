// MeshCore node (meshcore_node.h) and its transmitter (meshcore_tx.h).
// 1. Frames made by the real MeshCore firmware code: meshcore-dev/MeshCore at 81dbae5 (2026-10-15), its src/Mesh.cpp, Dispatcher.cpp,
//    Packet.cpp, Utils.cpp, Identity.cpp, helpers/BaseChatMesh.cpp and lib/ed25519 built on a computer behind a fake radio (SHA-256, HMAC and
//    AES from Apple CommonCrypto, not OnAir's), seeds 01..01 (firmware "Fw"), 02..02 (OnAir), 03..03 (a firmware repeater), clock
//    1790000000. The frames below are what it sent while it talked to OnAir's node; OnAir must read them, answer with the ACKs it expects,
//    and build byte-identical packets from the same inputs.
// 2. Loop-back: the node's frames through the transmit generator, noise and carrier offsets, the Mesh receiver's LoRa demodulator on the
//    MeshCore setting, and into a second node: adverts, a direct message with its path + ACK answer, a channel message, at every spreading
//    factor, bandwidth and coding rate of the presets, at low SNR and off the tuned frequency.
// Simulated only: nothing here has been on the air against a MeshCore radio.
#include "dect2/dsp/gen_util.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/mesh/mesh_lora.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_rx.h"
#include "dect2/mesh/meshcore_node.h"
#include "dect2/mesh/meshcore_tx.h"
#include "dect2/tx/tx.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::meshcore;

#include "../common/test_util.h"

static const struct { const char* k; const char* v; } kVec[] = {
    {"fw_pub",
     "8a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c"},
    {"oa_pub",
     "8139770ea87d175f56a35466c34c7ecccb8d8a91b4ee37a25df60f5b8fc9b394"},
    {"fw_prv",
     "58e86efb75fa4e2c410f46e16de9f6acae1a1703528651b69bc176c088bef36eb17a2a2cf3d4a41a8e4e18cc45c8656d558e"
     "ceddb0adb46bfa088a5f53bed252"},
    {"secret",
     "4181d7302557342bdb6d061c4b1eebea828ecb625c3368b7111680793307220b"},
    {"fw_advert",
     "11008a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c803bb16a4219403d58a9d446481d485d"
     "ecad0a1efc875d6dd2c92f085d6708fcb8611f65bc7f5ba6985f62974585ba3b4f6f4bafab944c2c6b0c47ec85e8b91cb058"
     "5904814677"},
    {"oa_advert",
     "11008139770ea87d175f56a35466c34c7ecccb8d8a91b4ee37a25df60f5b8fc9b394813bb16a6ee65333985ac5fb14845706"
     "4681247a49942c51ff68fbcd4e78454fd4ee8766ca8cd7d7a705fd55134f3f961474e6efeb301c805b41ea6229234e473310"
     "d802814f6e416972"},
    {"oa_msg_flood",
     "09008a81e242bcf7a593cc14381df51b921c1f862383d7c2ac544c8ec3c91b29673708c13f4d"},
    {"fw_path_ack",
     "2100818aad07f9fdda122e37b4530e1c7d2d40132fc2"},
    {"oa_path_back",
     "22008a8125cc7ffe66c725964f10974f2a741edaf90f"},
    {"oa_msg_direct",
     "0a008a81d89d2999e423be98dc8bfae9384dfb59a4efaaa021f8227c71f3e93b123f6b00b893"},
    {"fw_ack_direct",
     "0e00f4ee81df0076"},
    {"fw_expect_ack",
     "3d8e67c5"},
    {"oa_ack_to_fw",
     "0e003d8e67c50054"},
    {"fw_msg_direct",
     "0a00818a1561e280e7e74a1fca1dd24bb056774a104c1f72720f5d0b67a34af1cc38e98ee776"},
    {"fw_chan_public",
     "1500119a9f8d5d9895c17bd5e13b14ad5f394e8cd5bd33e4ee00e66f77510b02f738a7c289"},
    {"fw_chan_test",
     "1500d9addc851406fffcefed1144aaf70b69538c5af2167c9e07631609c8b12526c2b5231b"},
    {"fw_chan_priv",
     "15006373c2db7063444acd6e5150400808b565d155cc732f3f143f343ca39448d4c38f0f60"},
    {"rep_forward",
     "1501ed119a9f8d5d9895c17bd5e13b14ad5f394e8cd5bd33e4ee00e66f77510b02f738a7c289"},
    {"rep_pub",
     "ed4928c628d1c2c6eae90338905995612959273a5c63f93636c14614ac8737d1"},
};

static std::vector<uint8_t> V(const char* k) {
    std::string s;
    for (const auto& e : kVec) if (!std::strcmp(e.k, k)) s = e.v;
    std::vector<uint8_t> b;
    meshcrypto::hexDecode(s, b);
    return b;
}
static std::string H(const std::vector<uint8_t>& b) { return hexOf(b.data(), b.size()); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static Identity seedId(uint8_t b) {
    uint8_t s[32];
    std::memset(s, b, 32);
    Identity id;
    identityFromSeed(s, id);
    return id;
}

// a node with a fixed clock and random source
struct TNode {
    Node n;
    double base = 1790000000, t = 0;
    uint64_t r = 0x1234567;
    explicit TNode(uint8_t seed, const char* name, bool repeater = false) {
        n.unixClock = [this] { return (uint32_t)(base + t); };
        n.random = [this](uint8_t* d, size_t k) { for (size_t i = 0; i < k; i++) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; d[i] = (uint8_t)r; } };
        n.setIdentity(seedId(seed));
        NodeConfig c;
        c.name = name; c.repeater = repeater; c.turnaroundSec = 0;
        n.setConfig(c);
    }
    std::vector<Outgoing> drain(double upTo) {
        std::vector<Outgoing> v;
        Outgoing o;
        while (n.popDue(upTo, o)) { n.noteSent(o, upTo); v.push_back(o); }
        return v;
    }
};

static void firmwareVectors() {
    printf("frames of the MeshCore firmware code:\n");
    const Identity fw = seedId(1), oa = seedId(2);
    CHECK(H(std::vector<uint8_t>(fw.pub, fw.pub + 32)) == H(V("fw_pub")), "public key from seed 01 as the firmware's");
    CHECK(identityPrivateHex(fw) == H(V("fw_prv")), "64 byte private key as LocalIdentity::writeTo");
    CHECK(H(std::vector<uint8_t>(oa.pub, oa.pub + 32)) == H(V("oa_pub")), "public key from seed 02");
    Identity imp;
    std::string why;
    CHECK(identityFromPrivateHex(H(V("fw_prv")), imp, &why) && std::memcmp(imp.pub, fw.pub, 32) == 0, "import of the firmware's private key: %s", why.c_str());
    CHECK(!identityFromPrivateHex("00", imp) && !identityFromPrivateHex(std::string(128, 'z'), imp), "a malformed key is refused");
    uint8_t s1[32], s2[32];
    sharedSecret(oa, fw.pub, s1);
    sharedSecret(fw, oa.pub, s2);
    CHECK(H(std::vector<uint8_t>(s1, s1 + 32)) == H(V("secret")) && std::memcmp(s1, s2, 32) == 0, "shared secret as the firmware's ed25519_key_exchange");

    // the firmware's advert: parses, the signature checks, and our builder makes the same bytes from the same inputs
    const auto adv = V("fw_advert");
    Packet p;
    CHECK(parsePacket(adv.data(), adv.size(), p) && p.type() == TAdvert && p.flood(), "advert parses");
    AdvertData ad;
    CHECK(parseAdvertData(p.payload.data() + 100, p.payload.size() - 100, ad) && ad.name == "Fw" && ad.type == AdvChat, "advert data: '%s'", ad.name.c_str());
    {
        Packet mine = buildAdvert(fw, get32(p.payload.data() + 32), ad);
        floodOf(mine, 1);
        CHECK(H(mine.bytes()) == H(adv), "our advert for the same identity, time and name is the firmware's, byte for byte");
    }
    {   // our own advert (seed 02, name OnAir) was accepted by the firmware; it is also identical to what we build now
        const auto oadv = V("oa_advert");
        Packet q;
        CHECK(parsePacket(oadv.data(), oadv.size(), q), "OnAir advert parses");
        AdvertData a2; a2.name = "OnAir";
        Packet mine = buildAdvert(oa, get32(q.payload.data() + 32), a2);
        floodOf(mine, 1);
        CHECK(H(mine.bytes()) == H(oadv), "OnAir advert rebuilt");
    }

    // OnAir's node: hears the firmware's advert, then the firmware's PATH + ACK answer to the message OnAir sent
    TNode o(2, "OnAir");
    o.n.receive(adv.data(), adv.size(), 7, 0);
    CHECK(o.n.contacts().size() == 1 && o.n.contacts()[0].name == "Fw", "contact from the advert");
    o.n.receive(adv.data(), adv.size(), 7, 0);
    CHECK(o.n.contacts().size() == 1, "the same advert again changes nothing");
    {
        // our message as sent (oa_msg_flood): its timestamp, then the same message built here, then the firmware's answer
        const auto m = V("oa_msg_flood");
        Packet q;
        CHECK(parsePacket(m.data(), m.size(), q) && q.type() == TTxtMsg && q.flood(), "OnAir message parses");
        std::vector<uint8_t> plain;
        CHECK(macThenDecrypt(s2, q.payload.data() + 2, q.payload.size() - 2, plain) && plain.size() >= 5, "the firmware side decrypts it");
        const uint32_t ts = get32(plain.data());
        Packet mine;
        uint32_t ack = 0;
        CHECK(buildText(oa, fw.pub, s1, ts, 0, "hello from OnAir", mine, ack), "build");
        floodOf(mine, 1);
        CHECK(H(mine.bytes()) == H(m), "message rebuilt byte for byte");
        // the firmware's PATH packet carries the ACK we expect
        const auto pa = V("fw_path_ack");
        Packet pp;
        CHECK(parsePacket(pa.data(), pa.size(), pp) && pp.type() == TPath, "firmware PATH parses");
        std::vector<uint8_t> d;
        CHECK(macThenDecrypt(s1, pp.payload.data() + 2, pp.payload.size() - 2, d) && d.size() >= 6, "PATH decrypts");
        CHECK(d[0] == 0 && (d[1] & 0x0F) == TAck && get32(d.data() + 2) == ack, "PATH extra is our ACK %08x (got %08x)", ack, d.size() >= 6 ? get32(d.data() + 2) : 0);
        // and the direct message's ACK
        const auto md = V("oa_msg_direct");
        Packet qd;
        CHECK(parsePacket(md.data(), md.size(), qd) && qd.direct() && qd.hashCount() == 0, "second message is direct");
        CHECK(macThenDecrypt(s2, qd.payload.data() + 2, qd.payload.size() - 2, plain), "decrypts");
        uint32_t ack2 = 0;
        CHECK(buildText(oa, fw.pub, s1, get32(plain.data()), 0, "second, direct", mine, ack2), "build 2");
        const auto fa = V("fw_ack_direct");
        Packet pf;
        CHECK(parsePacket(fa.data(), fa.size(), pf) && pf.type() == TAck && pf.payload.size() == 6 && get32(pf.payload.data()) == ack2, "firmware's direct ACK is ours");
    }
    // the node itself: send, take the firmware's PATH + ACK, the message is delivered and the route learned
    {
        std::string w;
        CHECK(o.n.sendText(o.n.contacts()[0].pubHex(), "hello from OnAir", 0, &w), "send: %s", w.c_str());
        auto sent = o.drain(0);
        CHECK(sent.size() == 1, "one frame queued");
        // our frame differs in timestamp from the recorded one, so the recorded PATH does not match it: feed it a PATH for its own ACK
        Packet q;
        parsePacket(sent[0].bytes.data(), sent[0].bytes.size(), q);
        std::vector<uint8_t> plain;
        macThenDecrypt(s2, q.payload.data() + 2, q.payload.size() - 2, plain);
        uint8_t dg[32];
        meshcrypto::Sha256 h;
        size_t tl = 0;
        while (5 + tl < plain.size() && plain[5 + tl]) tl++;
        h.update(plain.data(), 5 + tl); h.update(oa.pub, 32); h.final(dg);
        const uint8_t rnd[4] = {1, 2, 3, 4};
        Packet r = buildPathReturn(oa.pub[0], fw.pub[0], s2, 0, {}, TAck, std::vector<uint8_t>(dg, dg + 6), rnd);
        floodOf(r, 1);
        const auto rb = r.bytes();
        o.n.receive(rb.data(), rb.size(), 7, 1);
        CHECK(o.n.messages().back().status == Message::Delivered, "delivered (status %d)", o.n.messages().back().status);
        CHECK(o.n.contacts()[0].outPathLen == 0, "route learned: zero hops");
        auto back = o.drain(2);
        CHECK(back.size() == 1 && back[0].bytes[0] == 0x22, "a PATH back, direct (as Mesh.cpp)");
    }
    // the firmware's direct message to us: shown once, answered with the ACK the firmware expects
    {
        const auto fm = V("fw_msg_direct");
        o.n.receive(fm.data(), fm.size(), 6, 3);
        CHECK(!o.n.messages().empty() && o.n.messages().back().text == "hello from the firmware" && o.n.messages().back().from == "Fw", "firmware message read");
        auto acks = o.drain(4);
        CHECK(acks.size() == 1 && acks[0].bytes.size() == 2 + 6, "one ACK");
        if (!acks.empty()) {
            Packet a;
            parsePacket(acks[0].bytes.data(), acks[0].bytes.size(), a);
            CHECK(a.type() == TAck && a.direct() && H(std::vector<uint8_t>(a.payload.begin(), a.payload.begin() + 4)) == H(V("fw_expect_ack")),
                  "ACK value is the firmware's expected_ack %s", H(V("fw_expect_ack")).c_str());
        }
        o.n.receive(fm.data(), fm.size(), 6, 5);
        CHECK(o.drain(6).empty(), "a repeat of the same frame is not answered again (table of packets seen)");
    }
    // channels: Public, the hashtag channel #test, a channel with a 32 byte key
    {
        Channel t, k;
        CHECK(makeChannel("#test", "", t) && channelKeyHex(t) == "9cd8fcf22a47333b591d96a2b848b73f", "hashtag key as docs/companion_protocol.md");
        std::string key32;
        for (int i = 0; i < 32; i++) { char b[3]; snprintf(b, sizeof b, "%02x", i); key32 += b; }
        CHECK(makeChannel("Priv", key32, k) && k.keyLen == 32, "32 byte key");
        CHECK(!makeChannel("Bad", "abcd", k), "a short key is refused");
        o.n.addChannel(t);
        makeChannel("Priv", key32, k);
        o.n.addChannel(k);
        const size_t before = o.n.messages().size();
        for (const char* key : {"fw_chan_public", "fw_chan_test", "fw_chan_priv"}) { const auto b = V(key); o.n.receive(b.data(), b.size(), 5, 7); }
        const auto& ms = o.n.messages();
        CHECK(ms.size() == before + 3, "three channel messages (%zu)", ms.size() - before);
        if (ms.size() == before + 3) {
            CHECK(ms[before].peer == "Public" && ms[before].from == "Fw" && ms[before].text == "public from firmware", "Public: %s", ms[before].text.c_str());
            CHECK(ms[before + 1].peer == "#test" && ms[before + 1].text == "hashtag from firmware", "#test");
            CHECK(ms[before + 2].peer == "Priv" && ms[before + 2].text == "thirty-two byte key", "Priv");
        }
    }
    // repeater: our node with the repeater's seed repeats the firmware's flood exactly as the firmware repeater did
    {
        TNode rp(3, "Rep", true);
        const auto b = V("fw_chan_public");
        rp.n.receive(b.data(), b.size(), 5, 0);
        auto o2 = rp.drain(10);
        CHECK(o2.size() == 1 && H(o2[0].bytes) == H(V("rep_forward")), "repeat of the flood = the firmware repeater's");
        rp.n.receive(o2[0].bytes.data(), o2[0].bytes.size(), 5, 11);
        rp.n.receive(b.data(), b.size(), 5, 11);
        CHECK(rp.drain(20).empty(), "not repeated twice");
        TNode off(3, "Rep", false);
        off.n.receive(b.data(), b.size(), 5, 0);
        CHECK(off.drain(10).empty(), "repeater mode off (the default): nothing is repeated");
        CHECK(!NodeConfig().repeater, "repeater off by default");
    }
}

static void packets() {
    printf("packet layer:\n");
    Packet p;
    p.header = (uint8_t)(TGrpTxt << 2 | RouteTransportFlood);
    p.transport[0] = 0x1234; p.transport[1] = 0xBEEF;
    p.pathLen = (uint8_t)(1 << 6 | 2);
    p.path = {1, 2, 3, 4};
    p.payload = {9, 8, 7};
    const auto b = p.bytes();
    Packet q;
    CHECK(parsePacket(b.data(), b.size(), q) && q.transport[0] == 0x1234 && q.transport[1] == 0xBEEF && q.hashSize() == 2 && q.hashCount() == 2 &&
          q.path == p.path && q.payload == p.payload, "transport codes and 2 byte hashes round trip");
    const uint8_t bad1[] = {0x15, 0xC1, 1, 2, 3};               // hash size 4: reserved
    const uint8_t bad2[] = {0x15, 0x02, 1, 2};                  // path runs into the end: no payload
    const uint8_t bad3[] = {0x15, 0x3F};                        // 63 hashes, nothing there
    CHECK(!parsePacket(bad1, sizeof bad1, q) && !parsePacket(bad2, sizeof bad2, q) && !parsePacket(bad3, sizeof bad3, q), "bad encodings refused");
    for (int n = 0; n < 2000; n++) {                            // random bytes never crash the node
        std::vector<uint8_t> r(1 + (size_t)(n * 7919 % 200));
        for (size_t i = 0; i < r.size(); i++) r[i] = (uint8_t)((n * 31 + i * 17) ^ (i >> 1));
        static TNode z(9, "Z", true);
        z.n.receive(r.data(), r.size(), 0, n * 0.01);
        z.drain(n * 0.01 + 100);
    }
    // airtime: SF7 125 kHz 4/5, preamble 8, 20 bytes, CRC: 56.6 ms (Semtech calculator); duty-cycle sub-bands
    Radio r;
    r.sf = 7; r.bwHz = 125000; r.cr = 5; r.preamble = 8;
    CHECK(std::fabs(airtimeSec(r, 20) - 0.05658) < 0.0005, "airtime %.5f", airtimeSec(r, 20));
    r.sf = 12; r.bwHz = 125000; r.cr = 5;
    CHECK(std::fabs(airtimeSec(r, 20) - 1.31891) < 0.002, "SF12 with LDRO %.5f", airtimeSec(r, 20));   // 40.25 symbols of 32.768 ms
    CHECK(dutyCycleRule(869.618e6).limit == 0.10 && dutyCycleRule(868.1e6).limit == 0.01 && dutyCycleRule(869.0e6).limit == 0.001 &&
          dutyCycleRule(433.65e6).limit == 0.10 && dutyCycleRule(910.525e6).limit == 0, "duty-cycle table");
    DutyMeter dm;
    dm.add(0, 1.0); dm.add(1800, 2.0);
    CHECK(std::fabs(dm.used(1900) - 3.0) < 1e-9 && std::fabs(dm.used(3700) - 2.0) < 1e-9, "sliding hour");
    // save / load
    TNode a(5, "Ann");
    const auto adv = V("fw_advert");
    a.n.receive(adv.data(), adv.size(), 3, 0);
    Channel c;
    makeChannel("#onair", "", c);
    a.n.addChannel(c);
    a.n.sendChannel("#onair", "kept text with spaces % and\nnewline", 0);
    Node nb;
    CHECK(nb.load(a.n.save()), "load");
    CHECK(nb.identity().valid && std::memcmp(nb.identity().pub, a.n.identity().pub, 32) == 0, "identity kept");
    CHECK(nb.contacts().size() == 1 && nb.contacts()[0].name == "Fw" && nb.channels().size() == 2 && nb.channels()[1].name == "#onair", "contacts and channels kept");
    CHECK(!nb.messages().empty() && nb.messages().back().text == a.n.messages().back().text, "message text kept");
}

// ---------------------------------------------------------------- loop-back through the radio path

struct Air {
    double rate;
    MeshReceiver rx;
    std::vector<std::vector<uint8_t>> got;
    Air(const Radio& r, double rateHz) : rate(rateHz) {
        MeshLoraSettings s;
        s.name = "MeshCore"; s.freqHz = 0; s.sf = r.sf; s.bwHz = r.bwHz; s.cr = r.cr; s.syncWord = 0x12; s.preamble = r.preamble;
        s.ldro = meshLoraLdro(r.sf, r.bwHz);
        rx.setProtocols(2);
        rx.setCustom(true, s);
        rx.configure(rate);
        rx.setSignalOffset(-meshTuning().tuneOffsetHz);
        rx.setTunedHz(869.618e6);
        rx.setFrameCallback([this](int, const std::vector<uint8_t>& p, float, double) { got.push_back(p); });
    }
    // frames through the transmitter (placed cfo off the tuned frequency), with noise at snrDb in the LoRa bandwidth
    std::vector<std::vector<uint8_t>> send(const Radio& r, const std::vector<std::vector<uint8_t>>& frames, double cfoHz, double snrDb, uint32_t seed, double sroPpm = 0) {
        TxRequest q;
        q.payload = meshCoreTxPayload(frames);
        q.opt["meshcore.sf"] = r.sf; q.opt["meshcore.bwHz"] = r.bwHz; q.opt["meshcore.cr"] = r.cr; q.opt["meshcore.preamble"] = r.preamble;
        q.opt["meshcore.ifHz"] = -meshTuning().tuneOffsetHz + cfoHz;
        std::string err;
        auto g = makeMeshCoreTxGenerator(q, rate, err);
        CHECK(g != nullptr, "generator: %s", err.c_str());
        if (!g) return {};
        std::vector<cf32> x;
        std::vector<cf32> blk(16384);
        while (!g->finished()) { g->generate(blk.data(), blk.size()); x.insert(x.end(), blk.begin(), blk.end()); }
        x.resize(x.size() + (size_t)(0.1 * rate), cf32(0, 0));
        if (sroPpm != 0) {   // the transmitter's clock off by sroPpm: linear interpolation onto the receiver's sample times
            std::vector<cf32> y((size_t)((double)x.size() / (1 + sroPpm * 1e-6)) - 2);
            for (size_t i = 0; i < y.size(); i++) {
                const double t = (double)i * (1 + sroPpm * 1e-6);
                const size_t k = (size_t)t;
                const float fr = (float)(t - (double)k);
                y[i] = x[k] * (1 - fr) + x[k + 1] * fr;
            }
            x.swap(y);
        }
        // amplitude 0.7 in the band; noise sigma for the SNR in the LoRa bandwidth
        const double sig = 0.7 * 0.7;
        const double n0 = sig / std::pow(10.0, snrDb / 10.0) * rate / r.bwHz;
        genutil::NoiseSource ns(seed);
        ns.add(x.data(), x.size(), (float)std::sqrt(n0 / 2));
        got.clear();
        for (size_t i = 0; i < x.size(); i += 32768) rx.feed(x.data() + i, std::min<size_t>(32768, x.size() - i));
        return got;
    }
};

static std::vector<std::vector<uint8_t>> bytesOf(const std::vector<Outgoing>& v) { std::vector<std::vector<uint8_t>> o; for (const auto& e : v) o.push_back(e.bytes); return o; }

// one conversation through the air: adverts, a direct message and its PATH + ACK, a channel message. True when everything arrived.
static bool conversation(const Radio& r, double rate, double cfo, double snr, uint32_t seed, bool verbose, double sro = 0) {
    Air air(r, rate);
    TNode a(5, "Ann"), b(6, "Bob");
    a.n.config().radio = r; b.n.config().radio = r;
    a.n.sendAdvert(0); b.n.sendAdvert(0);
    for (auto& f : air.send(r, bytesOf(a.drain(0)), cfo, snr, seed, sro)) b.n.receive(f.data(), f.size(), 0, 1);
    for (auto& f : air.send(r, bytesOf(b.drain(0)), -cfo, snr, seed + 1, -sro)) a.n.receive(f.data(), f.size(), 0, 1);
    if (a.n.contacts().size() != 1 || b.n.contacts().size() != 1) { if (verbose) printf("  adverts lost\n"); return false; }
    a.n.sendText(a.n.contacts()[0].pubHex(), "loop-back over LoRa", 2);
    for (auto& f : air.send(r, bytesOf(a.drain(2)), cfo, snr, seed + 2, sro)) b.n.receive(f.data(), f.size(), 0, 3);
    const bool msg = !b.n.messages().empty() && b.n.messages().back().text == "loop-back over LoRa";
    for (auto& f : air.send(r, bytesOf(b.drain(10)), -cfo, snr, seed + 3, -sro)) a.n.receive(f.data(), f.size(), 0, 11);
    const bool ack = !a.n.messages().empty() && a.n.messages().back().status == Message::Delivered && a.n.contacts()[0].outPathLen == 0;
    b.n.sendChannel("Public", "channel over LoRa", 12);
    for (auto& f : air.send(r, bytesOf(b.drain(12)), -cfo, snr, seed + 4, -sro)) a.n.receive(f.data(), f.size(), 0, 13);
    const bool ch = !a.n.messages().empty() && a.n.messages().back().text == "channel over LoRa" && a.n.messages().back().from == "Bob";
    if (verbose && !(msg && ack && ch)) printf("  message %d ack %d channel %d\n", msg, ack, ch);
    return msg && ack && ch;
}

static void loopBack() {
    printf("loop-back (simulated air):\n");
    // every spreading factor, bandwidth and coding rate the MeshCore presets use (and the rest of SF7 - SF12 x 4/5 - 4/8)
    int pass = 0, total = 0;
    for (double bw : {62500.0, 125000.0, 250000.0})
        for (int sf = 7; sf <= 12; sf++)
            for (int cr = 5; cr <= 8; cr++) {
                if (sf >= 11 && cr != 5 && cr != 8) continue;          // the long ones: the two coding rates the presets use
                Radio r;
                r.sf = sf; r.bwHz = bw; r.cr = cr; r.preamble = sf <= 8 ? 32 : 16;
                const bool ok = conversation(r, std::max(bw * 8, 1e6), 0.04 * bw, 5, (uint32_t)(sf * 100 + cr), false);
                total++; pass += ok;
                if (!ok) printf("  SF%d %.1f kHz 4/%d: FAILED\n", sf, bw / 1e3, cr);
            }
    printf("  %d of %d settings: adverts, message, path + ACK, channel all through\n", pass, total);
    CHECK(pass == total, "every setting");
    // the EU/UK Narrow preset (SF8, 62.5 kHz, 4/8): noise sweep and carrier offsets
    Radio eu;
    for (double snr : {10.0, 0.0, -5.0}) for (double cfo : {0.0, 2500.0, -5000.0}) {
        const bool ok = conversation(eu, 1e6, cfo, snr, 77, true);
        printf("  EU Narrow SNR %+5.1f dB, offset %+6.0f Hz: %s\n", snr, cfo, ok ? "through" : "LOST");
        CHECK(ok, "EU Narrow at %.1f dB, %.0f Hz", snr, cfo);
    }
    for (double sro : {20.0, -20.0}) {   // crystals 20 ppm off (the frames are longer or shorter than the receiver's clock says)
        Radio slow; slow.sf = 11; slow.bwHz = 250000; slow.cr = 5; slow.preamble = 16;   // the old EU/UK preset: long frames, the most drift
        const bool ok = conversation(eu, 1e6, 1200, 3, 91, true, sro) && conversation(slow, 2e6, -3000, 3, 93, true, sro);
        printf("  clock error %+.0f ppm (SF8 / 62.5 kHz and SF11 / 250 kHz): %s\n", sro, ok ? "through" : "LOST");
        CHECK(ok, "clock error %.0f ppm", sro);
    }
}

// the transmit window's row: the list entry MeshCore offers it, it ends by itself, a typed line goes to the Public channel
static void txRow() {
    printf("transmit row:\n");
    const TxModeSpec* m = txModeById("meshcore");
    CHECK(m && txModeForList(20, "meshcore") == m && txPayloadKind("meshcore") == TxPayloadKind::Keyboard && txModeFinite(*m), "row, list entry, kind, finite");
    if (!m) return;
    TxRequest r;
    r.text = "typed in the Transmit window";
    r.opt["meshcore.ifHz"] = -meshTuning().tuneOffsetHz;
    std::string err;
    auto g = makeTxGenerator(*m, r, 1e6, err);
    CHECK(g != nullptr, "generator: %s", err.c_str());
    if (!g) return;
    Radio eu;
    Air air(eu, 1e6);
    std::vector<cf32> x, blk(16384);
    int guard = 0;
    while (!g->finished() && guard++ < 2000) { g->generate(blk.data(), blk.size()); x.insert(x.end(), blk.begin(), blk.end()); }
    CHECK(g->finished(), "the transmission ends by itself");
    x.resize(x.size() + 100000, cf32(0, 0));
    for (size_t i = 0; i < x.size(); i += 32768) air.rx.feed(x.data() + i, std::min<size_t>(32768, x.size() - i));
    TNode a(5, "Ann");
    for (auto& f : air.got) a.n.receive(f.data(), f.size(), 0, 1);
    CHECK(!a.n.messages().empty() && a.n.messages().back().from == "OnAir" && a.n.messages().back().text == r.text, "Public channel line received");
}

int main() {
    firmwareVectors();
    txRow();
    packets();
    loopBack();
    return testSummary("mesh_meshcore_node");
}
