// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Graphics back end behind the UI: Metal (macOS) or OpenGL 3 (everything else, and optionally macOS).
#pragma once
#include "imgui.h"
#include <cstdint>

struct GLFWwindow;

namespace gfx {

// Picture produced by the video player: converts NV12 (or takes RGBA) into a texture the UI can show.
struct Video {
    virtual ~Video() {}
    virtual ImTextureID texture() const = 0;
    virtual void uploadRGBA(const uint8_t* rgba) = 0;
    // y: w x h bytes, uv: interleaved CbCr at half size (w/2 x h/2 pairs, row stride w bytes)
    virtual bool uploadNV12(const uint8_t* y, const uint8_t* uv, bool bt709, bool fullRange, bool deinterlace) = 0;
};

// A plain RGBA8 texture that can be updated by rows (the waterfall).
struct Image {
    virtual ~Image() {}
    virtual ImTextureID texture() const = 0;
    virtual void update(int x, int y, int w, int h, const uint32_t* rgba) = 0;
};

// A second OS window drawn with the same device (Metal) or a context sharing the main one (OpenGL): the video pop-out.
struct Surface {
    virtual ~Surface() {}
    // draw dd (coordinates in window points) on a black background and present; fbW x fbH is the framebuffer in pixels
    virtual void present(ImDrawData* dd, int fbW, int fbH) = 0;
};

struct Backend {
    virtual ~Backend() {}
    virtual Image* createImage(int w, int h, uint32_t fill) = 0;
    virtual Video* createVideo(int w, int h) = 0;
    // The widest (and tallest) picture the GPU takes in pixels: an Image asked larger is kept at this size and shrunk on update (#284).
    virtual int maxImageSize() const { return 16384; }
    virtual void newFrame(int fbW, int fbH, const float clear[4]) = 0;   // before ImGui::NewFrame(); sizes in pixels
    // render the draw data and present; if shotPath is set the finished frame is written there as a PNG
    virtual void endFrame(ImDrawData* dd, const char* shotPath) = 0;
    // A frame that nobody can see (the window is covered): true when the back end can end it without drawing (skipFrame() instead of
    // endFrame()), so the interface keeps running but the GPU does nothing. Metal; the OpenGL back end draws every frame.
    virtual bool canSkipFrame() const { return false; }
    virtual void skipFrame() {}
    // The window a new Surface's window must share its context with (OpenGL), or null (Metal: the window has no context).
    virtual GLFWwindow* shareWindow() const { return nullptr; }
    // After the window exists (created after windowHints() and with shareWindow()); delete the Surface before destroying the window.
    virtual Surface* createSurface(GLFWwindow* window) = 0;
    virtual void shutdown() = 0;
};

// Context levels, best first (OpenGL: 3.2 core, 3.1, 3.0; Metal: one). The caller tries each until the window opens (#283).
int levelCount();
int level();
void setLevel(int l);                     // before windowHints(); the pop-out window then uses the same level as the main one
const char* levelName();
int glesLevel();                          // OpenGL only: the OpenGL ES 3.0 / EGL level, tried on Linux when desktop OpenGL is a software renderer (#470)
void windowHints();                       // before glfwCreateWindow
Backend* create(GLFWwindow* window);      // after the window exists; also initialises the ImGui renderer back end

bool writePng(const char* path, const uint8_t* rgba, int w, int h);   // shared helper (gfx_png.cpp)

}
