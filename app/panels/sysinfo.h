// This computer for the debug report (system.txt): the processor and its instruction set extensions (SIMD), what OnAir was built for
// and uses, memory, graphics (OpenGL strings, the GPU decoder's device, the screens) and the operating system. Call it on the main
// thread (it reads the OpenGL strings of the current context).
#pragma once
#include <string>

struct App;

std::string systemInfoReport(App& a);
// One line for report.txt: "OnAir 3.9.0 on <OS>, <CPU>, <GPU>, SIMD: <level>"
std::string systemInfoSummary();
