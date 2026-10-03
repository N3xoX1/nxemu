#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include "render_window_macos.h"

@interface NxEmuMetalRenderView : NSView
- (void)updateDrawableSize;
@end

@implementation NxEmuMetalRenderView

- (BOOL)wantsUpdateLayer
{
    return YES;
}

- (CALayer *)makeBackingLayer
{
    return [CAMetalLayer layer];
}

- (void)updateDrawableSize
{
    CAMetalLayer * metalLayer = (CAMetalLayer *)self.layer;
    if (metalLayer == nil)
    {
        return;
    }

    const CGFloat scale = self.window != nil ? self.window.backingScaleFactor : 1.0;
    // Retina sizes the presentation surface only. Guest render targets are
    // scaled separately by the video resolution setting; do not multiply that
    // setting by backingScaleFactor or force the layer to 1x.
    metalLayer.contentsScale = scale;
    metalLayer.drawableSize = [self convertRectToBacking:self.bounds].size;
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

- (void)setFrameSize:(NSSize)newSize
{
    [super setFrameSize:newSize];
    [self updateDrawableSize];
}

- (NSView *)hitTest:(NSPoint)point
{
    (void)point;
    return nil;
}

@end

static NSWindow * NxEmuGetSciterWindow(const void * nativeWindowHandle)
{
    return (NSWindow *)const_cast<void *>(nativeWindowHandle);
}

void NxEmuMacOSApplyWindowStyle(const void * nativeWindowHandle)
{
    NSWindow * window = NxEmuGetSciterWindow(nativeWindowHandle);
    if (window == nil)
    {
        return;
    }

    window.titleVisibility = NSWindowTitleVisible;
    window.titlebarAppearsTransparent = NO;
    window.movableByWindowBackground = NO;
    window.hasShadow = YES;
    // A standard main window must have an opaque background, including the
    // status bar. A clear background can retain per-pixel click-through regions.
    window.opaque = YES;
    window.backgroundColor = [NSColor windowBackgroundColor];
    window.tabbingMode = NSWindowTabbingModeDisallowed;

    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

    if (@available(macOS 14.0, *))
    {
        [NSApp activate];
    }
    else
    {
        [NSApp activateIgnoringOtherApps:YES];
    }
    [window makeKeyAndOrderFront:nil];
}

void * NxEmuMacOSCreateRenderView(const void * nativeWindowHandle)
{
    NSWindow * window = NxEmuGetSciterWindow(nativeWindowHandle);
    NSView * parentView = window != nil ? window.contentView : nil;
    if (parentView == nil)
    {
        return nullptr;
    }

    NxEmuMetalRenderView * renderView = [[NxEmuMetalRenderView alloc] initWithFrame:NSZeroRect];
    renderView.wantsLayer = YES;

    CAMetalLayer * metalLayer = (CAMetalLayer *)renderView.layer;
    if (metalLayer == nil)
    {
        [renderView release];
        return nullptr;
    }

    metalLayer.opaque = YES;
    renderView.hidden = YES;
    [parentView addSubview:renderView positioned:NSWindowAbove relativeTo:nil];
    [renderView updateDrawableSize];
    return renderView;
}

void NxEmuMacOSDestroyRenderView(void * renderViewHandle)
{
    NSView * renderView = (NSView *)renderViewHandle;
    if (renderView == nil)
    {
        return;
    }

    [renderView removeFromSuperview];
    [renderView release];
}

void * NxEmuMacOSGetRenderSurface(void * renderViewHandle)
{
    NSView * renderView = (NSView *)renderViewHandle;
    return renderView != nil ? (void *)renderView.layer : nullptr;
}

void NxEmuMacOSLayoutRenderView(void * renderViewHandle, int x, int y, int width, int height)
{
    NxEmuMetalRenderView * renderView = (NxEmuMetalRenderView *)renderViewHandle;
    if (renderView == nil || renderView.superview == nil || width <= 0 || height <= 0)
    {
        return;
    }

    const NSRect parentBounds = renderView.superview.bounds;
    const CGFloat originY = renderView.superview.isFlipped ? y : parentBounds.size.height - y - height;
    renderView.frame = NSMakeRect(x, originY, width, height);
    [renderView updateDrawableSize];
}

void NxEmuMacOSSetRenderViewVisible(void * renderViewHandle, bool visible)
{
    NSView * renderView = (NSView *)renderViewHandle;
    if (renderView != nil)
    {
        renderView.hidden = !visible;
    }
}

float NxEmuMacOSWindowScale(const void * nativeWindowHandle)
{
    NSWindow * window = NxEmuGetSciterWindow(nativeWindowHandle);
    return window != nil ? (float)window.backingScaleFactor : 1.0f;
}
