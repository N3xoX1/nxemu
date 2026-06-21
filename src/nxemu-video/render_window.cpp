#include "render_window.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_common/settings.h"
#include "yuzu_video_core/frontend/graphics_context.h"
#include <string>
#if defined(_WIN32)
#include <Windows.h>
#include <glad/glad.h>
#include <glad/glad_wgl.h>
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
#include <nxemu-core/settings/identifiers.h>
#include <yuzu_common/dynamic_library.h>
#ifdef ARCHITECTURE_arm64
#include <adrenotools/driver.h>
#endif
#endif
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-module-spec/video.h>

extern IModuleSettings * g_settings;

#if defined(_WIN32)

class OpenGLSharedContext : public Core::Frontend::GraphicsContext
{
public:
    explicit OpenGLSharedContext(IRenderWindow & renderWindow) :
        m_renderWindow(renderWindow),
        m_glrc(nullptr),
        m_hdc(nullptr)
    {
        if (!m_openglLoaded && !LoadOpenGL())
        {
            return;
        }

        HWND hWnd = (HWND)renderWindow.RenderSurface();
        if (hWnd == nullptr)
        {
            return;
        }
        m_hdc = GetDC(hWnd);
        if (m_hdc == nullptr)
        {
            return;
        }

        int pfm = GetPixelFormat(m_hdc);
        if (pfm == 0)
        {
            PIXELFORMATDESCRIPTOR pfd = {0};
            pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
            pfd.nVersion = 1;
            pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            pfd.iPixelType = PFD_TYPE_RGBA;
            pfd.iLayerType = PFD_MAIN_PLANE;
            pfd.cColorBits = 32;
            pfd.cDepthBits = 24;
            pfd.cStencilBits = 8;

            int pfAttribs[] = {
                WGL_DRAW_TO_WINDOW_ARB, TRUE,
                WGL_SUPPORT_OPENGL_ARB, TRUE,
                WGL_DOUBLE_BUFFER_ARB, TRUE,
                WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
                WGL_COLOR_BITS_ARB, 32,
                WGL_DEPTH_BITS_ARB, 24,
                WGL_STENCIL_BITS_ARB, 8,
                WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
                0};
            int fmt = 0;
            UINT num = 0;
            BOOL ok = wglChoosePixelFormatARB(m_hdc, pfAttribs, nullptr, 1, &fmt, &num);
            if (!ok || num == 0)
            {
                return;
            }

            DescribePixelFormat(m_hdc, fmt, sizeof(pfd), &pfd);
            if (!SetPixelFormat(m_hdc, fmt, &pfd))
            {
                return;
            }
        }

        if (wglCreateContextAttribsARB)
        {
            int contextAttribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
                WGL_CONTEXT_MINOR_VERSION_ARB, 6,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
                0};

            m_glrc = wglCreateContextAttribsARB(m_hdc, nullptr, contextAttribs);
            if (m_glrc == nullptr)
            {
                contextAttribs[5] = WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
                m_glrc = wglCreateContextAttribsARB(m_hdc, nullptr, contextAttribs);
            }
        }
        if (m_glrc == nullptr)
        {
            m_glrc = wglCreateContext(m_hdc);
        }
        if (m_glrc == nullptr)
        {
            return;
        }
    }
    ~OpenGLSharedContext()
    {
        DoneCurrent();
        if (m_hdc != nullptr)
        {
            HWND hwnd = static_cast<HWND>(m_renderWindow.RenderSurface());
            ReleaseDC(hwnd, m_hdc);
            m_hdc = nullptr;
        }
    }
    void SwapBuffers() override
    {
        ::SwapBuffers(m_hdc);
    }
    void MakeCurrent() override
    {
        wglMakeCurrent(m_hdc, m_glrc);
    }
    void DoneCurrent() override
    {
        wglMakeCurrent(nullptr, nullptr);
    }

private:
    static LRESULT CALLBACK DummyWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    bool LoadOpenGL()
    {
        HWND hwnd = CreateDummyWindow(nullptr);
        if (hwnd == nullptr)
        {
            return false;
        }
        HDC hdc = GetDC(hwnd);
        if (hdc == nullptr)
        {
            DestroyWindow(hwnd);
            return false;
        }

        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        int pf = ChoosePixelFormat(hdc, &pfd);
        if (pf == 0)
        {
            ReleaseDC(hwnd, hdc);
            DestroyWindow(hwnd);
            return false;
        }
        if (!SetPixelFormat(hdc, pf, &pfd))
        {
            ReleaseDC(hwnd, hdc);
            DestroyWindow(hwnd);
            return false;
        }

        HGLRC dummyCtx = wglCreateContext(hdc);
        if (dummyCtx == nullptr)
        {
            ReleaseDC(hwnd, hdc);
            DestroyWindow(hwnd);
            return false;
        }
        if (!wglMakeCurrent(hdc, dummyCtx))
        {
            wglDeleteContext(dummyCtx);
            ReleaseDC(hwnd, hdc);
            DestroyWindow(hwnd);
            return false;
        }

        if (gladLoadGL() == 0)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(dummyCtx);
            ReleaseDC(hwnd, hdc);
            DestroyWindow(hwnd);
            return false;
        }
        gladLoadWGL(hdc);
        m_openglLoaded = true;
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(dummyCtx);
        ReleaseDC(hwnd, hdc);
        DestroyWindow(hwnd);
        return true;
    }

    HWND CreateDummyWindow(HINSTANCE hInstance)
    {
        WNDCLASSA wc = {};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DummyWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = "DummyGLWindowClass";

        RegisterClassA(&wc);

        HWND hwnd = CreateWindowA(wc.lpszClassName, "DummyGL", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, hInstance, nullptr);
        return hwnd;
    }

    static bool m_openglLoaded;
    IRenderWindow & m_renderWindow;
    HGLRC m_glrc;
    HDC m_hdc;
};

