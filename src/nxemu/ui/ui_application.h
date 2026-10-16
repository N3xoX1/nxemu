#pragma once

// Runs the frontend until the user quits and returns the process exit code.
// `probeGraphics` is given the path of the executable and returns false when
// the system's Vulkan driver cannot be used; it runs once the settings are
// loaded, unless they turn the check off.
int RunUiApplication(int argc, char ** argv, bool (*probeGraphics)(const char * executable));
