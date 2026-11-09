// Channelised analog voice services: the channel plans (CB, FRS / GMRS, PMR446, amateur FM repeaters, HFGCS and HF aviation voice, UHF
// wireless microphone bands). Pure data and lookups, no state. Receive aid only: national rules differ and decide what may be used.
//
// Sources (public band plans, checked against the pages named where one was found):
//   CB: FCC 47 CFR 95 subpart D (40 channels 26.965 - 27.405 MHz, channel 9 emergency / roadside assistance, 4 W AM, 12 W PEP SSB); CEPT
//       (ERC/REC 70-03, the same 40 channels with channels 23, 24 and 25 in number order 27.235 / 27.245 / 27.255); Germany, Czechia and Hungary
//       add 40 channels at 26.565 - 26.955 MHz in 10 kHz steps; the UK adds the 27/81 plan, 40 FM channels 27.60125 - 27.99125 MHz.
//   FRS / GMRS: FCC 47 CFR 95 subparts B and E (FRS 1 - 7 at 462.5625 + 25 kHz n, 8 - 14 at 467.5625 + 25 kHz n, 15 - 22 at 462.550 + 25 kHz n;
//       GMRS repeater inputs 467.550 - 467.725 MHz in 25 kHz steps).
//   PMR446: ECC Decision (15)05: 16 analogue 12.5 kHz channels 446.00625 + 12.5 kHz n, from 2016 (before that channels 1 - 8 only).
//   Amateur: IARU Region 1 / 2 / 3 band plans as implemented in dect2::ham (ham_voice.h); the repeater grids below are the common outputs of
//       each region (see docs/CHANNELS-3.8.md for what was and was not checked); tones are measured from the signal, not listed.
//   HF: HFGCS primary voice frequencies (4724.0, 8992.0, 11175.0 and 15016.0 kHz USB) and the MARS / Canadian Forces relays named on the same
//       page; VOLMET frequencies and minute schedules from the public station list of dxinfocentre.com (2025-07).
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace chv {

// The services (the families 340 .. 346 are kFamilyBase + Service)
enum class Service { Cb = 0, Frs, Pmr, Ham, HfVoice, Mdc, Mic, kCount };
constexpr int kFamilyBase = 340;
constexpr int kServiceCount = (int)Service::kCount;
inline int familyOf(Service s) { return kFamilyBase + (int)s; }
inline bool serviceOfFamily(int family, Service& s) { if (family < kFamilyBase || family >= kFamilyBase + kServiceCount) return false; s = (Service)(family - kFamilyBase); return true; }
const char* serviceId(Service s);      // "cb", "frs", "pmr446", "hamfm", "hfvoice", "mdc1200", "wmic"
const char* serviceName(Service s);

// How a channel is demodulated
enum class Demod { Nfm, Am, Usb, Lsb, Wfm, Auto };   // Auto: CB (AM, FM or SSB, decided from the signal)
const char* demodName(Demod d);

enum ChannelFlag : unsigned {
    kEmergency = 1u << 0,      // CB 9, HF distress / emergency use
    kCalling = 1u << 1,        // calling channel
    kRepeater = 1u << 2,       // listen on mhz (the repeater output), the input is txMhz
    kLicence = 1u << 3,        // needs a licence to transmit (GMRS repeater, amateur)
    kLowPower = 1u << 4,       // 0.5 W class
    kSsb = 1u << 5,            // SSB use is common here
    kScheduled = 1u << 6,      // a broadcast with a minute schedule (VOLMET)
    kContinuous = 1u << 7,     // broadcasts all the time
    kDigitalAlso = 1u << 8,    // the frequency is also used by a digital mode
};

struct Channel {
    int number = 0;            // the channel number of the plan (0: none, a frequency list)
    std::string label;         // "19", "15R", "RV48", "HFGCS 11175"
    double mhz = 0;            // where to listen
    double txMhz = 0;          // repeater input (0: simplex)
    Demod demod = Demod::Nfm;
    double bwHz = 12500;       // the channel width of the plan
    unsigned flags = 0;
    std::string name;          // short, for the picker
    std::string note;          // one sentence
    // HF voice: the minutes past the hour the broadcast is on the air (empty: always or not scheduled); e.g. {20, 50} for 20 and 50
    std::vector<int> minutes;
    bool has(unsigned f) const { return (flags & f) != 0; }
    bool duplex() const { return txMhz != 0 && txMhz != mhz; }
};

// ---- regions of each service: an index into regionNames(s); 0 is the default when nothing is known
struct RegionInfo { const char* id; const char* name; };
const std::vector<RegionInfo>& regions(Service s);
int regionIndex(Service s, const std::string& id);   // -1 when unknown

// The region of a country code ("DE", "GB", "US" ...) and of a position; -1: not known
int regionForCountry(Service s, const std::string& iso2);
int regionForPosition(Service s, double latDeg, double lonDeg);

// The channels of a service in a region. Mdc has none (every carrier on a 12.5 kHz raster is a channel), Mic lists the bands instead.
const std::vector<Channel>& channels(Service s, int region);

// The channel that holds mhz within tolMhz; nullptr when none. With a repeater the output is matched (and the input when matchInput).
const Channel* findFrequency(Service s, int region, double mhz, double tolMhz, bool matchInput = false);
const Channel* findLabel(Service s, int region, const std::string& text);   // "19", "ch 19", "15r", "rv48", case blind

// The raster the carrier finder uses for a service and region (every channel of the plan is a multiple of it, to within its tolerance)
double rasterHz(Service s, int region);
// Default frequency (the middle of the plan: a wide capture holds every channel) and the span the plan covers
struct Span { double loMhz = 0, hiMhz = 0, centreMhz = 0; };
Span planSpan(Service s, int region);

// ---- wireless microphones (UHF): the bands a finder searches
struct MicBand { double loMhz, hiMhz; const char* name; const char* note; };
const std::vector<MicBand>& micBands(int region);       // 0 Europe, 1 USA / Canada

// ---- HF voice: is a scheduled broadcast on the air at this minute of the hour? (continuous ones always)
bool onAirAtMinute(const Channel& c, int minuteOfHour);
// The next minute of the hour (0 .. 59, wrapping) a scheduled broadcast starts at; -1 for a continuous or unscheduled channel
int nextStartMinute(const Channel& c, int minuteOfHour);

// ---- sub-audible tones
// CTCSS tones (Hz) a service commonly uses: PMR446 and FRS radios offer the 38 standard tones, DCS the 83 standard codes
// (the lists themselves are in tones/tones_proto.h); what a repeater listing wants: the access tone and its index for a display
std::string toneLabel(double ctcssHz, int dcsCode, bool dcsInverted);   // "88.5 Hz", "D023N", "" none
// The PMR446 / FRS "code" number (1 .. 38) of a CTCSS tone, and the 39 .. 121 codes of the DCS list: the way a handset menu numbers them; 0 none
int handsetToneNumber(double ctcssHz);

} // namespace chv
} // namespace dect2
