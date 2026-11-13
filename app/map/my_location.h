#pragma once
#include "imgui.h"
#include <string>

namespace mylocmap {

bool markerOn();
bool ringsOn();
void overlay(ImDrawList* dl, ImVec2 origin, ImVec2 size, bool hovered);
bool describeFromMe(double lat, double lon, std::string& out);
void infoLine(double lat, double lon);
std::string distanceBearingText(double lat, double lon);

}
