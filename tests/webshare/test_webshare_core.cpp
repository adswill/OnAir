// OnAir Web setup (app/webshare/webshare_core.*): saved choices, the capture plan, decoder rows for a range, the server and listing settings files, the
// step 2 state machine, the test lines, the QR code. No window, no processes.
#include "../../app/webshare/webshare_core.h"
#include "../../app/webshare/qr.h"
#include "../../server/server_config.h"
#include "../../server/web/web_config.h"
#include "../../server/web/web_directory_proto.h"
#include "../../server/web/web_policy.h"
#include "../../server/web/web_ranges.h"
#include "../common/test_util.h"
#include <cmath>
#include <string>
using namespace webshare;
#define CHK(c) CHECK(c, "%s", #c)

static RigCaps rtl() { RigCaps r; r.name = "RTL-SDR Blog V4"; r.minHz = 24e6; r.maxHz = 1766e6; r.maxRate = 3.2e6; return r; }
static RigCaps hackrf() { RigCaps r; r.name = "HackRF Pro"; r.minHz = 1e6; r.maxHz = 6e9; r.maxRate = 20e6; return r; }

static const DecoderRow* row(const std::vector<DecoderRow>& v, const char* k) { for (const auto& r : v) if (r.kind == k) return &r; return nullptr; }

static void testPrefsRoundTrip() {
    ShareSettings s;
    s.rig = "HackRF Pro (serial 0000a0)"; s.wholeSpan = false; s.centreMHz = 127.5; s.spanMHz = 19; s.rateAuto = false; s.rateMsps = 22.5; s.gainManual = true; s.gainDb = 41;
    s.dataOff = {"ft8", "wspr"}; s.restrictedOn = {"dmr"}; s.legalAck = true; s.demods = "am,usb"; s.maxUsers = 12; s.userKbps = 256; s.audioOnly = true; s.limitToRange = true;
    s.password = "correct horse"; s.link = "own"; s.ownUrl = "https://radio.example.org/"; s.listPublic = true; s.name = "Roof"; s.desc = "FM and air"; s.loc = "54.69,25.28"; s.contact = "me@example.org";
    s.radioLabel = "HackRF Pro"; s.locDecimals = 1; s.reach = true; s.dirUrl = "https://web.on-air.dev";
    const ShareSettings b = settingsFromText(settingsToText(s));
    CHK(b.rig == s.rig && !b.wholeSpan && b.centreMHz == 127.5 && b.spanMHz == 19 && !b.rateAuto && b.rateMsps == 22.5 && b.gainManual && b.gainDb == 41);
    CHK(b.dataOff == s.dataOff && b.restrictedOn == s.restrictedOn && b.legalAck && b.demods == "am,usb" && b.maxUsers == 12 && b.userKbps == 256 && b.audioOnly && b.limitToRange);
    CHK(b.password == s.password && b.link == "own" && b.ownUrl == s.ownUrl && b.listPublic && b.name == "Roof" && b.desc == "FM and air" && b.loc == "54.69,25.28" && b.contact == s.contact);
    CHK(b.radioLabel == "HackRF Pro" && b.locDecimals == 1 && b.reach);
    // garbage lines and bad numbers keep the defaults
    const ShareSettings d = settingsFromText("centre_mhz=abc\nmax_users=-5\nlink=evil\nwhat\n=x\nloc_decimals=99\n");
    CHK(d.centreMHz == 98.0 && d.maxUsers == 1 && d.link == "tunnel" && d.locDecimals == 5);
    CHK(settingsToText(ShareSettings{}) == settingsToText(settingsFromText("")));
}

