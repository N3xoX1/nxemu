#import <AppKit/AppKit.h>

#include "native_window.h"
#include "system_theme.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <unordered_map>
#include <vector>

namespace
{
struct NativeWindowState
{
    NSWindow * window = nil;
    NSRect windowedFrame{};
    bool transitioning = false;
    bool fullscreen = false;
    bool changed = false;
    std::vector<id> observers;
};

// Notifications and the frontend loop both run on the main thread. The
// notification center owns the observer tokens until explicit removal.
std::unordered_map<SDL_Window *, NativeWindowState> windows;
} // namespace

int ApplyNativeWindowStyle(SDL_Window * window)
{
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (window == nullptr || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_COCOA)
    {
        return 0;
    }
    NSWindow * nativeWindow = info.info.cocoa.window;
    nativeWindow.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    nativeWindow.backgroundColor = NSColor.blackColor;
    nativeWindow.tabbingMode = NSWindowTabbingModeDisallowed;
    const int titleBarHeight = static_cast<int>(NSHeight(nativeWindow.frame) - NSHeight(nativeWindow.contentLayoutRect));
    nativeWindow.titlebarAppearsTransparent = YES;
    nativeWindow.titleVisibility = NSWindowTitleHidden;
    const NSRect frame = nativeWindow.frame;
    nativeWindow.styleMask |= NSWindowStyleMaskFullSizeContentView;
    [nativeWindow setFrame:frame display:YES];

    // SDL 2 does not update its fullscreen flags for the native green button.
    // Let Cocoa own every transition, including keyboard/menu requests. Trying
    // to synchronise SDL's flags with SDL_SetWindowFullscreen can start a second
    // transition while SDL is handling the first one's notifications.
    NativeWindowState & state = windows[window];
    state.window = nativeWindow;
    state.windowedFrame = nativeWindow.frame;
    NativeWindowState * pending = &state;
    NSNotificationCenter * center = NSNotificationCenter.defaultCenter;
    state.observers.push_back([center addObserverForName:NSWindowWillEnterFullScreenNotification object:nativeWindow queue:nil usingBlock:^(NSNotification *) {
        pending->fullscreen = true;
        pending->transitioning = true;
    }]);
    state.observers.push_back([center addObserverForName:NSWindowWillExitFullScreenNotification object:nativeWindow queue:nil usingBlock:^(NSNotification *) {
        pending->fullscreen = false;
        pending->transitioning = true;
    }]);
    for (NSNotificationName name : {NSWindowDidEnterFullScreenNotification, NSWindowDidExitFullScreenNotification})
    {
        state.observers.push_back([center addObserverForName:name object:nativeWindow queue:nil usingBlock:^(NSNotification *) {
            pending->transitioning = false;
            pending->changed = true;
        }]);
    }

    if (@available(macOS 14.0, *))
    {
        [NSApp activate];
    }
    else
    {
        [NSApp activateIgnoringOtherApps:YES];
    }
    [nativeWindow makeKeyAndOrderFront:nil];
    return titleBarHeight;
}

bool SyncNativeWindowState(SDL_Window * window)
{
    const auto it = windows.find(window);
    if (it == windows.end() || it->second.transitioning)
    {
        return false;
    }
    NativeWindowState & state = it->second;
    NSWindow * nativeWindow = state.window;
    if (!state.changed)
    {
        if (!state.fullscreen)
        {
            // Capture before SDL removes FullSizeContentView in WillEnter.
            state.windowedFrame = nativeWindow.frame;
        }
        return false;
    }
    state.changed = false;
    const bool fullscreen = (nativeWindow.styleMask & NSWindowStyleMaskFullScreen) != 0;
    state.fullscreen = fullscreen;
    // Keep the native menu's exit control available. SDL hides it on entry.
    [NSMenu setMenuBarVisible:YES];
    if (!fullscreen)
    {
        // SDL removes FullSizeContentView during the transition. Restore the
        // original frame as well: reusing the post-transition frame loses the
        // title-bar height and can grow the window on subsequent toggles.
        nativeWindow.styleMask |= NSWindowStyleMaskFullSizeContentView;
        [nativeWindow setFrame:state.windowedFrame display:YES];
    }
    return true;
}

bool NativeWindowIsFullscreen(SDL_Window * window)
{
    const auto it = windows.find(window);
    return it != windows.end() && it->second.fullscreen;
}

void SetNativeWindowFullscreen(SDL_Window * window, bool fullscreen)
{
    const auto it = windows.find(window);
    if (it == windows.end() || it->second.transitioning || it->second.fullscreen == fullscreen)
    {
        return;
    }
    [it->second.window toggleFullScreen:nil];
}

void PerformNativeTitleBarDoubleClick(SDL_Window * window)
{
    const auto it = windows.find(window);
    if (it == windows.end() || it->second.transitioning || it->second.fullscreen)
    {
        return;
    }
    // Absent until the user changes it; zooming is the system's default.
    NSString * action = [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"None"])
    {
        return;
    }
    if ([action isEqualToString:@"Minimize"])
    {
        [it->second.window performMiniaturize:nil];
    }
    else
    {
        [it->second.window performZoom:nil];
    }
}

bool SystemPrefersDarkTheme()
{
    NSString * match = [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return match == nil || [match isEqualToString:NSAppearanceNameDarkAqua];
}

void SetNativeWindowTheme(SDL_Window * window, const char * choice)
{
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (window == nullptr || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_COCOA)
    {
        return;
    }
    NSString * mode = [NSString stringWithUTF8String:choice];
    NSAppearance * appearance = nil;
    if ([mode isEqualToString:@"light"])
    {
        appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    }
    else if ([mode isEqualToString:@"dark"])
    {
        appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    }
    info.info.cocoa.window.appearance = appearance;
}

void ReleaseNativeWindowStyle(SDL_Window * window)
{
    const auto it = windows.find(window);
    if (it == windows.end())
    {
        return;
    }
    for (id observer : it->second.observers)
    {
        [NSNotificationCenter.defaultCenter removeObserver:observer];
    }
    windows.erase(it);
}
