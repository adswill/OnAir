// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Native radios (RTL-SDR, Airspy, HydraSDR, BladeRF, LimeSDR, PlutoSDR, USRP, SDRplay, Fobos SDR, RFNM, Mirics MSi2500 / RSP1): the list of connected ones and the way to open one.
// The drivers are in source_rtlsdr.cpp, source_airspy.cpp, source_hydrasdr.cpp, source_bladerf.cpp, source_limesdr.cpp, source_plutosdr.cpp, source_usrp.cpp,
// source_sdrplay.cpp, source_airspyhf.cpp, source_fobos.cpp and source_rx888.cpp;
// each loads the radio's own library at run time (native_common.h), so nothing here depends on those libraries being installed.
// The exception is the RFNM (source_rfnm.cpp): librfnm has a C++ interface only, so it is linked, and only when CMake found it (DECT2_HAVE_RFNM).
#include "native_common.h"

namespace dect2 {

// ---- airspyhf
namespace native {
void listAirspyHf(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeAirspyHf(const DeviceInfo& d);
// ---- fobos
void listFobos(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeFobos(const DeviceInfo& d);
// ---- rx888
void listRx888(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeRx888(const DeviceInfo& d);
// ---- icom (IC-R8600 I/Q, receive only)
void listIcom(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeIcom(const DeviceInfo& d);
// ---- mirics (SDRplay RSP1 / MSi2500 + MSi001: OnAir's own libusb driver, mirics_usb.cpp)
void listMirics(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeMirics(const DeviceInfo& d);
// ---- spectran (Aaronia SPECTRAN V6 / V6 ECO through the RTSA API, source_spectran.cpp)
void listSpectran(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeSpectran(const DeviceInfo& d);
std::unique_ptr<TxSink> makeSpectranTx(const DeviceInfo& d);
#ifdef DECT2_HAVE_RFNM
// ---- rfnm
void listRfnm(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeRfnm(const DeviceInfo& d);
#endif
}

std::vector<DeviceInfo> listNativeDevices(std::string& err) {
    std::vector<DeviceInfo> out;
    if (native::nativeDisabled()) return out;
    try {
        native::listRtl(out);
        native::listAirspy(out);
        native::listHydra(out);
        native::listBlade(out);
        native::listLime(out);
        native::listPluto(out);
        native::listUsrp(out);
        native::listSdrplay(out);
        native::listAirspyHf(out);   // ---- airspyhf
        native::listFobos(out);      // ---- fobos
        native::listRx888(out);      // ---- rx888
        native::listIcom(out);       // ---- icom
        native::listSpectran(out);   // ---- spectran
        native::listMirics(out);     // ---- mirics
#ifdef DECT2_HAVE_RFNM
        native::listRfnm(out);       // ---- rfnm
#endif
    } catch (const std::exception& e) {
        err = std::string("native radios: ") + e.what();
    }
    // An RX888 MkII is a Cypress FX3 (04b4:00f1), the USB ID LimeSuite also takes for a LimeSDR-USB, and it reports the same serial. The RX888
    // driver names that radio, so the LimeSDR entries with its serial are dropped: otherwise the first radio in the list (the Lime) is the
    // one started, and its "initialisation failed" is what the user sees for an RX888.
    // A HydraSDR RFOne carries the Airspy's USB id (1d50:60a1): on systems where libairspy lists it, its own driver names it and the
    // Airspy entry with the same serial is dropped
    std::vector<std::string> hydraSerials;
    for (const auto& d : out) if (d.board == "hydrasdr" && !d.serial.empty()) hydraSerials.push_back(d.serial);
    if (!hydraSerials.empty())
        out.erase(std::remove_if(out.begin(), out.end(), [&](const DeviceInfo& d) {
            return d.board == "airspy" && std::find(hydraSerials.begin(), hydraSerials.end(), d.serial) != hydraSerials.end();
        }), out.end());
    std::vector<std::string> rx888Serials;
    for (const auto& d : out) if (d.board == "rx888" && !d.serial.empty()) rx888Serials.push_back(d.serial);
    if (!rx888Serials.empty())
        out.erase(std::remove_if(out.begin(), out.end(), [&](const DeviceInfo& d) {
            return d.board == "lime" && std::find(rx888Serials.begin(), rx888Serials.end(), d.serial) != rx888Serials.end();
        }), out.end());
    // An RSP1 that the SDRplay API lists is driven by the API (its service holds the radio, so this driver could not open it): the own
    // driver's entry for the same radio is dropped
    const bool apiRsp1 = std::any_of(out.begin(), out.end(), [](const DeviceInfo& d) { return d.board == "sdrplay" && d.name.rfind("SDRplay RSP1 ", 0) == 0; });
    if (apiRsp1)
        out.erase(std::remove_if(out.begin(), out.end(), [](const DeviceInfo& d) {
            return d.board == "mirics" && d.nativeArgs.size() >= 9 && d.nativeArgs.compare(d.nativeArgs.size() - 9, 9, "1df7:2500") == 0;
        }), out.end());
    return out;
}

// The radios of the native drivers that can transmit: the PlutoSDR (libiio). The LimeSDR, BladeRF and USRP can transmit through SoapySDR
// when its module is installed; their native drivers here are receive-only.
std::unique_ptr<TxSink> makeNativeTxSink(const DeviceInfo& d) {
    if (d.board == "pluto") return native::makePlutoTx(d);
    if (d.board == "usrp") return native::makeUsrpTx(d);
    if (d.board == "spectran") return native::makeSpectranTx(d);   // ---- spectran
    return nullptr;
}

std::unique_ptr<IqSource> makeNativeSource(const DeviceInfo& d) {
    if (d.board == "rtlsdr") return native::makeRtl(d);
    if (d.board == "airspy") return native::makeAirspy(d);
    if (d.board == "hydrasdr") return native::makeHydra(d);
    if (d.board == "bladerf") return native::makeBlade(d);
    if (d.board == "lime") return native::makeLime(d);
    if (d.board == "pluto") return native::makePluto(d);
    if (d.board == "usrp") return native::makeUsrp(d);
    if (d.board == "sdrplay") return native::makeSdrplay(d);
    if (d.board == "airspyhf") return native::makeAirspyHf(d);   // ---- airspyhf
    if (d.board == "rtltcp") return native::makeRtlTcp(d);
    if (d.board == "spyserver") return native::makeSpyServer(d);
    if (d.board == "sdrpp") return native::makeSdrpp(d);
    if (d.board == "tci") return native::makeTci(d);
    if (d.board == "nrspst") return native::makeNrspSt(d);
    if (d.board == "kiwisdr") return native::makeKiwiSdr(d);
    if (d.board == "hermeslite2") return native::makeHermesLite2(d);
    if (d.board == "redpitaya") return native::makeRedPitaya(d);
    if (d.board == "fobos") return native::makeFobos(d);         // ---- fobos
    if (d.board == "rx888") return native::makeRx888(d);         // ---- rx888
    if (d.board == "icr8600") return native::makeIcom(d);        // ---- icom
    if (d.board == "spectran") return native::makeSpectran(d);   // ---- spectran
    if (d.board == "mirics") return native::makeMirics(d);       // ---- mirics
#ifdef DECT2_HAVE_RFNM
    if (d.board == "rfnm") return native::makeRfnm(d);           // ---- rfnm
#endif
    return nullptr;
}

} // namespace dect2