static void testPlan() {
    ShareSettings s;
    Plan p = resolvePlan(s, hackrf());   // whole span: the radio's full rate around the default centre
    CHK(p.rateHz == 20e6 && p.centreHz == 98e6 && std::fabs(p.loHz - (98e6 - 9.6e6)) < 1 && std::fabs(p.hiHz - (98e6 + 9.6e6)) < 1 && p.warning.empty());
    p = resolvePlan(s, rtl());
    CHK(p.rateHz == 3.2e6);
    RigCaps st = rtl(); st.steadyRate = 2.4e6;   // a link that cannot carry the maximum
    CHK(resolvePlan(s, st).rateHz == 2.4e6);
    RigCaps unk; unk.name = "Mystery";
    CHK(resolvePlan(s, unk).rateHz == 2.4e6);
    // a span: the usable part is 96 % of the capture, so the rate is span / 0.96 rounded up
    ShareSettings f = s;
    applyPreset(f, rangePresets()[0]);   // FM broadcast
    p = resolvePlan(f, hackrf());
    CHK(!f.wholeSpan && std::fabs(f.centreMHz - 97.75) < 1e-9 && p.rateHz >= 20.5e6 / 0.96 - 1 && p.rateHz < 20.5e6 / 0.96 + 1e4 + 1 && p.loHz <= 87.5e6 && p.hiHz >= 108e6);
    // no clamps: the owner's rate is used as it is, with a sentence when it is above what the radio reports
    ShareSettings o = s; o.rateAuto = false; o.rateMsps = 61.44;
    p = resolvePlan(o, hackrf());
    CHK(p.rateHz == 61.44e6 && p.warning.find("nothing is limited") != std::string::npos);
    o.centreMHz = 5000; p = resolvePlan(o, rtl());
    CHK(p.centreHz == 5000e6 && p.warning.find("outside") != std::string::npos);
    CHK(resolvePlan(s, RigCaps{"x", "", 0, 0, 0, 0, 0, false, true, true}).device == "test");
    CHK(defaultCentreMHz(hackrf()) == 98.0);
    RigCaps hf; hf.minHz = 0.1e6; hf.maxHz = 30e6;
    CHK(std::fabs(defaultCentreMHz(hf) - 15.05) < 0.01);
    CHK(rangeSpecMHz(88e6, 108e6) == "88-108");
    // presets are sane
    for (const auto& pr : rangePresets()) CHK(pr.centreHz > 0 && pr.spanHz > 0 && std::string(pr.label).size() > 3);
}

static void testRigs() {
    std::vector<dect2::DeviceInfo> v;
    dect2::DeviceInfo syn; syn.kind = dect2::DeviceInfo::Synthetic; syn.name = "Test signal (no radio)";
    dect2::DeviceInfo file; file.kind = dect2::DeviceInfo::File; file.name = "File: x.iq";
    dect2::DeviceInfo snd; snd.kind = dect2::DeviceInfo::Audio; snd.name = "Sound card";
    dect2::DeviceInfo net; net.kind = dect2::DeviceInfo::Soapy; net.name = "rtl_tcp 10.0.0.2 (network)"; net.maxRateHz = 2.4e6;
    dect2::DeviceInfo usb; usb.kind = dect2::DeviceInfo::HackRF; usb.name = "HackRF Pro"; usb.maxRateHz = 20e6; usb.minFreqHz = 1e6; usb.maxFreqHz = 6e9;
    v = {syn, file, snd, net, usb};
    const auto r = shareableRigs(v);
    CHK(r.size() == 3 && r[0].name == "HackRF Pro" && r[1].network && r[2].test);   // radios first, network next, the test signal last; no files or sound cards
    CHK(capsLine(r[0]).find("20 Msps") != std::string::npos && capsLine(r[0]).find("MHz") != std::string::npos);
}

