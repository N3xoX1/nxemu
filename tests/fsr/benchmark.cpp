// SPDX-License-Identifier: GPL-2.0-or-later
// Runs real repository shaders on an offscreen GL context. No game, VSync or FPS cap.
#include "nxemu-module-spec/fsr_sharpness.h"
#include "yuzu_video_core/fsr.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glad/glad.h>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
std::string Read(const fs::path &path) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(f), {}};
}
void Replace(std::string &text, const std::string &from, const std::string &to) {
    const auto p = text.find(from);
    if (p == std::string::npos)
        throw std::runtime_error("Missing shader token: " + from);
    text.replace(p, from.size(), to);
}
GLuint Shader(GLenum stage, const std::string &source) {
    GLuint s = glCreateShader(stage);
    const char *p = source.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[16384];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        throw std::runtime_error(log);
    }
    return s;
}
const char *vertex = R"(#version 460
layout(location=0) out vec2 frag_texcoord;
layout(location=10) uniform vec4 uv_rect = vec4(0,0,1,1);
void main() {
    vec2 p = vec2((gl_VertexID & 1) << 2, (gl_VertexID & 2) << 1);
    gl_Position = vec4(p-1.0,0,1);
    frag_texcoord = mix(uv_rect.xy, uv_rect.zw, p*0.5);
})";
GLuint Program(const std::string &fragment) {
    auto v = Shader(GL_VERTEX_SHADER, vertex), f = Shader(GL_FRAGMENT_SHADER, fragment);
    auto p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) {
        char log[16384];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        throw std::runtime_error(log);
    }
    return p;
}
GLuint Texture(int w, int h, GLenum format, const std::vector<unsigned char> &pixels = {}) {
    GLuint t;
    glCreateTextures(GL_TEXTURE_2D, 1, &t);
    glTextureStorage2D(t, 1, format, w, h);
    glTextureParameteri(t, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(t, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(t, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(t, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!pixels.empty())
        glTextureSubImage2D(t, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return t;
}
std::vector<unsigned char> Pattern(int w, int h) {
    std::vector<unsigned char> p(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (x + 0.5) / w, v = (y + 0.5) / h;
            double checker = ((int(u * 40) ^ int(v * 24)) & 1) ? 0.85 : 0.15;
            double diagonal = std::abs(u - v * 0.7 - 0.1) < 0.004 ? 1.0 : 0.05;
            size_t i = (static_cast<size_t>(y) * w + x) * 4;
            p[i] = static_cast<unsigned char>(255 * (v < 0.5 ? u : checker));
            p[i + 1] = static_cast<unsigned char>(255 * (u < 0.5 ? v : diagonal));
            p[i + 2] = static_cast<unsigned char>(
                255 * (0.5 + 0.45 * std::sin(170 * u) * std::sin(90 * v)));
            p[i + 3] = static_cast<unsigned char>(64 + 191 * u);
        }
    return p;
}
void Save(const fs::path &path, const std::vector<unsigned char> &p, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << ' ' << h << "\n255\n";
    for (size_t i = 0; i < p.size(); i += 4)
        f.write(reinterpret_cast<const char *>(p.data() + i), 3);
}
double Percentile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(q * (v.size() - 1))];
}

int main(int argc, char **argv) try {
    if (argc != 3)
        throw std::runtime_error("Usage: nxemu-fsr-benchmark SOURCE_ROOT OUTPUT_DIRECTORY");
    const fs::path root = argv[1], out = argv[2];
    fs::create_directories(out);
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        throw std::runtime_error(SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    auto *window =
        SDL_CreateWindow("FSR offscreen", 0, 0, 32, 32, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window)
        throw std::runtime_error(SDL_GetError());
    auto context = SDL_GL_CreateContext(window);
    if (!context || !gladLoadGLLoader(SDL_GL_GetProcAddress))
        throw std::runtime_error("GL 4.6 context unavailable");
    std::ofstream device(out / "device.txt");
    device << glGetString(GL_VENDOR) << '\n'
           << glGetString(GL_RENDERER) << '\n'
           << glGetString(GL_VERSION) << '\n';
    auto common = Read(root / "src/yuzu_video_core/host_shaders/opengl_fidelityfx_fsr.frag");
    const auto ffx = root / "src/3rd_party/fidelityfx_fsr/ffx-fsr";
    Replace(common, "#include \"ffx_a.h\"", Read(ffx / "ffx_a.h"));
    Replace(common, "#include \"ffx_fsr1.h\"", Read(ffx / "ffx_fsr1.h"));
    auto easu = Program("#version 460\n#define USE_EASU 1\n" + common);
    auto rcas = Program("#version 460\n#define USE_RCAS 1\n" + common);
    auto direct_easu =
        Program("#version 460\n#define USE_EASU 1\n#define YUZU_PRESENT_EASU 1\n" + common);
    auto copy = Program(R"(#version 460
layout(location=0) in vec2 frag_texcoord;
layout(location=0) out vec4 color;
layout(binding=0) uniform sampler2D source_texture;
void main(){ color=texture(source_texture,frag_texcoord); })");
    GLuint vao, fb;
    glCreateVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glCreateFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glDisable(GL_DITHER);
    glDisable(GL_BLEND);
    auto draw = [&](GLuint prog, GLuint source, GLuint target, int w, int h) {
        glNamedFramebufferTexture(fb, GL_COLOR_ATTACHMENT0, target, 0);
        if (glCheckNamedFramebufferStatus(fb, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Incomplete FBO");
        glViewport(0, 0, w, h);
        glUseProgram(prog);
        glBindTextureUnit(0, source);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    };
    auto pixels = [&](GLuint t, int w, int h) {
        std::vector<unsigned char> p(static_cast<size_t>(w) * h * 4);
        glGetTextureImage(t, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(p.size()),
                          p.data());
        return p;
    };
    std::ofstream summary(out / "summary.csv"), raw(out / "timings.csv");
    summary << "case,width,height,old_median_ms,new_median_ms,old_p95_ms,new_p95_ms,speedup_"
               "percent,rgb_mae,rgb_max_error,alpha_max_error,psnr_old_new_db\n";
    raw << "case,frame,implementation,gpu_ms\n";
    struct Case {
        const char *name;
        int iw, ih, ow, oh;
        bool native, off, flip;
    };
    const Case cases[] = {{"720p-1080p", 1280, 720, 1920, 1080, false, false, false},
                          {"1080p-4k", 1920, 1080, 3840, 2160, false, false, false},
                          {"native-1080p", 1920, 1080, 1920, 1080, true, false, false},
                          {"easu-only", 1280, 720, 1920, 1080, false, true, false},
                          {"easu-flip", 1280, 720, 1920, 1080, false, true, true}};
    for (const auto &c : cases) {
        auto src = Texture(c.iw, c.ih, GL_RGBA8, Pattern(c.iw, c.ih));
        auto a = Texture(c.ow, c.oh, GL_RGBA16F), b = Texture(c.ow, c.oh, GL_RGBA16F);
        auto old = Texture(c.ow, c.oh, GL_RGBA8), candidate = Texture(c.ow, c.oh, GL_RGBA8);
        std::array<u32, 16> ec{};
        std::array<u32, 4> rc{};
        FSR::FsrEasuConOffset(ec.data(), ec.data() + 4, ec.data() + 8, ec.data() + 12, float(c.iw),
                              float(c.ih), float(c.iw), float(c.ih), float(c.ow), float(c.oh), 0,
                              0);
        FSR::FsrRcasCon(rc.data(), 0.25f);
        glProgramUniform4uiv(easu, 0, 4, ec.data());
        glProgramUniform4uiv(direct_easu, 0, 4, ec.data());
        glProgramUniform4uiv(rcas, 0, 1, rc.data());
        glProgramUniform4f(direct_easu, 4, 0, 0, float(c.oh), c.flip ? 0.f : 1.f);
        glProgramUniform1i(direct_easu, 5, 1);
        auto render = [&](bool fused) {
            if (fused && c.off)
                draw(direct_easu, src, candidate, c.ow, c.oh);
            else if (fused) {
                if (!c.native)
                    draw(easu, src, a, c.ow, c.oh);
                draw(rcas, c.native ? src : a, candidate, c.ow, c.oh);
            } else {
                draw(easu, src, a, c.ow, c.oh);
                if (c.off) {
                    glProgramUniform4f(copy, 10, 0, c.flip ? 1.f : 0.f, 1, c.flip ? 0.f : 1.f);
                    draw(copy, a, old, c.ow, c.oh);
                    glProgramUniform4f(copy, 10, 0, 0, 1, 1);
                } else {
                    draw(rcas, a, b, c.ow, c.oh);
                    draw(copy, b, old, c.ow, c.oh);
                }
            }
        };
        for (int i = 0; i < 60; ++i) {
            render(false);
            render(true);
        }
        glFinish();
        GLuint q[2];
        glGenQueries(2, q);
        std::array<std::vector<double>, 2> samples;
        // Alternate order to limit clock/temperature bias. Waits are outside the measured GPU
        // interval.
        for (int i = 0; i < 240; ++i)
            for (int j = 0; j < 2; ++j) {
                const int mode = (i + j) & 1;
                glQueryCounter(q[0], GL_TIMESTAMP);
                render(mode != 0);
                glQueryCounter(q[1], GL_TIMESTAMP);
                GLuint64 t0, t1;
                glGetQueryObjectui64v(q[1], GL_QUERY_RESULT, &t1);
                glGetQueryObjectui64v(q[0], GL_QUERY_RESULT, &t0);
                double ms = double(t1 - t0) / 1.e6;
                samples[mode].push_back(ms);
                raw << c.name << ',' << i << ',' << (mode ? "fused" : "legacy") << ',' << ms
                    << '\n';
            }
        auto p = pixels(old, c.ow, c.oh), n = pixels(candidate, c.ow, c.oh);
        double sum = 0, sq = 0;
        int max_rgb = 0, max_a = 0;
        for (size_t i = 0; i < p.size(); ++i) {
            int d = std::abs(int(p[i]) - int(n[i]));
            if (i % 4 == 3)
                max_a = std::max(max_a, d);
            else {
                sum += d;
                sq += d * d;
                max_rgb = std::max(max_rgb, d);
            }
        }
        const double count = double(c.ow) * c.oh * 3, m0 = Percentile(samples[0], 0.5),
                     m1 = Percentile(samples[1], 0.5);
        summary << c.name << ',' << c.ow << ',' << c.oh << ',' << m0 << ',' << m1 << ','
                << Percentile(samples[0], 0.95) << ',' << Percentile(samples[1], 0.95) << ','
                << (1 - m1 / m0) * 100 << ',' << sum / count << ',' << max_rgb << ',' << max_a
                << ',' << (sq == 0 ? INFINITY : 10 * std::log10(255. * 255. * count / sq)) << '\n';
        Save(out / (std::string(c.name) + "-old.ppm"), p, c.ow, c.oh);
        Save(out / (std::string(c.name) + "-new.ppm"), n, c.ow, c.oh);
        std::cout << c.name << ": " << m0 << " -> " << m1 << " ms; max RGB difference " << max_rgb
                  << '\n';
        glDeleteQueries(2, q);
        const GLuint textures[] = {src, a, b, old, candidate};
        glDeleteTextures(5, textures);
        if (glGetError() != GL_NO_ERROR)
            throw std::runtime_error("OpenGL validation error");
    }
    for (int i = 0; i <= 100; ++i)
        if (FsrSharpness::ToDisplay(FsrSharpness::ToStored(i)) != i)
            throw std::runtime_error("Sharpness round trip");
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
