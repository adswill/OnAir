// OpenGL 3.2 core back end (Linux, Windows, and macOS when built with DECT2_UI_BACKEND=OpenGL3).
#include "gfx.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gfx {
namespace {

// The few GL entry points we need, loaded through GLFW (no GL headers or loader library required).
typedef unsigned int GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned int GLenum; typedef unsigned char GLubyte;
typedef float GLfloat; typedef char GLchar; typedef unsigned int GLbitfield;
enum : GLenum {
    TEXTURE_2D = 0x0DE1, RGBA = 0x1908, RED = 0x1903, RG = 0x8227, R8 = 0x8229, RG8 = 0x822B, RGBA8 = 0x8058, UNSIGNED_BYTE = 0x1401,
    TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, LINEAR = 0x2601, NEAREST = 0x2600, TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803,
    CLAMP_TO_EDGE = 0x812F, FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, UNPACK_ALIGNMENT = 0x0CF5, FRAGMENT_SHADER = 0x8B30,
    VERTEX_SHADER = 0x8B31, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, TRIANGLES = 0x0004, COLOR_BUFFER_BIT = 0x4000, TEXTURE0 = 0x84C0,
    TEXTURE1 = 0x84C1, FRAMEBUFFER_BINDING = 0x8CA6, VIEWPORT = 0x0BA2, CURRENT_PROGRAM = 0x8B8D, TEXTURE_BINDING_2D = 0x8069,
    ACTIVE_TEXTURE = 0x84E0, VERTEX_ARRAY_BINDING = 0x85B5, BACK = 0x0405, FRAMEBUFFER_COMPLETE = 0x8CD5
};

#define GLFN(ret, name, args) typedef ret (*PFN_##name) args; PFN_##name name = nullptr;
GLFN(void, glGenTextures, (GLsizei, GLuint*))
GLFN(void, glDeleteTextures, (GLsizei, const GLuint*))
GLFN(void, glBindTexture, (GLenum, GLuint))
GLFN(void, glTexParameteri, (GLenum, GLenum, GLint))
GLFN(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))
GLFN(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))
GLFN(void, glPixelStorei, (GLenum, GLint))
GLFN(void, glActiveTexture, (GLenum))
GLFN(void, glGenFramebuffers, (GLsizei, GLuint*))
GLFN(void, glDeleteFramebuffers, (GLsizei, const GLuint*))
GLFN(void, glBindFramebuffer, (GLenum, GLuint))
GLFN(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLFN(GLenum, glCheckFramebufferStatus, (GLenum))
GLFN(GLuint, glCreateShader, (GLenum))
GLFN(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
GLFN(void, glCompileShader, (GLuint))
GLFN(void, glGetShaderiv, (GLuint, GLenum, GLint*))
GLFN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLFN(void, glDeleteShader, (GLuint))
GLFN(GLuint, glCreateProgram, ())
GLFN(void, glAttachShader, (GLuint, GLuint))
GLFN(void, glLinkProgram, (GLuint))
GLFN(void, glGetProgramiv, (GLuint, GLenum, GLint*))
GLFN(void, glUseProgram, (GLuint))
GLFN(GLint, glGetUniformLocation, (GLuint, const GLchar*))
GLFN(void, glUniform1i, (GLint, GLint))
GLFN(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GLFN(void, glGenVertexArrays, (GLsizei, GLuint*))
GLFN(void, glBindVertexArray, (GLuint))
GLFN(void, glDrawArrays, (GLenum, GLint, GLsizei))
GLFN(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
GLFN(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
GLFN(void, glClear, (GLbitfield))
GLFN(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))
GLFN(void, glReadBuffer, (GLenum))
GLFN(void, glGetIntegerv, (GLenum, GLint*))
GLFN(void, glFlush, ())
#undef GLFN

bool loadGl() {
    bool ok = true;
#define LOAD(n) { n = (PFN_##n)glfwGetProcAddress(#n); if (!n) { fprintf(stderr, "OpenGL function %s missing\n", #n); ok = false; } }
    LOAD(glGenTextures) LOAD(glDeleteTextures) LOAD(glBindTexture) LOAD(glTexParameteri) LOAD(glTexImage2D) LOAD(glTexSubImage2D)
    LOAD(glPixelStorei) LOAD(glActiveTexture) LOAD(glGenFramebuffers) LOAD(glDeleteFramebuffers) LOAD(glBindFramebuffer)
    LOAD(glFramebufferTexture2D) LOAD(glCheckFramebufferStatus) LOAD(glCreateShader) LOAD(glShaderSource) LOAD(glCompileShader)
    LOAD(glGetShaderiv) LOAD(glGetShaderInfoLog) LOAD(glDeleteShader) LOAD(glCreateProgram) LOAD(glAttachShader) LOAD(glLinkProgram)
    LOAD(glGetProgramiv) LOAD(glUseProgram) LOAD(glGetUniformLocation) LOAD(glUniform1i) LOAD(glUniform3f) LOAD(glGenVertexArrays)
    LOAD(glBindVertexArray) LOAD(glDrawArrays) LOAD(glViewport) LOAD(glClearColor) LOAD(glClear) LOAD(glReadPixels) LOAD(glReadBuffer)
    LOAD(glGetIntegerv) LOAD(glFlush)
#undef LOAD
    return ok;
}

GLuint makeTexture(GLint internal, GLenum fmt, int w, int h, const void* px, GLenum filter) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(TEXTURE_2D, t);
    glTexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, (GLint)filter);
    glTexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
    glTexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
    glPixelStorei(UNPACK_ALIGNMENT, 1);
    glTexImage2D(TEXTURE_2D, 0, internal, w, h, 0, fmt, UNSIGNED_BYTE, px);
    glPixelStorei(UNPACK_ALIGNMENT, 4);
    return t;
}

// Saves and restores the GL state we touch outside of the ImGui renderer.
struct StateGuard {
    GLint fbo, prog, tex, active, vao, vp[4];
    StateGuard() {
        glGetIntegerv(FRAMEBUFFER_BINDING, &fbo); glGetIntegerv(CURRENT_PROGRAM, &prog); glGetIntegerv(ACTIVE_TEXTURE, &active);
        glGetIntegerv(TEXTURE_BINDING_2D, &tex); glGetIntegerv(VERTEX_ARRAY_BINDING, &vao); glGetIntegerv(VIEWPORT, vp);
    }
    ~StateGuard() {
        glBindFramebuffer(FRAMEBUFFER, (GLuint)fbo); glUseProgram((GLuint)prog); glBindVertexArray((GLuint)vao);
        glActiveTexture((GLenum)active); glBindTexture(TEXTURE_2D, (GLuint)tex); glViewport(vp[0], vp[1], vp[2], vp[3]);
    }
};

struct GlImage : Image {
    GLuint tex = 0;
    GlImage(int w, int h, uint32_t fill) {
        std::vector<uint32_t> px((size_t)w * h, fill);
        StateGuard g;
        tex = makeTexture(RGBA8, RGBA, w, h, px.data(), LINEAR);
    }
    ~GlImage() override { if (tex) glDeleteTextures(1, &tex); }
    ImTextureID texture() const override { return (ImTextureID)(intptr_t)tex; }
    void update(int x, int y, int w, int h, const uint32_t* rgba) override {
        StateGuard g;
        glBindTexture(TEXTURE_2D, tex);
        glTexSubImage2D(TEXTURE_2D, 0, x, y, w, h, RGBA, UNSIGNED_BYTE, rgba);
    }
};

const char* kVert = R"(#version 150
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
// NV12 -> RGB, same maths as the Metal kernel (limited/full range, BT.601/709, optional comb removal for interlaced pictures)
const char* kFrag = R"(#version 150
uniform sampler2D Y;
uniform sampler2D UV;
uniform vec3 prm;     // x: BT.709, y: full range, z: deinterlace
out vec4 o;
void main() {
    ivec2 g = ivec2(gl_FragCoord.xy);
    int H = textureSize(Y, 0).y;
    float y = texelFetch(Y, g, 0).r;
    if (prm.z > 0.5) {
        float ya = texelFetch(Y, ivec2(g.x, g.y > 0 ? g.y - 1 : g.y), 0).r;
        float yb = texelFetch(Y, ivec2(g.x, g.y + 1 < H ? g.y + 1 : g.y), 0).r;
        float comb = (y - ya) * (y - yb);
        float w = clamp((comb - 0.0008) / 0.004, 0.0, 1.0);
        y = mix(y, 0.5 * (ya + yb), w);
    }
    vec2 c = texelFetch(UV, g / 2, 0).rg;
    if (prm.y < 0.5) { y = (y - 16.0 / 255.0) * (255.0 / 219.0); c = (c - 128.0 / 255.0) * (255.0 / 224.0); }
    else c -= 0.5;
    vec3 rgb;
    if (prm.x > 0.5) rgb = vec3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x);
    else rgb = vec3(y + 1.402 * c.y, y - 0.344136 * c.x - 0.714136 * c.y, y + 1.772 * c.x);
    o = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log); fprintf(stderr, "shader: %s\n", log); glDeleteShader(s); return 0; }
    return s;
}