static void testDecoders() {
    ShareSettings s;
    Plan fm = resolvePlan(s, hackrf());                          // 88.4-107.6 MHz
    auto rows = decoderRows(s, fm);
    CHK(row(rows, "rds") && row(rows, "rds")->possible && row(rows, "rds")->on);
    CHK(row(rows, "adsb") && !row(rows, "adsb")->possible && row(rows, "adsb")->why.find("1089") != std::string::npos);
    CHK(row(rows, "ft8") && !row(rows, "ft8")->possible);
    CHK(row(rows, "dmr") && !row(rows, "dmr")->on && row(rows, "dmr")->restricted && row(rows, "dmr")->needsCodec);
    CHK(row(rows, "wfm") && row(rows, "wfm")->isDemod && row(rows, "wfm")->on);
    // switching things off and the group buttons
    s.dataOff.insert("rds");
    CHK(!row(decoderRows(s, fm), "rds")->on);
    setGroup(s, "Data decoders", false);
    CHK(s.dataOff.size() == 12);
    setGroup(s, "Data decoders", true);
    CHK(s.dataOff.empty());
    // restricted decoders need the legal notice
    setGroup(s, "Needs your decision (legal notice)", true);
    CHK(s.restrictedOn.empty());
    s.legalAck = true;
    setGroup(s, "Needs your decision (legal notice)", true);
    CHK(s.restrictedOn.count("dmr") && row(decoderRows(s, fm), "dmr")->on);
    s.legalAck = false;
    CHK(!row(decoderRows(s, fm), "dmr")->on);                      // the notice is withdrawn: they are off again
    // a range that holds a band makes that decoder possible
    ShareSettings air; applyPreset(air, rangePresets()[1]);      // airband
    CHK(row(decoderRows(air, resolvePlan(air, hackrf())), "acars")->possible);
    ShareSettings ad; applyPreset(ad, rangePresets()[6]);        // 1090 MHz
    CHK(row(decoderRows(ad, resolvePlan(ad, hackrf())), "adsb")->possible && !row(decoderRows(ad, resolvePlan(ad, hackrf())), "rds")->possible);
    CHK(cpuScore(decoderRows(ShareSettings{}, fm)) > 0);
    // every kind the window offers is known to the server's policy (a misspelled kind would be silently ignored there)
    for (const auto& r : rows) if (!r.isDemod) CHK(onair_web::decoderKindClassified(r.kind));
    for (const auto& r : rows) if (!r.isDemod) CHK((onair_web::decoderClass(r.kind) != onair_web::DecoderClass::Data) == r.restricted);
}

// the settings file must reach the real server code: parse it with the server's own loaders
static void testServerConf() {
    ShareSettings s;
    s.maxUsers = 20; s.userKbps = 128; s.audioOnly = true; s.password = "longpassword"; s.dataOff = {"ft8", "aprs"}; s.demods = "am,wfm";
    s.legalAck = true; s.restrictedOn = {"pocsag"}; s.limitToRange = true; s.gainManual = true; s.gainDb = 33;
    ShareSettings d = s; applyPreset(d, rangePresets()[0]);
    d.dataOff = s.dataOff; d.demods = s.demods; d.legalAck = true; d.restrictedOn = s.restrictedOn; d.limitToRange = true; d.password = s.password; d.audioOnly = true; d.maxUsers = 20; d.userKbps = 128;
    const Plan p = resolvePlan(d, hackrf());
    const std::string conf = buildServerConf(d, p, 8075, 8766, "", "0123456789abcdef0123456789abcdef");
    onair_server::ConfigFile c;
    c.parse(conf);
    CHK(c.get("device") == "HackRF Pro" && c.flag("autostart", false) && c.get("mode") == "fm");
    CHK(std::fabs(c.num("freq_mhz", 0) - 97.75) < 1e-6 && std::fabs(c.num("rate_msps", 0) - p.rateHz / 1e6) < 1e-6 && c.num("gain_db", 0) == 33);
    CHK(c.num("port", 0) == 8766 && !c.flag("lan", true));
    bool madeAdmin = false;
    onair_web::WebSettings w = onair_web::loadWebSettings(c, madeAdmin);
    CHK(w.on && w.server.port == 8075 && w.server.bind == "127.0.0.1" && w.server.trustProxy && !madeAdmin);
    CHK(w.limits.maxUsers == 20 && w.limits.maxKbpsUser == 128 && !w.limits.spectrum);
    CHK(w.server.auth.mode == onair_web::AuthMode::Password && w.server.auth.password == "longpassword");
    CHK(w.server.auth.adminToken == "0123456789abcdef0123456789abcdef");
    onair_web::WebPolicy pol;
    pol.load(c);
    CHK(!pol.decoderAllowed("ft8") && !pol.decoderAllowed("aprs") && pol.decoderAllowed("adsb") && pol.decoderAllowed("pocsag") && !pol.decoderAllowed("dmr"));
    CHK(pol.demodAllowed(onair_web::Demod::Am) && pol.demodAllowed(onair_web::Demod::Wfm) && !pol.demodAllowed(onair_web::Demod::Usb));
    onair_web::WebRanges rg;
    std::vector<std::string> warn;
    rg.load(c, &warn);
    CHK(warn.empty());
    CHK(!c.getAll("web_allow_range").empty());
    // without the legal notice a restricted decoder is never switched on
    ShareSettings n = d; n.legalAck = false;
    onair_server::ConfigFile c2; c2.parse(buildServerConf(n, p, 8075, 8766, "", ""));
    onair_web::WebPolicy pol2; pol2.load(c2);
    CHK(!pol2.decoderAllowed("pocsag") && c2.get("web_decoders_on").empty() && c2.get("web_admin_token").empty());
    // open access, LAN link, defaults
    ShareSettings o; o.link = "lan";
    onair_server::ConfigFile c3; c3.parse(buildServerConf(o, resolvePlan(o, rtl()), 8073, 8765, "", ""));
    CHK(c3.get("web_bind") == "0.0.0.0" && c3.get("web_auth") == "open" && c3.get("web_spectrum") == "1");
    // the tunnel case behind the proxy: this is what makes the WebSocket's https Origin acceptable
    CHK(c3.flag("web_trust_proxy", false));
}

