// HD Radio test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeHdrSynth(): a hybrid station with its digital sidebands, carrying station information (SIS: call sign WONR, name "OnAir HD", long
// name, slogan, location, station message), two audio programs HD1 and HD2, program service data (title, artist, album, genre) for both,
// the station information guide, and one large object: a small PNG logo that HD1's program data names as its album art. The audio packets
// are filler bytes (OnAir has no HDC encoder; HDC sound is patented and not decoded either).
//
// SynthConfig:
//   modeOpt[0]  waveform: 0 FM hybrid MP1 (analog FM with a 1 kHz tone, primary main sidebands; P1 and PIDS),
//               1 AM hybrid MA1 (analog AM with a 1 kHz tone; primary, secondary, tertiary sidebands; P1, P3 and PIDS),
//               2 FM extended hybrid MP3 (MP1 plus two extended partitions per sideband carrying P3, with a third program HD3)
//   snrDb       FM: the SNR of each digital subcarrier (the digital sidebands' power against the noise in their bandwidth);
//               AM: the same for the primary subcarriers (the secondary, tertiary and PIDS subcarriers are 7 to 14 dB weaker, as on the air)
//   cfoHz       carrier offset
#pragma once
#include "mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

std::unique_ptr<ModeSynth> makeHdrSynth(const SynthConfig& cfg, double sampleRate);

// What the test signal carries, for the tests
struct HdrTestContent {
    std::string callSign = "WONR";
    std::string name = "OnAir HD";
    std::string longName = "ONAIR HD RADIO TEST STATION";
    std::string slogan = "Station data you can see, sound you cannot hear";
    std::string message = "This is a test signal made by OnAir. HD Radio sound uses the HDC codec, which OnAir does not decode.";
    std::string country = "US";
    int facilityId = 54321;
    double lat = 40.75, lon = -73.99;
    int altM = 96;
    struct Prog { int number; int type; std::string sigName, title, artist, album, genre; };
    std::vector<Prog> programs = {
        {0, 5, "OnAir HD", "Test Pattern", "The OnAir Band", "Signals Vol. 1", "Rock"},
        {1, 14, "OnAir HD2", "Second Program", "OnAir Quartet", "Data Only", "Jazz"},
    };
    Prog extended = {2, 9, "OnAir HD3", "Third Program", "OnAir Trio", "Extended Partitions", "Pop"};   // MP3 only, on P3
    int artPort = 0x1000;          // HD1 album art (LOT, primary image)
    int artLot = 1;
    std::string artName = "onair_logo.png";
    int trafficPort = 0x1100;      // a station data service listed in the SIG (traffic, nothing sent on it)
    std::string trafficName = "OnAir Traffic";
};
const HdrTestContent& hdrTestContent();
const std::vector<uint8_t>& hdrTestLogoPng();    // the LOT image (a 64 x 64 palette PNG)

} // namespace dect2
