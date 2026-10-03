#pragma once

void NxEmuMacOSApplyWindowStyle(const void * nativeWindow);
void * NxEmuMacOSCreateRenderView(const void * nativeWindow);
void NxEmuMacOSDestroyRenderView(void * renderView);
void * NxEmuMacOSGetRenderSurface(void * renderView);
void NxEmuMacOSLayoutRenderView(void * renderView, int x, int y, int width, int height);
void NxEmuMacOSSetRenderViewVisible(void * renderView, bool visible);
float NxEmuMacOSWindowScale(const void * nativeWindow);