static void testDirectoryConf() {
    ShareSettings s; s.name = "Roof"; s.loc = "54.6872,25.2797"; s.desc = "FM"; s.radioLabel = ""; s.reach = true;
    const Plan p = resolvePlan(s, hackrf());
    const std::string conf = buildDirectoryConf(s, p, 8073, "https://a-b.trycloudflare.com", "abc123def456", "/tmp/x/web_directory.key");
    onair_server::ConfigFile c; c.parse(conf);
    CHK(c.flag("web_directory", false) && c.get("web_directory_url") == "https://web.on-air.dev" && c.get("web_public_url") == "https://a-b.trycloudflare.com");
    CHK(c.get("web_directory_radio") == "HackRF Pro" && c.get("web_directory_ack") == "abc123def456" && c.flag("web_directory_reach", false) && c.get("web_directory_freq") == coverageText(p));
    bool made2 = false;
    onair_web::WebSettings w = onair_web::loadWebSettings(c, made2);
    CHK(w.directory.on && w.directory.loc == "54.6872,25.2797" && w.directory.locDecimals == 2 && w.directory.keyFile == "/tmp/x/web_directory.key");
    // the coverage text is what dirParseFreqMHz understands
    std::vector<std::pair<long long, long long>> fr; std::string e;
    CHK(onair_web::dirParseFreqMHz(coverageText(p), fr, e) && fr.size() == 1 && fr[0].first > 80000000 && fr[0].second < 120000000);
    // validity
    ShareSettings bad = s; std::string why;
    CHK(settingsValid(bad, why));
    bad.name = ""; CHK(!settingsValid(bad, why));
    bad = s; bad.name = std::string(49, 'a'); CHK(!settingsValid(bad, why));
    bad = s; bad.loc = "north,east"; CHK(!settingsValid(bad, why) && !why.empty());
    bad = s; bad.password = "short"; CHK(!settingsValid(bad, why));
    bad = s; bad.desc = "two\nlines"; CHK(!settingsValid(bad, why));
    bad = s; bad.link = "own"; bad.ownUrl = "http://nope"; CHK(!settingsValid(bad, why));
    bad.ownUrl = "https://radio.example.org"; CHK(settingsValid(bad, why));
    bad = s; bad.centreMHz = -1; CHK(!settingsValid(bad, why));
    bad = s; bad.loc = "KO24pq"; CHK(settingsValid(bad, why));
}

