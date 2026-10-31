// The table of modes (sig_known.h). The widths are the 99 % occupied width as this identifier measures it on the mode's signal; they
// were checked against the spec values (docs/IDENTIFIER.md) and against the app's own generators. Fields: name, mode id, sub-mode id,
// family, accepted modulation classes, the usual class, symbol rates (OFDM: useful symbol lengths), widths, FSK deviation, 0 either /
// 1 continuous / 2 bursty, burst length, burst period, frame sync to look for, bands (MHz), note; optional: MFSK tones and their spacing.
#include "sig_known.h"
#include "dect2/engine/mode_tuning.h"
#include <cstring>

namespace dect2 {
namespace sigid {

const Known kKnown[] = {
// ---- broadcast
{"DVB-T / T2", "dvbt", "", "Broadcast", MB(ModOfdm), ModOfdm, {112e-6, 224e-6, 448e-6, 896e-6, 1792e-6, 3584e-6, 128e-6, 256e-6, 512e-6, 1024e-6, 2048e-6, 4096e-6, 149.3e-6, 298.7e-6, 597.3e-6, 1194.7e-6, 2389e-6, 4779e-6}, {8.27e6, 7.24e6, 6.2e6}, 0, 1, 0, 0, SyncNone, {174, 230, 470, 862}, "OFDM, 6 / 7 / 8 MHz channel, 1k to 32k FFT"},
{"ATSC 1.0", "atsc", "", "Broadcast", MB(ModQam)|MB(ModAm)|MB(ModPsk8), ModQam, {}, {5.55e6}, 0, 1, 0, 0, SyncNone, {54, 88, 174, 216, 470, 698}, "8-VSB, 6 MHz channel, pilot carrier near the lower edge"},
{"ATSC 3.0", "atsc3", "", "Broadcast", MB(ModOfdm), ModOfdm, {1333.3e-6, 2666.7e-6, 5333.3e-6}, {5.15e6, 5.55e6}, 0, 1, 0, 0, SyncNone, {174, 216, 470, 698}, "OFDM, 6 MHz channel, bootstrap symbols"},
{"ISDB-T", "isdbt", "", "Broadcast", MB(ModOfdm), ModOfdm, {252e-6, 504e-6, 1008e-6}, {5.55e6, 6.5e6}, 0, 1, 0, 0, SyncNone, {470, 806}, "OFDM, 13 segments, 6 / 7 / 8 MHz channel"},
{"DTMB", "dtmb", "", "Broadcast", MB(ModOfdm)|MB(ModQam), ModOfdm, {500e-6, 555e-6, 625e-6}, {7.56e6}, 0, 1, 0, 0, SyncNone, {470, 862}, "OFDM 3780 carriers (or single carrier), 8 MHz"},
{"DVB-C", "dvbc", "", "Broadcast", MB(ModQam)|MB(ModOfdm), ModQam, {6.875e6, 6.9e6, 5.217e6}, {7.3e6, 8e6}, 0, 1, 0, 0, SyncNone, {110, 862}, "16 to 256-QAM, 6.9 MBd, 8 MHz cable channel"},
{"DVB-S / S2", "dvbs", "", "Broadcast", MB(ModQpsk)|MB(ModPsk8), ModQpsk, {}, {5.85e6}, 0, 1, 0, 0, SyncNone, {950, 2150, 10700, 12750}, "QPSK / 8PSK, 1 to 45 MBd, roll-off 0.20 to 0.35"},
{"Analog TV", "atv", "", "Broadcast", MB(ModAm)|MB(ModCarrier)|MB(ModPulse)|MB(ModFm), ModAm, {15625}, {6e6}, 0, 0, 0, 0, SyncNone, {47, 68, 174, 230, 470, 862}, "vestigial sideband AM picture, 15.625 kHz lines, FM sound"},
{"MAC television", "mac", "", "Broadcast", MB(ModFm), ModFm, {}, {13e6}, 0, 1, 0, 0, SyncNone, {10700, 12750}, "wide FM satellite TV, packet sound"},
{"DAB / DAB+", "dab", "", "Broadcast", MB(ModOfdm), ModOfdm, {1e-3, 250e-6, 125e-6, 500e-6}, {1.53e6}, 0, 1, 0, 0, SyncNone, {174, 240, 1452, 1492}, "OFDM, 1.536 MHz, Tu 1 ms (mode I)"},
{"FM broadcast", "fm", "", "Broadcast", MB(ModFm), ModFm, {}, {140e3}, 0, 1, 0, 0, SyncNone, {65.8f, 74, 76, 108}, "wide FM, 200 kHz, stereo pilot at 19 kHz"},
{"DRM", "drm", "", "Broadcast", MB(ModOfdm), ModOfdm, {21.33e-3, 24e-3, 14.67e-3, 9.33e-3}, {9.8e3, 4.5e3, 18e3}, 0, 1, 0, 0, SyncNone, {0.1f, 30}, "OFDM, 4.5 to 20 kHz, Tu 9 to 24 ms"},
{"DRM+", "drm", "", "Broadcast", MB(ModOfdm), ModOfdm, {2.25e-3}, {96e3}, 0, 1, 0, 0, SyncNone, {47, 108}, "OFDM, 96 kHz, Tu 2.25 ms"},
{"HD Radio", "hdr", "", "Broadcast", MB(ModGmsk)|MB(ModOfdm)|MB(ModFm)|MB(ModCarrier), ModOfdm, {}, {105e3}, 0, 1, 0, 0, SyncNone, {88, 108, 0.5f, 1.7f}, "OFDM sidebands beside an FM or AM carrier"},
{"CDR", "cdr", "", "Broadcast", MB(ModOfdm), ModOfdm, {}, {96.8e3}, 0, 1, 0, 0, SyncNone, {87, 108}, "OFDM, 100 kHz, in the FM band"},
{"AM broadcast", "general", "am", "Broadcast", MB(ModAm)|MB(ModGmsk)|MB(ModCarrier), ModAm, {}, {9e3}, 0, 1, 0, 0, SyncNone, {0.15f, 30}, "AM, 9 / 10 kHz"},
// ---- digital voice
{"DMR", "dmr", "", "Digital voice", MB(ModFsk4), ModFsk4, {4800}, {7.7e3}, 1944, 0, 0, 0, SyncDmr, {136, 174, 403, 470, 806, 870}, "4FSK 4800 Bd, +-648 / +-1944 Hz, 12.5 kHz"},
{"P25 Phase 1", "p25", "", "Digital voice", MB(ModFsk4), ModFsk4, {4800}, {6.6e3}, 1800, 0, 0, 0, SyncP25, {136, 174, 380, 512, 764, 870}, "C4FM 4800 Bd, +-600 / +-1800 Hz, 12.5 kHz"},
{"NXDN96", "nxdn", "", "Digital voice", MB(ModFsk4), ModFsk4, {4800}, {8.5e3}, 2400, 0, 0, 0, SyncNxdn, {136, 174, 400, 520}, "4FSK 4800 Bd, +-800 / +-2400 Hz, 12.5 kHz"},
{"NXDN48", "nxdn", "", "Digital voice", MB(ModFsk4), ModFsk4, {2400}, {4.5e3}, 1050, 0, 0, 0, SyncNxdn, {136, 174, 400, 520}, "4FSK 2400 Bd, +-350 / +-1050 Hz, 6.25 kHz"},
{"dPMR", "dpmr", "", "Digital voice", MB(ModFsk4), ModFsk4, {2400}, {4.1e3}, 1050, 0, 0, 0, SyncDpmr, {136, 174, 400, 470}, "4FSK 2400 Bd, +-350 / +-1050 Hz, 6.25 kHz"},
{"M17", "m17", "", "Digital voice", MB(ModFsk4), ModFsk4, {4800}, {9.1e3}, 2400, 0, 0, 0, SyncNone, {144, 148, 420, 450}, "4FSK 4800 Bd, +-800 / +-2400 Hz"},
{"D-STAR", "dstar", "", "Digital voice", MB(ModGmsk)|MB(ModFsk2), ModGmsk, {4800}, {4.9e3}, 1200, 0, 0, 0, SyncNone, {144, 148, 420, 450, 1240, 1300}, "GMSK 4800 Bd, 6.25 kHz"},
{"TETRA", "tetra", "", "Digital voice", MB(ModPi4Dqpsk)|MB(ModQpsk)|MB(ModQam), ModPi4Dqpsk, {18000}, {20.9e3}, 0, 1, 0, 0, SyncNone, {380, 400, 410, 430, 450, 470, 870, 876, 915, 921}, "pi/4-DQPSK 18 kBd, 25 kHz, continuous downlink"},
{"TETRAPOL", "tetrapol", "", "Digital voice", MB(ModGmsk)|MB(ModFsk2)|MB(ModFsk4), ModGmsk, {8000}, {7.5e3}, 0, 1, 0, 0, SyncNone, {380, 400, 410, 430, 450, 470}, "GMSK 8 kBd, 10 / 12.5 kHz, TDMA frames of 40 ms"},
{"DECT", "dect", "", "Digital voice", MB(ModGmsk)|MB(ModFsk2)|MB(ModOfdm)|MB(ModPulse)|MB(ModFm), ModGmsk, {1.152e6}, {1.4e6}, 0, 2, 0.417, 0, SyncNone, {1880, 1900, 1920, 1930}, "GFSK 1.152 MBd, 10 ms TDMA frame, 24 slots of 0.417 ms"},
{"GSM-R / GSM", "gsmr", "", "Cellular", MB(ModGmsk)|MB(ModFsk2)|MB(ModChirp), ModGmsk, {270833}, {245e3}, 67708, 0, 0, 0, SyncNone, {876, 880, 921, 960, 1805, 1880}, "GMSK 270.833 kBd, 200 kHz, TDMA slots of 0.577 ms"},
// ---- aviation
{"ADS-B / Mode S", "adsb", "", "Aviation", MB(ModPulse)|MB(ModAm)|MB(ModOfdm), ModPulse, {1e6, 2e6}, {2.75e6, 2.6e6}, 0, 2, 0.12, 0, SyncNone, {1089, 1091}, "PPM pulses 1 Mbit/s, 120 us messages on 1090 MHz"},
{"Mode S uplink", "uplink", "", "Aviation", MB(ModPulse)|MB(ModBpsk)|MB(ModQam), ModPulse, {1e6}, {96e3, 6e6}, 0, 2, 0, 0, SyncNone, {1029, 1031}, "1030 MHz interrogations"},
{"UAT", "uat", "", "Aviation", MB(ModGmsk)|MB(ModFsk2)|MB(ModPulse), ModFsk2, {1.04e6}, {1.2e6}, 0, 2, 0, 0, SyncNone, {977, 979}, "CPFSK 1.041 MBd bursts on 978 MHz"},
{"FLARM", "flarm", "", "Aviation", MB(ModGmsk)|MB(ModFsk2)|MB(ModOfdm)|MB(ModPulse)|MB(ModFm), ModFsk2, {100e3}, {360e3}, 0, 2, 0, 0, SyncNone, {868, 869}, "GFSK 100 kBd bursts on 868 MHz"},
{"ACARS", "acars", "", "Aviation", MB(ModAm)|MB(ModPulse)|MB(ModGmsk)|MB(ModFsk2), ModAm, {2400, 4800}, {5.3e3}, 0, 2, 0, 0, SyncNone, {129, 137}, "AM-MSK 2400 bit/s, bursts"},
{"VDL Mode 2", "vdl2", "", "Aviation", MB(ModPsk8)|MB(ModQpsk)|MB(ModQam), ModPsk8, {10500}, {12.1e3}, 0, 2, 0, 0, SyncNone, {136.6f, 137}, "D8PSK 10.5 kBd, 25 kHz, bursts"},
{"VDL Mode 4", "vdl4", "", "Aviation", MB(ModGmsk)|MB(ModFsk2)|MB(ModOfdm)|MB(ModPulse), ModGmsk, {19200}, {18.8e3}, 0, 2, 0, 0, SyncNone, {118, 137}, "GFSK 19.2 kBd, STDMA slots"},
{"HFDL", "hfdl", "", "Aviation", MB(ModBpsk)|MB(ModQpsk)|MB(ModPsk8), ModBpsk, {1800}, {2.13e3}, 0, 0, 0, 0, SyncNone, {2.8f, 22}, "PSK 1800 Bd, 2.4 kHz SSB channel"},
{"SSB voice", "general", "", "Analog voice", MB(ModPulse)|MB(ModAm)|MB(ModQam)|MB(ModFm)|MB(ModCarrier)|MB(ModUnknown)|MB(ModBpsk)|MB(ModFsk2)|MB(ModGmsk), ModAm, {}, {2.4e3}, 0, 0, 0, 0, SyncSsb, {1.8f, 30, 50, 54, 144, 148, 420, 450}, "single sideband speech: 2 to 3 kHz of one sideband, no carrier, syllabic envelope; the General receiver in USB / LSB"},
{"NFM voice", "general", "", "Analog voice", MB(ModFm), ModFm, {}, {9e3, 5e3}, 0, 0, 0, 0, SyncNone, {30, 50, 136, 174, 400, 470}, "narrow FM speech, 12.5 / 25 kHz; the General receiver in NFM"},
{"Airband voice", "airband", "", "Aviation", MB(ModAm)|MB(ModCarrier)|MB(ModPulse), ModAm, {}, {6.3e3}, 0, 0, 0, 0, SyncNone, {108, 137}, "AM voice, 8.33 / 25 kHz"},
{"VOR", "vorils", "vor", "Aviation", MB(ModCarrier)|MB(ModAm), ModCarrier, {}, {250}, 0, 1, 0, 0, SyncNone, {108, 118}, "AM carrier with 30 Hz and 9960 Hz FM subcarrier"},
{"ILS localizer", "vorils", "ils", "Aviation", MB(ModCarrier)|MB(ModAm), ModCarrier, {}, {320}, 0, 1, 0, 0, SyncNone, {108.1f, 112, 329, 335}, "AM carrier with 90 and 150 Hz tones"},
{"NDB", "ndb", "", "Aviation", MB(ModCarrier)|MB(ModAm), ModAm, {}, {840}, 0, 1, 0, 0, SyncNone, {0.19f, 0.535f}, "keyed carrier or 400 / 1020 Hz tone"},
{"Drone Remote ID", "rid", "", "Aviation", MB(ModOfdm)|MB(ModPi4Dqpsk)|MB(ModFsk2)|MB(ModGmsk)|MB(ModQam), ModGmsk, {1e6}, {13e6, 2e6}, 0, 2, 0, 0, SyncNone, {2400, 2484, 5725, 5875}, "Bluetooth LE advertising and Wi-Fi beacons"},
{"DJI DroneID", "dji", "", "Aviation", MB(ModOfdm), ModOfdm, {66.67e-6}, {9e6}, 0, 2, 1, 0, SyncNone, {2400, 2484, 5725, 5875}, "OFDM bursts, 10 MHz, LTE-like"},
// ---- marine
{"AIS", "ais", "", "Marine", MB(ModGmsk)|MB(ModFsk2)|MB(ModPulse), ModGmsk, {9600}, {9.9e3}, 2400, 2, 0, 0, SyncAis, {156, 163}, "GMSK 9600 Bd, 25 kHz, bursts"},
{"VDES", "vdes", "", "Marine", MB(ModQpsk)|MB(ModPi4Dqpsk)|MB(ModQam)|MB(ModGmsk)|MB(ModPulse), ModQam, {}, {93.6e3, 50e3, 100e3}, 0, 2, 0, 0, SyncNone, {157, 163}, "VDE-TER pi/4-QPSK 25 to 100 kHz, ASM GMSK 9600"},
{"NAVTEX", "marine", "navtex", "Marine", MB(ModFsk2)|MB(ModGmsk), ModFsk2, {100}, {360}, 85, 1, 0, 0, SyncNone, {0.4f, 0.6f}, "FSK 100 Bd, 170 Hz shift (SITOR-B)", 2, 170},
{"DSC", "marine", "dsc", "Marine", MB(ModFsk2)|MB(ModGmsk), ModFsk2, {100}, {411}, 85, 0, 0, 0, SyncNone, {2.1f, 2.2f, 156.5f, 156.6f}, "FSK 100 Bd, 170 Hz shift", 2, 170},
{"Weather fax", "marine", "wefax", "Marine", MB(ModCarrier)|MB(ModFm), ModCarrier, {}, {172}, 0, 1, 0, 0, SyncNone, {3, 30}, "FM of a 1500 - 2300 Hz audio subcarrier, 120 lpm"},
// ---- satellite
{"Inmarsat-C", "inmc", "", "Satellite", MB(ModBpsk)|MB(ModQpsk), ModBpsk, {1200}, {1.98e3}, 0, 1, 0, 0, SyncNone, {1525, 1560}, "BPSK 1200 Bd"},
{"Inmarsat Aero", "aero", "", "Satellite", MB(ModBpsk)|MB(ModQpsk)|MB(ModGmsk), ModBpsk, {600, 1200, 5250, 10500}, {8.7e3}, 0, 0, 0, 0, SyncNone, {1525, 1560}, "A/O-QPSK 600 to 10500 Bd"},
{"Iridium", "iridium", "", "Satellite", MB(ModQpsk)|MB(ModQam)|MB(ModBpsk)|MB(ModPi4Dqpsk)|MB(ModFm), ModQpsk, {25000, 12500}, {29.6e3}, 0, 2, 20.9, 90, SyncNone, {1616, 1626.5f}, "DE-QPSK 25 kBd bursts, 90 ms frame"},
{"Globalstar", "globalstar", "", "Satellite", MB(ModPulse)|MB(ModQam)|MB(ModQpsk)|MB(ModBpsk), ModQam, {}, {1.44e6}, 0, 1, 0, 0, SyncNone, {1610, 1627, 2483, 2500}, "CDMA, 1.23 MHz"},
{"Orbcomm", "orbcomm", "", "Satellite", MB(ModFsk2)|MB(ModGmsk), ModFsk2, {4800}, {5.7e3}, 1900, 1, 0, 0, SyncNone, {137, 138}, "SDPSK 4800 Bd downlink on 137 MHz"},
{"Meteor-M LRPT", "meteor", "", "Satellite", MB(ModQpsk)|MB(ModGmsk)|MB(ModFsk2)|MB(ModPsk8), ModQpsk, {72000, 80000, 144000}, {96.6e3}, 0, 1, 0, 0, SyncNone, {137, 138}, "QPSK / OQPSK 72 or 80 kBd on 137.1 / 137.9 MHz"},
{"GOES HRIT/LRIT", "goes", "", "Satellite", MB(ModBpsk)|MB(ModQpsk), ModBpsk, {927000, 293883}, {1.18e6}, 0, 1, 0, 0, SyncNone, {1691, 1695}, "BPSK 927 kBd (HRIT) or 293.9 kBd (LRIT) near 1694 MHz"},
{"HRPT", "hrpt", "", "Satellite", MB(ModQpsk)|MB(ModBpsk), ModQpsk, {2.33e6, 665.4e3}, {3.13e6}, 0, 1, 0, 0, SyncNone, {1698, 1710}, "BPSK 665 kBd (NOAA) or QPSK 2.33 MBd (MetOp AHRPT) near 1700 MHz"},
{"Elektro-L LRIT", "elektro", "", "Satellite", MB(ModBpsk)|MB(ModQpsk), ModBpsk, {665.4e3}, {845e3}, 0, 1, 0, 0, SyncNone, {1690, 1695}, "BPSK 665.4 kBd near 1691 MHz"},
{"CubeSat telemetry", "cubesat", "", "Satellite", MB(ModFsk2)|MB(ModFsk4)|MB(ModGmsk), ModFsk2, {1200, 4800, 9600}, {9.5e3}, 0, 0, 0, 0, SyncNone, {144, 146, 435, 438}, "GFSK / AFSK 1200 to 9600 Bd amateur satellites"},
{"QO-100", "qo100", "", "Satellite", MB(ModQam)|MB(ModQpsk)|MB(ModOfdm)|MB(ModAm)|MB(ModBpsk)|MB(ModCarrier)|MB(ModFm), ModQam, {}, {1.65e6}, 0, 1, 0, 0, SyncNone, {10489, 10499, 2400, 2401}, "narrowband transponder, SSB / digital 2.7 kHz to 2 MHz"},
// ---- data, paging, IoT
{"POCSAG", "pager", "pocsag", "Data and IoT", MB(ModFsk2)|MB(ModGmsk)|MB(ModChirp), ModFsk2, {512, 1200, 2400}, {10.1e3}, 4500, 0, 0, 0, SyncPocsag, {25, 1000}, "2FSK 512 / 1200 / 2400 Bd, +-4.5 kHz"},
{"FLEX", "pager", "flex", "Data and IoT", MB(ModFsk2)|MB(ModFsk4), ModFsk2, {1600, 3200, 6400}, {11e3}, 4800, 0, 0, 0, SyncNone, {929, 932, 138, 174}, "2/4FSK 1600 / 3200 Bd, +-4.8 kHz"},
{"APRS / packet", "packet", "", "Data and IoT", MB(ModFm)|MB(ModFsk2)|MB(ModFsk4)|MB(ModAm), ModFm, {}, {9e3}, 0, 2, 0, 0, SyncNone, {144.39f, 144.8f, 136, 174, 26, 30}, "narrow FM, AFSK 1200 Bd (APRS, packet), G3RUH 9600"},
{"ISM sensors", "ism", "", "Data and IoT", MB(ModPulse)|MB(ModFsk2)|MB(ModAm), ModPulse, {}, {17e3, 90e3}, 0, 2, 0, 0, SyncNone, {433.05f, 434.79f, 868, 868.6f, 902, 928}, "OOK / FSK bursts of weather sensors, remotes, meters"},
{"Same alerts", "same", "", "Data and IoT", MB(ModFsk2)|MB(ModGmsk), ModFsk2, {520.83, 3125}, {8.6e3}, 0, 2, 0, 0, SyncNone, {162.4f, 162.55f}, "NOAA weather radio, SAME AFSK 520.83 Bd"},
{"LBJ railway", "lbj", "", "Data and IoT", MB(ModFm)|MB(ModFsk2), ModFm, {1200}, {25e3}, 1200, 0, 0, 0, SyncNone, {821}, "FM, 1200 bit/s FSK (Chinese railway)"},
{"Multitone", "multitone", "", "Data and IoT", MB(ModCarrier)|MB(ModChirp)|MB(ModAm)|MB(ModFm), ModCarrier, {}, {5.6e3}, 0, 2, 0, 0, SyncNone, {}, "tone-sequence paging and selcall"},
{"ANT+", "ant", "", "Data and IoT", MB(ModGmsk)|MB(ModFsk2)|MB(ModPulse), ModGmsk, {1e6}, {1.2e6}, 0, 2, 0, 0, SyncNone, {2457, 2458}, "GFSK 1 Mbit/s, bursts, 2457 MHz"},
{"Bluetooth LE", "ble", "", "Data and IoT", MB(ModGmsk)|MB(ModFsk2)|MB(ModOfdm)|MB(ModPulse)|MB(ModFm), ModGmsk, {1e6, 2e6}, {1.04e6}, 250000, 2, 0, 0, SyncNone, {2400, 2484}, "GFSK 1 Mbit/s, bursts"},
{"Zigbee / 802.15.4", "zigbee", "", "Data and IoT", MB(ModFsk2)|MB(ModGmsk)|MB(ModQpsk)|MB(ModPulse), ModQpsk, {2e6, 1e6, 500e3}, {2.2e6}, 0, 2, 1.1, 0, SyncNone, {2405, 2480, 868, 915}, "O-QPSK 2 MChip/s, 5 MHz channel"},
{"Wi-Fi", "wifi", "", "Data and IoT", MB(ModOfdm)|MB(ModQam)|MB(ModFsk2)|MB(ModPulse), ModOfdm, {3.2e-6}, {13.8e6, 17e6}, 0, 2, 1, 0, SyncNone, {2400, 2500, 5150, 5900, 5925, 7125}, "OFDM bursts, 20 to 160 MHz"},
{"Wi-Fi HaLow", "halow", "", "Data and IoT", MB(ModOfdm)|MB(ModQam), ModOfdm, {32e-6}, {1.94e6, 1e6, 2e6}, 0, 2, 1.5, 0, SyncNone, {863, 928}, "OFDM 1 / 2 / 4 MHz, sub-GHz"},
{"Wi-SUN", "wisun", "", "Data and IoT", MB(ModFsk2)|MB(ModQam)|MB(ModGmsk)|MB(ModPulse), ModFsk2, {50e3, 100e3, 150e3, 300e3}, {155e3}, 0, 2, 0, 0, SyncNone, {863, 870, 902, 928}, "2FSK 50 to 300 kBd"},
{"LoRa", "loraraw", "", "Data and IoT", MB(ModChirp)|MB(ModOfdm)|MB(ModFm), ModChirp, {}, {125e3, 250e3, 500e3}, 0, 2, 0, 0, SyncNone, {433, 435, 863, 870, 902, 928}, "chirp spread spectrum"},
{"Meshtastic", "mesh", "meshtastic", "Data and IoT", MB(ModChirp), ModChirp, {}, {250e3}, 0, 2, 0, 0, SyncNone, {433, 435, 868, 870, 902, 928}, "LoRa, long-fast preset 250 kHz SF11"},
{"MeshCore", "mesh", "meshcore", "Data and IoT", MB(ModChirp), ModChirp, {}, {62.5e3}, 0, 2, 0, 0, SyncNone, {868, 870}, "LoRa 62.5 kHz SF8"},
{"LoRa APRS", "mesh", "loraaprs", "Data and IoT", MB(ModChirp), ModChirp, {}, {125e3}, 0, 2, 0, 0, SyncNone, {433.7f, 433.9f}, "LoRa 125 kHz SF12 on 433.775 MHz"},
{"MeshCom", "mesh", "meshcom", "Data and IoT", MB(ModFsk4)|MB(ModFsk2)|MB(ModGmsk), ModFsk4, {}, {248e3}, 0, 2, 0, 0, SyncNone, {433, 434}, "GFSK / LoRa hybrid on 433.175 MHz"},
{"LoRaWAN", "mesh", "lorawan", "Data and IoT", MB(ModChirp), ModChirp, {}, {125e3}, 0, 2, 0, 0, SyncNone, {863, 870, 902, 928}, "LoRa 125 kHz uplinks on the 868 MHz channels"},
{"LoRaWAN FSK", "lorawanwide", "", "Data and IoT", MB(ModFsk2)|MB(ModGmsk)|MB(ModPulse), ModFsk2, {50e3}, {110e3}, 25000, 2, 0, 0, SyncNone, {868.7f, 868.9f, 924.7f, 924.9f}, "GFSK 50 kbit/s, +-25 kHz, BT 1, bursts on 868.8 MHz (EU868 DR7)"},
// ---- cellular
{"LTE", "cell", "", "Cellular", MB(ModOfdm), ModOfdm, {66.667e-6}, {1.08e6, 2.7e6, 4.5e6, 8.85e6, 13.4e6, 17.9e6}, 0, 1, 0, 0, SyncNone, {700, 960, 1710, 2170, 2500, 2700}, "OFDM, 15 kHz carriers, 1.4 to 20 MHz"},
{"NB-IoT", "nbiot", "", "Cellular", MB(ModOfdm), ModOfdm, {66.667e-6}, {229e3, 180e3}, 0, 1, 0, 0, SyncNone, {700, 960, 1710, 1880}, "OFDM 180 kHz carrier, in-band or guard-band of LTE"},
{"5G NR", "nr", "", "Cellular", MB(ModOfdm), ModOfdm, {33.33e-6, 66.67e-6}, {4.3e6, 8.9e6, 17.8e6, 35.6e6}, 0, 1, 0, 0, SyncNone, {700, 960, 1710, 2700, 3300, 4200}, "OFDM, 30 / 15 kHz carriers"},
// ---- HF and utility
{"CW", "cw", "", "HF and utility", MB(ModCarrier)|MB(ModAm)|MB(ModPulse)|MB(ModPsk8)|MB(ModFm), ModCarrier, {}, {200}, 0, 2, 0, 0, SyncNone, {}, "keyed carrier"},
{"RTTY", "hfdig", "rtty", "HF and utility", MB(ModFsk2)|MB(ModGmsk), ModFsk2, {45.45, 50, 75, 100}, {350}, 85, 0, 0, 0, SyncNone, {1.8f, 30}, "FSK 45.45 Bd, 170 Hz shift", 2, 170},
{"PSK31", "psk", "", "HF and utility", MB(ModBpsk)|MB(ModPulse)|MB(ModCarrier), ModBpsk, {31.25}, {954, 62}, 0, 0, 0, 0, SyncNone, {1.8f, 30}, "BPSK 31.25 Bd, 62 Hz wide"},
{"SSTV", "hfdig", "sstv", "HF and utility", MB(ModFm)|MB(ModCarrier)|MB(ModGmsk), ModFm, {}, {1.3e3}, 0, 0, 0, 0, SyncNone, {3.7f, 30, 144.5f}, "audio FM 1200 to 2300 Hz on SSB"},
{"FreeDV", "hfdig", "freedv", "HF and utility", MB(ModQam)|MB(ModQpsk)|MB(ModOfdm)|MB(ModFm), ModQam, {}, {1.16e3}, 0, 1, 0, 0, SyncNone, {3.5f, 30}, "OFDM voice, 1.25 kHz"},
{"FT8", "hfdig", "ft8", "HF and utility", MB(ModQam)|MB(ModPulse)|MB(ModFm)|MB(ModAm), ModQam, {6.25}, {2.37e3, 50}, 0, 2, 0, 0, SyncNone, {1.8f, 30, 50, 52, 144, 146}, "8-GFSK, 6.25 Bd, 50 Hz wide, 15 s slots", 8, 6.25},
{"FT4", "hfdig", "ft4", "HF and utility", MB(ModAm)|MB(ModPulse)|MB(ModQam)|MB(ModFm), ModAm, {20.83}, {2.05e3, 90}, 0, 2, 0, 0, SyncNone, {1.8f, 30}, "4-GFSK, 20.83 Bd, 83 Hz wide, 7.5 s slots", 4, 20.833},
{"FT2", "hfdig", "ft2", "HF and utility", MB(ModQam)|MB(ModPulse)|MB(ModAm)|MB(ModFm), ModQam, {41.67}, {1.65e3}, 0, 2, 0, 0, SyncNone, {1.8f, 30}, "4-GFSK, 41.67 Bd, 3.8 s slots"},
{"WSPR", "hfdig", "wspr", "HF and utility", MB(ModPulse)|MB(ModFm)|MB(ModCarrier)|MB(ModAm), ModPulse, {1.4648}, {270}, 0, 2, 0, 0, SyncNone, {0.4742f, 0.4743f, 1.8366f, 1.8367f, 3.5686f, 3.5687f, 7.0386f, 7.0387f, 10.1387f, 14.0956f}, "4-FSK 1.4648 Bd, 6 Hz wide, 2 minute slots", 4, 1.4648},
{"JT65", "hfdig", "jt65", "HF and utility", MB(ModPulse)|MB(ModFm)|MB(ModAm)|MB(ModQam), ModPulse, {2.69}, {1.4e3}, 0, 2, 0, 0, SyncNone, {1.8f, 30}, "65-FSK 2.69 Bd, 177 Hz wide, 1 minute slots", 65, 2.69},
{"JT9", "hfdig", "jt9", "HF and utility", MB(ModPulse)|MB(ModFm)|MB(ModAm)|MB(ModQam), ModPulse, {1.736}, {1.4e3}, 0, 2, 0, 0, SyncNone, {1.8f, 30}, "9-FSK 1.736 Bd, 15.6 Hz wide, 1 minute slots", 9, 1.736},
{"Q65", "hfdig", "q65", "HF and utility", MB(ModPulse)|MB(ModFm)|MB(ModAm)|MB(ModQam), ModPulse, {}, {1.7e3}, 0, 2, 0, 0, SyncNone, {1.8f, 30, 144, 146}, "65-FSK, 15 / 30 / 60 s slots"},
{"FST4", "hfdig", "fst4", "HF and utility", MB(ModPulse)|MB(ModFm)|MB(ModAm)|MB(ModQam), ModPulse, {}, {1.98e3}, 0, 2, 0, 0, SyncNone, {0.136f, 30}, "4-GFSK, 15 to 1800 s slots"},
{"MSK144", "hfdig", "msk144", "HF and utility", MB(ModGmsk)|MB(ModFsk2), ModGmsk, {2000}, {2.35e3}, 0, 2, 0, 0, SyncNone, {50, 52, 144, 146}, "MSK 2000 Bd meteor scatter bursts"},
{"HamDRM", "hfdig", "hamdrm", "HF and utility", MB(ModOfdm)|MB(ModQam)|MB(ModPulse), ModOfdm, {}, {2.52e3}, 0, 0, 0, 0, SyncNone, {3.5f, 30}, "DRM in 2.5 kHz for digital SSTV"},
{"Olivia", "hfdig", "olivia", "HF and utility", MB(ModFm)|MB(ModQam)|MB(ModOfdm), ModFm, {}, {1.04e3}, 0, 0, 0, 0, SyncNone, {1.8f, 30}, "32-tone MFSK 31.25 Bd, 1 kHz", 32, 31.25},
{"Contestia", "hfdig", "contestia", "HF and utility", MB(ModFm)|MB(ModQam)|MB(ModOfdm), ModFm, {}, {990}, 0, 0, 0, 0, SyncNone, {1.8f, 30}, "MFSK, Olivia family", 32, 31.25},
{"MFSK", "hfdig", "mfsk", "HF and utility", MB(ModFm)|MB(ModGmsk)|MB(ModQam), ModGmsk, {15.625}, {322}, 0, 0, 0, 0, SyncNone, {1.8f, 30}, "16-tone MFSK 15.625 Bd", 16, 15.625},
{"THOR", "hfdig", "thor", "HF and utility", MB(ModFm)|MB(ModGmsk)|MB(ModQam), ModFm, {}, {364}, 0, 0, 0, 0, SyncNone, {1.8f, 30}, "incremental MFSK with FEC", 18, 15.625},
{"DominoEX", "hfdig", "dominoex", "HF and utility", MB(ModFm)|MB(ModGmsk)|MB(ModQam), ModFm, {}, {299}, 0, 0, 0, 0, SyncNone, {1.8f, 30}, "IFK+ MFSK 18 tones", 18, 15.625},
{"Hellschreiber", "hfdig", "hell", "HF and utility", MB(ModQam)|MB(ModPulse)|MB(ModAm)|MB(ModCarrier), ModPulse, {122.5}, {438}, 0, 2, 0, 0, SyncNone, {1.8f, 30}, "keyed carrier 122.5 Bd (Feld Hell)"},
{"JS8Call", "js8", "", "HF and utility", MB(ModFm)|MB(ModPulse)|MB(ModCarrier)|MB(ModAm), ModFm, {6.25}, {184, 50}, 0, 2, 0, 0, SyncNone, {1.8f, 30}, "8-GFSK, 15 s slots", 8, 6.25},
// ROS: proprietary, no public framing or FEC, so nothing decodes it; the row is proposed only when the tone-grid check (ros_detect.h) finds it.
// Opens in the General receiver (USB) to listen. Numbers: docs/ROS.md (sigidwiki ROS, radioscanner.ru signal 236).
{"ROS", "general", "", "HF and utility", MB(ModFm)|MB(ModQam)|MB(ModGmsk)|MB(ModFsk2)|MB(ModFsk4)|MB(ModPulse)|MB(ModAm)|MB(ModCarrier)|MB(ModOfdm), ModFm, {}, {2.25e3}, 0, 0, 0, 0, SyncRos, {1.8f, 30, 50, 54, 144, 146}, "single-tone MFSK on a ~15.6 Hz grid, about 2.25 kHz, 4 / 8 / 16 Bd; identification only (ROS is proprietary)"},
{"ALE", "ale", "", "HF and utility", MB(ModFsk4)|MB(ModFsk2)|MB(ModPsk8)|MB(ModFm), ModPsk8, {125}, {2e3}, 0, 2, 0, 0, SyncNone, {2, 30}, "8-FSK 125 Bd (MIL-STD-188-141)"},
{"HF beacons", "hfbeacon", "", "HF and utility", MB(ModCarrier)|MB(ModPulse)|MB(ModAm), ModCarrier, {}, {200}, 0, 2, 0, 0, SyncNone, {14.1f, 14.11f, 18.11f, 18.12f, 21.15f, 21.16f, 24.93f, 24.94f, 28.2f, 28.3f}, "NCDXF/IARU beacons, 100 W to 1 W in 10 s slots"},
{"CODAR", "codar", "", "HF and utility", MB(ModChirp)|MB(ModFsk4)|MB(ModFm)|MB(ModQam), ModChirp, {}, {49.8e3, 25e3}, 0, 1, 0, 0, SyncNone, {4.4f, 5.4f, 12, 14, 24, 27, 41, 43}, "FMCW sweep, ocean radar"},
{"Time signals", "time", "", "HF and utility", MB(ModCarrier)|MB(ModAm), ModCarrier, {}, {40}, 0, 0, 0, 0, SyncNone, {0.06f, 0.078f, 2.5f, 25}, "WWV, DCF77, MSF, JJY seconds markers"},
{"Ionosonde", "iono", "", "HF and utility", MB(ModChirp)|MB(ModCarrier), ModChirp, {}, {25e3, 15e3}, 0, 2, 0, 0, SyncNone, {1, 30}, "chirp sounders"},
{"Datawell buoy", "datawell", "", "Marine", MB(ModGmsk)|MB(ModFsk2)|MB(ModCarrier), ModFsk2, {246}, {229}, 65, 1, 0, 0, SyncNone, {27.9f, 28, 29.5f, 30, 433, 434}, "HF buoy link FSK ~250 Bd, 65 Hz shift"},
{"Horus Binary", "horus", "", "Instruments", MB(ModFsk4), ModFsk4, {100}, {1.0e3}, 405, 0, 0, 0, SyncNone, {431, 438}, "4FSK 100 Bd, 270 Hz tone spacing, amateur balloons on 70 cm"},
{"Radiosonde", "sonde", "", "Instruments",MB(ModFsk2)|MB(ModGmsk), ModFsk2, {4800, 2500, 1200}, {7.9e3}, 2100, 1, 0, 0, SyncNone, {400, 406}, "2FSK 4800 Bd (RS41) and others, 400-406 MHz"},
{"Weather radio / wideband", "wb", "", "Instruments", MB(ModFm)|MB(ModCarrier), ModFm, {}, {20e3}, 0, 1, 0, 0, SyncNone, {162.4f, 162.55f}, "wideband measurement"},
{"Radio astronomy", "astro", "", "Instruments", MB(ModNoise), ModNoise, {}, {}, 0, 1, 0, 0, SyncNone, {1420, 1421}, "hydrogen line, below the noise floor"},
{"GNSS", "gnss", "", "Instruments", MB(ModNoise), ModNoise, {1.023e6}, {2e6}, 0, 1, 0, 0, SyncNone, {1164, 1610}, "CDMA below the noise floor"},
};
const int kKnownCount = (int)(sizeof kKnown / sizeof *kKnown);

const std::vector<Known>& signatures() {
    static const std::vector<Known> table = [] {
        std::vector<Known> v(kKnown, kKnown + kKnownCount);
        // a mode (sub "") counts as covered by any row of its id; a sub-mode by a row of its own
        auto covered = [&](const char* mode, const char* sub) {
            for (const Known& k : v) if (!std::strcmp(k.modeId, mode) && (!*sub || !std::strcmp(k.subId, sub))) return true;
            return false;
        };
        for (int i = 0; i < kKnownMeasuredCount; i++) {
            const Known& m = kKnownMeasured[i];
            if (covered(m.modeId, m.subId)) continue;
            v.push_back(m);
            v.back().source = 1;
        }
        int nsub = 0;
        const SubMode* subs = subModes(nsub);
        auto hasSubs = [&](int stdMode) { for (int i = 0; i < nsub; i++) if (subs[i].stdMode == stdMode) return true; return false; };
        auto fromList = [&](const char* name, const char* mode, const char* sub, double defMhz) {
            Known k{};
            k.name = name; k.modeId = mode; k.subId = sub; k.family = "Other modes";
            k.mods = 0; k.mod = ModUnknown; k.sync = SyncNone;
            k.note = "from the mode list: no signature measured for it yet";
            k.defMhz = defMhz; k.source = 2;
            v.push_back(k);
        };
        for (int s = 8; s < 200; s++) {
            const ModeTuning* mt = modeTuning(s);
            if (mt && !hasSubs(s) && !covered(mt->id, "")) fromList(mt->name, mt->id, "", mt->defMhz);
        }
        for (int i = 0; i < nsub; i++) {
            const ModeTuning* mt = modeTuning(subs[i].stdMode);
            if (mt && !covered(mt->id, subs[i].id)) fromList(subs[i].name, mt->id, subs[i].id, subs[i].defMhz);
        }
        // rows without bands: the mode's usual frequency from the mode list is their prior
        for (Known& k : v) {
            if (k.bands[0] > 0 || k.defMhz > 0) continue;
            if (*k.subId) { if (const SubMode* sm = subModeById(k.subId)) k.defMhz = sm->defMhz; }
            else if (const ModeTuning* mt = modeTuningById(k.modeId)) k.defMhz = mt->defMhz;
        }
        return v;
    }();
    return table;
}

} // namespace sigid
} // namespace dect2
