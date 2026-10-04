// Casting the playing service to an AirPlay device (Apple TV, AirPlay TVs and speakers). macOS only: the system's AirPlay does the work;
// OnAir serves the service as an HLS stream (the network tuner) and hands that address to the system player, which passes it on to the device.
#pragma once
#include <string>

struct GLFWwindow;

namespace airplay {

enum class State { Idle, Choosing, Casting, Failed };

#ifdef __APPLE__
bool available();
// Opens the system's AirPlay device list next to the given rectangle (window points, origin top left) and plays `url` on the chosen device.
void choose(GLFWwindow* window, float x, float y, float w, float h, const std::string& url);
void stop();
State state();            // call every frame: it also ends a cast whose device list was closed without a choice
std::string message();    // what went wrong, when the state is Failed
#else
inline bool available() { return false; }
inline void choose(GLFWwindow*, float, float, float, float, const std::string&) {}
inline void stop() {}
inline State state() { return State::Idle; }
inline std::string message() { return std::string(); }
#endif

} // namespace airplay