static std::string statusJson(const char* state, const char* code, bool ok, int fails, bool soon) {
    char b[1500];
    snprintf(b, sizeof b, "{\"state\":\"%s\",\"ok\":%s,\"http\":0,\"code\":\"%s\",\"message\":\"msg %s\",\"advice\":\"do this\",\"retry_soon\":%s,\"attempts\":%d,\"fails\":%d,\"last_try\":100,\"last_ok\":%d,\"next_try\":110,\"have_skew\":false,\"skew_sec\":0,\"id\":\"onair-1\",\"url\":\"u\",\"directory\":\"d\"}",
             state, ok ? "true" : "false", code, code, soon ? "true" : "false", fails + 1, fails, ok ? 100 : 0);
    return b;
}

static void testStep2() {
    Step2State st;
    CHK(st.s == Step2::Off);
    st.s = Step2::Previewing;
    step2PreviewResult(st, "error=the location must be \"latitude,longitude\"", false);
    CHK(st.s == Step2::PreviewFailed && st.problem.message.find("latitude") != std::string::npos && !st.problem.advice.empty());
    st.s = Step2::Previewing;
    step2PreviewResult(st, "ack=9f3c01a7b2de\n---\nOnAir Web directory: every 300 s ...\n  {json}\n", true);
    CHK(st.s == Step2::PreviewReady && st.ack == "9f3c01a7b2de" && st.preview.find("every 300 s") != std::string::npos && st.problem.message.empty());
    st.s = Step2::Previewing;
    step2PreviewResult(st, "", false);
    CHK(st.s == Step2::PreviewFailed);
    // confirm only from a ready preview
    Step2State z; step2Confirm(z, 5); CHK(z.s == Step2::Off);
    step2PreviewResult(st, "ack=aaaaaaaaaaaa\n---\ntext\n", true);
    step2Confirm(st, 1000);
    CHK(st.s == Step2::Listing && st.startedAt == 1000);
    // nothing reported yet: wait, then say so
    step2Status(st, "", true, 1010);
    CHK(st.s == Step2::Listing);
    step2Status(st, "", true, 1050);
    CHK(st.s == Step2::ListFailed && st.problem.code == "silent");
    // success
    step2PreviewResult(st, "ack=aaaaaaaaaaaa\n---\ntext\n", true); step2Confirm(st, 2000);
    step2Status(st, statusJson("sending", "", false, 0, false), true, 2001);
    CHK(st.s == Step2::Listing);
    step2Status(st, statusJson("ok", "ok", true, 0, false), true, 2002);
    CHK(st.s == Step2::Listed && st.lastOk == 100 && st.problem.message.empty());
    // a hiccup after success shows the problem but does not pretend to be listed
    step2Status(st, statusJson("error", "server", false, 1, false), true, 2400);
    CHK(st.s == Step2::ListFailed && st.problem.code == "server" && !st.problem.advice.empty());
    step2Status(st, statusJson("ok", "ok", true, 0, false), true, 2500);   // and it recovers by itself
    CHK(st.s == Step2::Listed);
    // a temporary failure (a new address not in DNS yet) is retried quietly for a while, a permanent one is shown at once
    step2PreviewResult(st, "ack=aaaaaaaaaaaa\n---\ntext\n", true); step2Confirm(st, 3000);
    step2Status(st, statusJson("error", "resolving", false, 2, true), true, 3010);
    CHK(st.s == Step2::Listing && st.problem.temporary && st.fails == 2);
    step2Status(st, statusJson("error", "resolving", false, 8, true), true, 3100);
    CHK(st.s == Step2::ListFailed);
    step2PreviewResult(st, "ack=aaaaaaaaaaaa\n---\ntext\n", true); step2Confirm(st, 4000);
    step2Status(st, statusJson("error", "invalid", false, 1, false), true, 4005);
    CHK(st.s == Step2::ListFailed && st.problem.code == "invalid");
    // the listing program died
    step2PreviewResult(st, "ack=aaaaaaaaaaaa\n---\ntext\n", true); step2Confirm(st, 5000);
    step2Status(st, "", false, 5003);
    CHK(st.s == Step2::ListFailed && st.problem.code == "stopped");
    step2Cancel(st);
    CHK(st.s == Step2::Off && st.preview.empty());
    CHK(std::string(step2Name(Step2::Listed)) == "listed");
}