bool OpenGLSharedContext::m_openglLoaded = false;

#endif // defined(_WIN32)

#if defined(__ANDROID__)

class GraphicsContext_Android final : public Core::Frontend::GraphicsContext
{
public:
    explicit GraphicsContext_Android(std::shared_ptr<Common::DynamicLibrary> driver_library) :
        m_driver_library{std::move(driver_library)}
    {
    }

    std::shared_ptr<Common::DynamicLibrary> GetDriverLibrary() override
    {
        return m_driver_library;
    }

private:
    std::shared_ptr<Common::DynamicLibrary> m_driver_library;
};

std::shared_ptr<Common::DynamicLibrary> LoadAndroidVulkanDriverFromSettings()
{
    if (g_settings == nullptr)
    {
        return {};
    }

    const std::string hook_lib_dir = g_settings->GetString(NXCoreSetting::GpuHookLibDir);
    if (hook_lib_dir.empty())
    {
        return {};
    }

    const std::string custom_driver_dir = g_settings->GetString(NXCoreSetting::GpuCustomDriverDir);
    const std::string custom_driver_name = g_settings->GetString(NXCoreSetting::GpuCustomDriverName);
    const std::string file_redirect_dir = g_settings->GetString(NXCoreSetting::GpuFileRedirectDir);

#ifdef ARCHITECTURE_arm64
    void * handle{};
    const char * file_redirect_dir_ptr{};
    int feature_flags{};

    if (!file_redirect_dir.empty())
    {
        feature_flags |= ADRENOTOOLS_DRIVER_FILE_REDIRECT;
        file_redirect_dir_ptr = file_redirect_dir.c_str();
    }

    if (!custom_driver_name.empty())
    {
        handle = adrenotools_open_libvulkan(RTLD_NOW, feature_flags | ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hook_lib_dir.c_str(), custom_driver_dir.c_str(), custom_driver_name.c_str(), file_redirect_dir_ptr, nullptr);
    }

    if (handle == nullptr)
    {
        handle = adrenotools_open_libvulkan(RTLD_NOW, feature_flags, nullptr, hook_lib_dir.c_str(), nullptr, nullptr, file_redirect_dir_ptr, nullptr);
    }

    if (handle != nullptr)
    {
        return std::make_shared<Common::DynamicLibrary>(handle);
    }
#endif

    auto fallback = std::make_shared<Common::DynamicLibrary>();
    if (fallback->Open("libvulkan.so"))
    {
        return fallback;
    }
    return {};
}

std::shared_ptr<Common::DynamicLibrary> & AndroidVulkanDriverCache()
{
    static std::shared_ptr<Common::DynamicLibrary> library;
    return library;
}

#endif // defined(__ANDROID__)

class DummyContext : public Core::Frontend::GraphicsContext
{
};

RenderWindow::RenderWindow(IRenderWindow & renderWindow) :
    m_renderWindow(renderWindow),
    m_firstFrame(false)
{
#if defined(_WIN32)
    window_info.type = Core::Frontend::WindowSystemType::Windows;
#elif defined(__ANDROID__)
    window_info.type = Core::Frontend::WindowSystemType::Android;
#else
    window_info.type = Core::Frontend::WindowSystemType::Headless;
#endif
    window_info.render_surface = renderWindow.RenderSurface();
    NotifyClientAreaSizeChanged({0, 0});
    UpdateCurrentFramebufferLayout(640, 480);
}

void RenderWindow::OnFrameDisplayed()
{
    if (!m_firstFrame)
    {
        m_firstFrame = true;
        g_settings->SetBool(NXCoreSetting::DisplayedFrames, true);
    }
}

std::unique_ptr<Core::Frontend::GraphicsContext> RenderWindow::CreateSharedContext() const
{
#if defined(_WIN32)
    if (videoSettings.renderer_backend == RendererBackend::OpenGL)
    {
        return std::make_unique<OpenGLSharedContext>(m_renderWindow);
    }
#endif
#if defined(__ANDROID__)
    if (!AndroidVulkanDriverCache())
    {
        AndroidVulkanDriverCache() = LoadAndroidVulkanDriverFromSettings();
    }
    if (const auto & library = AndroidVulkanDriverCache(); library && library->IsOpen())
    {
        return std::make_unique<GraphicsContext_Android>(library);
    }
#endif
    return std::make_unique<DummyContext>();
}

bool RenderWindow::IsShown() const
{
    return true;
}