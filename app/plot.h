// Our own plotting on top of Dear ImGui's draw lists: axes with ticks and grid, lines, dots, bars, shaded areas, images and heat maps,
// panning and zooming with the mouse. It replaces the ImPlot library; the names follow ImPlot's, so a plot reads the same way.
//
//   if (plt::BeginPlot("##id", ImVec2(-1, 200), plt::Flags_NoLegend)) {
//       plt::SetupAxes("x label", "y label");
//       plt::SetupAxisLimits(plt::X1, 0, 10, plt::Cond_Once);   // Once: the user may pan and zoom afterwards; Always: fixed
//       plt::Spec s; s.LineColor = ImVec4(1, 1, 1, 1);
//       plt::PlotLine("name", xs, ys, n, s);
//       plt::EndPlot();
//   }
#pragma once
#include "imgui.h"
#include <cstddef>
#include <cstdint>

namespace plt {

enum Axis { X1 = 0, Y1 = 1 };
enum Cond { Cond_Always, Cond_Once };

enum Flags { Flags_None = 0, Flags_NoTitle = 1, Flags_NoLegend = 2, Flags_Equal = 4, Flags_NoMouseText = 8 };
enum AxisFlags { AxisFlags_None = 0, AxisFlags_NoTickLabels = 1, AxisFlags_NoGridLines = 2, AxisFlags_AutoFit = 4 };
enum Scale { Scale_Linear, Scale_Log10 };
enum MarkerType { Marker_None, Marker_Circle, Marker_Cross };
enum LineFlags { InfLines_Vertical = 0, InfLines_Horizontal = 1 };
enum SubplotFlags { Subplot_None = 0, Subplot_NoTitle = 1, Subplot_NoLegend = 2, Subplot_NoMenus = 4, Subplot_LinkAllX = 8 };

struct Point { double x, y; Point(double x_ = 0, double y_ = 0) : x(x_), y(y_) {} };
struct Range { double Min = 0, Max = 1; };
struct Rect { Range X, Y; };

// How one series looks. A colour with alpha 0 and a negative LineWeight mean "automatic" (the next colour of the palette).
struct Spec {
    ImVec4 LineColor = ImVec4(0, 0, 0, -1);   // alpha < 0: automatic
    float LineWeight = 1.0f;
    ImVec4 FillColor = ImVec4(0, 0, 0, -1);
    MarkerType Marker = Marker_None;
    float MarkerSize = 4.0f;
    ImVec4 MarkerFillColor = ImVec4(0, 0, 0, -1);
    ImVec4 MarkerLineColor = ImVec4(0, 0, 0, -1);
    int Stride = 0;   // bytes between values; 0 = the size of one value
    int Flags = 0;    // InfLines_Horizontal for PlotInfLines
};

// Colours and sizes of the plots (change them through Style()).
struct Style {
    ImVec4 PlotBg = ImVec4(0.02f, 0.021f, 0.023f, 1);
    ImVec4 Border = ImVec4(0.24f, 0.25f, 0.26f, 1);
    ImVec4 Grid = ImVec4(0.60f, 0.62f, 0.64f, 0.16f);
    ImVec4 Text = ImVec4(0.62f, 0.64f, 0.66f, 1);
    float Pad = 4;           // between the frame and the axis text, in pixels at scale 1
    float TickLen = 4;
    float Scale = 1;         // the interface scale (high-resolution screens)
};
Style& GetStyle();

bool BeginPlot(const char* id, ImVec2 size = ImVec2(-1, 0), int flags = 0);
void EndPlot();

bool BeginSubplots(const char* id, int rows, int cols, ImVec2 size, int flags = 0);
void EndSubplots();

void SetupAxes(const char* xLabel, const char* yLabel, int xFlags = 0, int yFlags = 0);
void SetupAxisLimits(Axis axis, double min, double max, Cond cond = Cond_Once);
void SetupAxisFormat(Axis axis, const char* fmt);
void SetupAxisScale(Axis axis, Scale scale);
void SetupAxisTicks(Axis axis, const double* values, int n, const char* const* labels);

// Data given as arrays of float or double (Spec::Stride for interleaved data).
template <class T> void PlotLine(const char* label, const T* xs, const T* ys, int n, const Spec& s = Spec());
template <class T> void PlotLine(const char* label, const T* ys, int n, const Spec& s = Spec());   // x = 0, 1, 2, ...
template <class T> void PlotScatter(const char* label, const T* xs, const T* ys, int n, const Spec& s = Spec());
template <class T> void PlotScatter(const char* label, const T* ys, int n, double xScale, double xStart, const Spec& s = Spec());   // x = xStart + i * xScale
template <class T> void PlotBars(const char* label, const T* xs, const T* ys, int n, double width, const Spec& s = Spec());
template <class T> void PlotShaded(const char* label, const T* xs, const T* ys, int n, double yRef, const Spec& s = Spec());
template <class T> void PlotInfLines(const char* label, const T* values, int n, const Spec& s = Spec());
void PlotImage(const char* label, ImTextureID tex, Point bmin, Point bmax, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1));
// values: rows x cols, row 0 at the top; the colour comes from the colour map set with PushColormap
void PlotHeatmap(const char* label, const float* values, int rows, int cols, double scaleMin, double scaleMax, const char* fmt, Point bmin, Point bmax);
void PlotText(const char* text, double x, double y, ImVec2 pixelOffset = ImVec2(0, 0));

// A colour map is a list of colours that are blended into each other.
int AddColormap(const char* name, const ImVec4* colors, int n);
void PushColormap(int map);
void PopColormap();

Point GetPlotMousePos();
bool IsPlotHovered();
Rect GetPlotLimits();

} // namespace plt