static void testTestLines() {
    TestStep t;
    CHK(parseTestLine("{\"step\":\"public\",\"ok\":false,\"ms\":12004,\"http\":0,\"code\":\"public\",\"message\":\"m\",\"advice\":\"a\",\"skew_sec\":null}", t));
    CHK(t.step == "public" && !t.ok && t.ms == 12004 && t.advice == "a" && !t.done);
    CHK(parseTestLine("{\"step\":\"done\",\"ok\":true,\"ms\":0,\"http\":0,\"code\":\"done\",\"message\":\"\",\"advice\":\"\",\"skew_sec\":null}", t) && t.done);
    CHK(!parseTestLine("garbage", t));
    CHK(std::string(testStepTitle("local")).find("computer") != std::string::npos);
    ShareStats s;
    CHK(parseStats("{\"users\":3,\"max_users\":8,\"cpu_pct\":12.5,\"cpu_cap_pct\":75,\"kbps\":400,\"max_kbps\":8000,\"admitted\":3,\"rejected\":0,\"kicked\":0,\"iq_dropped\":2,\"capture\":{\"centre_hz\":97750000,\"rate_hz\":20000000},\"decoders\":[]}", s));
    CHK(s.users == 3 && s.maxUsers == 8 && s.kbps == 400 && s.iqDropped == 2 && s.rateHz == 20e6 && s.ok);
    CHK(!parseStats("nope", s));
}

static void testQr() {
    // format bits for error correction level M (the standard's table)
    CHK(qrFormatBitsM(0) == 0x5412 && qrFormatBitsM(1) == 0x5125);
    // Reed-Solomon: the check bytes of the 16 data bytes of the standard's "HELLO WORLD" example (version 1-M, 10 check bytes)
    const unsigned char d[16] = {0x20, 0x5B, 0x0B, 0x78, 0xD1, 0x72, 0xDC, 0x4D, 0x43, 0x40, 0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11};
    const auto rs = qrReedSolomon(std::vector<unsigned char>(d, d + 16), 10);
    const unsigned char want[10] = {0xC4, 0x23, 0x27, 0x77, 0xEB, 0xD7, 0xE7, 0xE2, 0x5D, 0x17};
    bool same = rs.size() == 10;
    for (int i = 0; i < 10 && same; i++) same = rs[(size_t)i] == want[i];
    CHK(same);
    QrCode q;
    CHK(qrEncode("https://quiet-river-lamp-7.trycloudflare.com", q));
    CHK(q.size == 29 || q.size == 25 || q.size == 33);   // version 3 (29), 4 (33) ...
    // the three finder patterns and the timing lines
    for (int fx : {0, q.size - 7}) for (int fy : {0, q.size - 7}) {
        if (fx != 0 && fy != 0) continue;
        CHK(q.dark(fx, fy) && q.dark(fx + 6, fy + 6) && !q.dark(fx + 1, fy + 1) && q.dark(fx + 3, fy + 3));
    }
    bool timing = true;
    for (int i = 8; i < q.size - 8; i++) if (q.dark(i, 6) != (i % 2 == 0) || q.dark(6, i) != (i % 2 == 0)) timing = false;
    CHK(timing);
    CHK(q.dark(8, q.size - 8));                       // the always-dark module
    QrCode big;
    CHK(qrEncode(std::string(213, 'x'), big) && big.size == 57);
    CHK(!qrEncode(std::string(214, 'x'), big));
    QrCode tiny;
    CHK(qrEncode("a", tiny) && tiny.size == 21);
}

int main() {
    testPrefsRoundTrip();
    testPlan();
    testRigs();
    testDecoders();
    testServerConf();
    testDirectoryConf();
    testStep2();
    testTestLines();
    testQr();
    return testSummary("webshare_core");
}
