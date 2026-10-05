#pragma once

void * NxEmuLinuxCreateRenderView(const void * parent);
void NxEmuLinuxDestroyRenderView(void * view);
void NxEmuLinuxLayoutRenderView(void * view, int x, int y, int width, int height);
void NxEmuLinuxSetRenderViewVisible(void * view, bool visible);
void * NxEmuLinuxGetRenderSurface(void * view);
float NxEmuLinuxWindowScale(const void * window);
