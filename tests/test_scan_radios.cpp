// The channel scanner accepts any radio the receiver can open (not only a HackRF), and refuses what cannot work with a clear reason.
#include "dect2/scanner.h"
#include <cstdio>

using namespace dect2;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    ScanConfig dvb; dvb.bwMhz = 8;
    ScanConfig atsc; atsc.atsc = true; atsc.bwMhz = 6;
    std::string err;
    DeviceInfo hack; hack.kind = DeviceInfo::HackRF; hack.name = "HackRF One";
    CHECK(Scanner::check(hack, dvb, err), "HackRF can scan");
    DeviceInfo rsp; rsp.kind = DeviceInfo::Soapy; rsp.name = "SDRplay RSP1B"; rsp.maxRateHz = 10.66e6;
    CHECK(Scanner::check(rsp, dvb, err), "a SoapySDR radio that reaches 10 Msps can scan DVB");
    CHECK(Scanner::check(rsp, atsc, err), "and ATSC");
    DeviceInfo nat; nat.kind = DeviceInfo::Native; nat.name = "Airspy R2"; nat.maxRateHz = 10e6;
    CHECK(Scanner::check(nat, dvb, err), "a native driver radio can scan");
    DeviceInfo rtl; rtl.kind = DeviceInfo::Soapy; rtl.name = "RTL-SDR"; rtl.maxRateHz = 3.2e6;
    err.clear();
    CHECK(!Scanner::check(rtl, dvb, err) && err.find("3.2 Msps") != std::string::npos, "an RTL-SDR is too slow for an 8 MHz channel, with the reason");
    CHECK(!Scanner::check(rtl, atsc, err), "and for a 6 MHz channel");
    DeviceInfo unknownRate; unknownRate.kind = DeviceInfo::Soapy; unknownRate.name = "radio without a rate range";
    CHECK(Scanner::check(unknownRate, dvb, err), "a radio that reports no maximum is tried");
    DeviceInfo file; file.kind = DeviceInfo::File; file.name = "recording";
    err.clear();
    CHECK(!Scanner::check(file, dvb, err) && !err.empty(), "a recording cannot be scanned");
    printf(fails ? "scan radios: FAILED\n" : "scan radios: ok\n");
    return fails ? 1 : 0;
}
