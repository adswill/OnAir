// Radio access for the Android bridge (jni_radio.cpp): USB radios handed over as descriptors, network radios, the pushed sound-card input.
// jni_onair.cpp asks it for the DeviceInfo of the source kinds it adds (USB, network, sound card).
#pragma once
#include <string>
#include "dect2/source/source.h"

namespace onair {

// The bridge's source numbers (enum Source of jni_onair.cpp continues with these; SourceKind in Models.kt)
enum RadioSource { kSrcUsb = 4, kSrcNet = 5, kSrcSoundCard = 6 };

// The radio to open for a source kind: USB: `key` is the registry name of the descriptor ("/dev/bus/usb/001/004"); network: `key` is one
// radio in the settings' text form ("rtltcp,host,port", "kiwisdr,host,port,password", see netradio.h); sound card: `key` is ignored.
// false: err in plain words.
bool radioDevice(int kind, const std::string& key, dect2::DeviceInfo& out, std::string& err);

// The device JSON of jni_onair.cpp (name, gains, rates, bias tee, settings), defined there
std::string deviceJsonOf(const dect2::DeviceInfo& d);

} // namespace onair

namespace dect2 { class Engine; }
namespace onair {
// The bridge's one engine (jni_onair.cpp), or nullptr before nativeInit: for jni_tv.cpp's packet tap and service list
dect2::Engine* bridgeEngine();

} // namespace onair