struct Converter {
    GLuint prog = 0, vao = 0, locY = -1, locUV = -1, locPrm = -1;
    bool tried = false;
    bool ready() {
        if (prog) return true;
        if (tried) return false;
        tried = true;
        GLuint v = compile(VERTEX_SHADER, kVert), f = compile(FRAGMENT_SHADER, kFrag);
        if (!v || !f) return false;
        prog = glCreateProgram();
        glAttachShader(prog, v); glAttachShader(prog, f);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, LINK_STATUS, &ok);
        if (!ok) { fprintf(stderr, "video shader failed to link\n"); prog = 0; return false; }
        glDeleteShader(v); glDeleteShader(f);
        locY = glGetUniformLocation(prog, "Y"); locUV = glGetUniformLocation(prog, "UV"); locPrm = glGetUniformLocation(prog, "prm");
        glGenVertexArrays(1, &vao);
        return true;
    }
};
Converter gConv;

struct GlVideo : Video {
    int w, h;
    GLuint out = 0, yTex = 0, uvTex = 0, fbo = 0;
    GlVideo(int w_, int h_) : w(w_), h(h_) {
        StateGuard g;
        out = makeTexture(RGBA8, RGBA, w, h, nullptr, LINEAR);
        yTex = makeTexture(R8, RED, w, h, nullptr, NEAREST);
        uvTex = makeTexture(RG8, RG, w / 2, h / 2, nullptr, NEAREST);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(FRAMEBUFFER, fbo);
        glFramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, out, 0);
    }
    ~GlVideo() override { glDeleteTextures(1, &out); glDeleteTextures(1, &yTex); glDeleteTextures(1, &uvTex); glDeleteFramebuffers(1, &fbo); }
    ImTextureID texture() const override { return (ImTextureID)(intptr_t)out; }
    void uploadRGBA(const uint8_t* rgba) override {
        StateGuard g;
        glBindTexture(TEXTURE_2D, out);
        glPixelStorei(UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(TEXTURE_2D, 0, 0, 0, w, h, RGBA, UNSIGNED_BYTE, rgba);
        glPixelStorei(UNPACK_ALIGNMENT, 4);
    }
    bool uploadNV12(const uint8_t* y, const uint8_t* uv, bool bt709, bool fullRange, bool deint) override {
        StateGuard g;
        if (!gConv.ready()) return false;
        glPixelStorei(UNPACK_ALIGNMENT, 1);
        glActiveTexture(TEXTURE0); glBindTexture(TEXTURE_2D, yTex);
        glTexSubImage2D(TEXTURE_2D, 0, 0, 0, w, h, RED, UNSIGNED_BYTE, y);
        glActiveTexture(TEXTURE1); glBindTexture(TEXTURE_2D, uvTex);
        glTexSubImage2D(TEXTURE_2D, 0, 0, 0, w / 2, h / 2, RG, UNSIGNED_BYTE, uv);
        glPixelStorei(UNPACK_ALIGNMENT, 4);
        glBindFramebuffer(FRAMEBUFFER, fbo);
        glViewport(0, 0, w, h);
        glUseProgram(gConv.prog);
        glUniform1i((GLint)gConv.locY, 0); glUniform1i((GLint)gConv.locUV, 1);
        glUniform3f((GLint)gConv.locPrm, bt709 ? 1.f : 0.f, fullRange ? 1.f : 0.f, deint ? 1.f : 0.f);
        glBindVertexArray(gConv.vao);
        glDrawArrays(TRIANGLES, 0, 3);
        glActiveTexture(TEXTURE0);
        return true;
    }
};

