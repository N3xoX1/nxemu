#pragma once
struct ANativeWindow { int releases = 0; };
inline void ANativeWindow_release(ANativeWindow* window) { ++window->releases; }
