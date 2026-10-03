#include "render_window_linux.h"
#include <nxemu-module-spec/base.h>
#include <sciter-x-api.h>
#include <sciter_wayland_native.h>
#include <wayland-client.h>
#include "viewporter-client-protocol.h"
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
struct RenderView
{
    LinuxRenderSurface surface{};
    const void * sciterWindow = nullptr;
    wl_surface * parent = nullptr;
    wl_compositor * compositor = nullptr;
    wl_subcompositor * subcompositor = nullptr;
    wl_subsurface * subsurface = nullptr;
    wp_viewporter * viewporter = nullptr;
    wp_viewport * viewport = nullptr;
    int scale = 1;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool positioned = false;
    bool visible = false;
};

void RegistryGlobal(void * data, wl_registry * registry, uint32_t name,
                    const char * interface, uint32_t version)
{
    auto & view = *static_cast<RenderView *>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0 &&
        view.compositor == nullptr && version >= 3)
        view.compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 3));
    else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0 &&
             view.subcompositor == nullptr && version >= 1)
        view.subcompositor = static_cast<wl_subcompositor *>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    else if (std::strcmp(interface, wp_viewporter_interface.name) == 0 &&
             view.viewporter == nullptr && version >= 1)
        view.viewporter = static_cast<wp_viewporter *>(
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
}
void RegistryRemove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener registryListener{RegistryGlobal, RegistryRemove};
}

float NxEmuLinuxWindowScale(const void * window)
{
    if (!window) return 1.0f;
    UINT x = 96, y = 96;
    SciterGetPPI(const_cast<void *>(window), &x, &y);
    // Keep the fractional ratio for framebuffer sizing. Wayland's buffer
    // scale remains integral; wp_viewporter maps it to the logical rectangle.
    return std::clamp(static_cast<float>(x) / 96.0f, 1.0f, 4.0f);
}

void NxEmuLinuxDestroyRenderView(void * handle)
{
    std::unique_ptr<RenderView> view(static_cast<RenderView *>(handle));
    if (!view) return;
    // Caller must have joined Vulkan presentation and destroyed VkSurfaceKHR.
    if (view->viewport) wp_viewport_destroy(view->viewport);
    if (view->subsurface) wl_subsurface_destroy(view->subsurface);
    if (view->surface.window) wl_surface_destroy(static_cast<wl_surface *>(view->surface.window));
    if (view->subcompositor) wl_subcompositor_destroy(view->subcompositor);
    if (view->compositor) wl_compositor_destroy(view->compositor);
    if (view->viewporter) wp_viewporter_destroy(view->viewporter);
    // Sciter retains ownership of the display and parent surface.
}

void * NxEmuLinuxCreateRenderView(const void * parent)
{
    SciterWaylandNative native{};
    if (!SciterUIGetWaylandNative(parent, native))
    {
        std::fprintf(stderr, "nxemu: Sciter Wayland native ABI not configured or not available\n");
        return nullptr;
    }
    auto* parentSurface = static_cast<wl_surface*>(native.surface);
    auto* display = static_cast<wl_display*>(native.display);

    std::unique_ptr<RenderView, void(*)(RenderView *)> view(
        new RenderView, [](RenderView * p) { NxEmuLinuxDestroyRenderView(p); });
    view->parent = parentSurface;
    view->sciterWindow = parent;
    view->surface.display = display;
    view->scale = static_cast<int>(std::ceil(NxEmuLinuxWindowScale(parent)));
    wl_event_queue * queue = wl_display_create_queue(display);
    if (!queue)
        return nullptr;
    wl_registry * registry = wl_display_get_registry(display);
    if (!registry)
    {
        wl_event_queue_destroy(queue);
        return nullptr;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(registry), queue);
    const bool listener_ok = wl_registry_add_listener(registry, &registryListener, view.get()) == 0;
    const bool roundtrip_ok = listener_ok && wl_display_roundtrip_queue(display, queue) >= 0;
    wl_registry_destroy(registry);
    if (view->compositor)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(view->compositor), nullptr);
    if (view->subcompositor)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(view->subcompositor), nullptr);
    if (view->viewporter)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(view->viewporter), nullptr);
    wl_event_queue_destroy(queue);
    if (!roundtrip_ok || !view->compositor || !view->subcompositor || !view->viewporter)
    {
        std::fprintf(stderr, "nxemu: Wayland compositor/subcompositor/viewporter unavailable\n");
        return nullptr;
    }
    wl_surface * surface = wl_compositor_create_surface(view->compositor);
    if (!surface)
        return nullptr;
    view->surface.window = surface;
    view->viewport = wp_viewporter_get_viewport(view->viewporter, surface);
    if (!view->viewport)
        return nullptr;
    view->subsurface = wl_subcompositor_get_subsurface(view->subcompositor, surface, parentSurface);
    if (!view->subsurface)
        return nullptr;
    wl_subsurface_set_desync(view->subsurface);
    wl_region * empty = wl_compositor_create_region(view->compositor);
    if (!empty)
        return nullptr;
    wl_surface_set_input_region(surface, empty);
    wl_region_destroy(empty);
    wl_surface_set_buffer_scale(surface, view->scale);
    wl_surface_commit(surface);
    if (wl_display_flush(display) < 0 && errno != EAGAIN)
        return nullptr;
    return view.release();
}

void NxEmuLinuxLayoutRenderView(void * handle, int x, int y, int width, int height)
{
    auto * view = static_cast<RenderView *>(handle);
    if (!view || width <= 0 || height <= 0) return;
    const int scale = static_cast<int>(std::ceil(NxEmuLinuxWindowScale(view->sciterWindow)));
    const bool scaleChanged = scale != view->scale;
    const bool moved = !view->positioned || view->x != x || view->y != y;
    const bool resized = view->width != width || view->height != height;
    if (resized)
    {
        // Wayland does not clip subsurfaces to their parent. Bound even an old
        // Vulkan buffer to the current content area while the swapchain resizes.
        wp_viewport_set_destination(view->viewport, width, height);
        view->width = width;
        view->height = height;
    }
    if (moved)
    {
        wl_subsurface_set_position(view->subsurface, x, y);
        view->x = x;
        view->y = y;
        view->positioned = true;
        // Parent surface belongs to Sciter: do this only when position changes.
        wl_surface_commit(view->parent);
    }
    if (scaleChanged)
    {
        view->scale = scale;
        wl_surface_set_buffer_scale(static_cast<wl_surface *>(view->surface.window), scale);
    }
    if (resized || scaleChanged)
        wl_surface_commit(static_cast<wl_surface *>(view->surface.window));
    if (moved || resized || scaleChanged)
        wl_display_flush(static_cast<wl_display *>(view->surface.display));
}

void NxEmuLinuxSetRenderViewVisible(void * handle, bool visible)
{
    auto * view = static_cast<RenderView *>(handle);
    if (!view || view->visible == visible) return;
    view->visible = visible;
    if (!visible)
    {
        auto * surface = static_cast<wl_surface *>(view->surface.window);
        wl_surface_attach(surface, nullptr, 0, 0);
        wl_surface_commit(surface);
        wl_display_flush(static_cast<wl_display *>(view->surface.display));
    }
    // Vulkan presentation reattaches its own buffer on resume.
}

void * NxEmuLinuxGetRenderSurface(void * handle)
{
    auto * view = static_cast<RenderView *>(handle);
    return view ? &view->surface : nullptr;
}