// The pop-out window: its own context, sharing textures and buffers with the main one (the ImGui renderer makes its vertex array per draw,
// as those are not shared). The main context is current again afterwards.
struct GlSurface : Surface {
    GLFWwindow* win;
    explicit GlSurface(GLFWwindow* w) : win(w) {
        GLFWwindow* prev = glfwGetCurrentContext();
        glfwMakeContextCurrent(win);
        glfwSwapInterval(0);   // the main window already waits for the display: two waits per frame would halve the frame rate
        glfwMakeContextCurrent(prev);
    }
    void present(ImDrawData* dd, int fbW, int fbH) override {
        if (fbW <= 0 || fbH <= 0) return;
        GLFWwindow* prev = glfwGetCurrentContext();
        if (prev) glFlush();   // the picture converted in the main context must reach the GPU before another context samples it
        glfwMakeContextCurrent(win);
        glBindFramebuffer(FRAMEBUFFER, 0);
        glViewport(0, 0, fbW, fbH);
        glClearColor(0, 0, 0, 1);
        glClear(COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(dd);
        glfwSwapBuffers(win);
        glfwMakeContextCurrent(prev);
    }
};

struct GlBackend : Backend {
    GLFWwindow* win;
    explicit GlBackend(GLFWwindow* w) : win(w) {}
    Image* createImage(int w, int h, uint32_t fill) override { return new GlImage(w, h, fill); }
    Video* createVideo(int w, int h) override { return new GlVideo(w, h); }
    int fbW = 0, fbH = 0;
    float clear[4] = {0, 0, 0, 1};
    void newFrame(int w, int h, const float c[4]) override { fbW = w; fbH = h; std::memcpy(clear, c, sizeof clear); ImGui_ImplOpenGL3_NewFrame(); }
    void endFrame(ImDrawData* dd, const char* shotPath) override {
        glBindFramebuffer(FRAMEBUFFER, 0);
        glViewport(0, 0, fbW, fbH);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glClear(COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(dd);
        if (shotPath) {
            std::vector<uint8_t> px((size_t)fbW * fbH * 4), flipped(px.size());
            glReadBuffer(BACK);
            glReadPixels(0, 0, fbW, fbH, RGBA, UNSIGNED_BYTE, px.data());
            for (int y = 0; y < fbH; y++) std::memcpy(&flipped[(size_t)y * fbW * 4], &px[(size_t)(fbH - 1 - y) * fbW * 4], (size_t)fbW * 4);
            for (size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
            writePng(shotPath, flipped.data(), fbW, fbH);
        }
        glfwSwapBuffers(win);
    }
    GLFWwindow* shareWindow() const override { return win; }
    Surface* createSurface(GLFWwindow* w) override { return new GlSurface(w); }
    void shutdown() override { ImGui_ImplOpenGL3_Shutdown(); }
};

} // namespace

void windowHints() {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
}

Backend* create(GLFWwindow* window) {
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!loadGl()) return nullptr;
    if (!ImGui_ImplOpenGL3_Init("#version 150")) return nullptr;
    return new GlBackend(window);
}

}
