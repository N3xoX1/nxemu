#pragma once

struct SDL_Window;

// Applies the conventions of the platform's window system that SDL does not
// cover; call once, after the window is created. Only macOS has any: there the
// document extends under the title bar, which keeps the system's buttons.
// Returns the height of that bar in window coordinates, 0 where there is none.
#if defined(__APPLE__)
int ApplyNativeWindowStyle(SDL_Window * window);
// Consume native fullscreen notifications from the main loop, outside SDL's
// event pump. Returns true when fullscreen state or the frame was refreshed.
bool SyncNativeWindowState(SDL_Window * window);
bool NativeWindowIsFullscreen(SDL_Window * window);
void SetNativeWindowFullscreen(SDL_Window * window, bool fullscreen);
// Does what the user chose, in the system's settings, for a double click on a
// window's title bar.
void PerformNativeTitleBarDoubleClick(SDL_Window * window);
void ReleaseNativeWindowStyle(SDL_Window * window);
void SetNativeWindowTheme(SDL_Window * window, const char * choice);
#else
inline int ApplyNativeWindowStyle(SDL_Window * /*window*/)
{
    return 0;
}
inline bool SyncNativeWindowState(SDL_Window * /*window*/) { return false; }
inline void ReleaseNativeWindowStyle(SDL_Window * /*window*/) {}
inline void SetNativeWindowTheme(SDL_Window * /*window*/, const char * /*choice*/) {}
#endif
