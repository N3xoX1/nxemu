#pragma once

struct SDL_Window;

#if defined(__APPLE__)
int ApplyNativeWindowStyle(SDL_Window * window);
bool SyncNativeWindowState(SDL_Window * window);
bool NativeWindowIsFullscreen(SDL_Window * window);
void SetNativeWindowFullscreen(SDL_Window * window, bool fullscreen);
void PerformNativeTitleBarDoubleClick(SDL_Window * window);
void ReleaseNativeWindowStyle(SDL_Window * window);
#else
inline int ApplyNativeWindowStyle(SDL_Window * /*window*/)
{
    return 0;
}
inline bool SyncNativeWindowState(SDL_Window * /*window*/) { return false; }
inline void ReleaseNativeWindowStyle(SDL_Window * /*window*/) {}
#endif
