// Native radios (RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP, SDRplay): the list of connected ones and the way to open one.
// The drivers are in source_rtlsdr.cpp, source_airspy.cpp, source_bladerf.cpp, source_limesdr.cpp, source_plutosdr.cpp, source_usrp.cpp and
// source_sdrplay.cpp;
// each loads the radio's own library at run time (native_common.h), so nothing here depends on those libraries being installed.
#include "native_common.h"

namespace dect2 {

// ---- airspyhf
namespace native {
void listAirspyHf(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeAirspyHf(const DeviceInfo& d);
}

std::vector<DeviceInfo> listNativeDevices(std::string& err) {
    std::vector<DeviceInfo> out;
    if (native::nativeDisabled()) return out;
    try {
        native::listRtl(out);
        native::listAirspy(out);
        native::listBlade(out);
        native::listLime(out);
        native::listPluto(out);
        native::listUsrp(out);
        native::listSdrplay(out);
        native::listAirspyHf(out);   // ---- airspyhf
    } catch (const std::exception& e) {
        err = std::string("native radios: ") + e.what();
    }
    return out;
}

std::unique_ptr<IqSource> makeNativeSource(const DeviceInfo& d) {
    if (d.board == "rtlsdr") return native::makeRtl(d);
    if (d.board == "airspy") return native::makeAirspy(d);
    if (d.board == "bladerf") return native::makeBlade(d);
    if (d.board == "lime") return native::makeLime(d);
    if (d.board == "pluto") return native::makePluto(d);
    if (d.board == "usrp") return native::makeUsrp(d);
    if (d.board == "sdrplay") return native::makeSdrplay(d);
    if (d.board == "airspyhf") return native::makeAirspyHf(d);   // ---- airspyhf
    return nullptr;
}

} // namespace dect2
