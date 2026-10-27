#pragma once
#include <chrono>

int RunUiApplication(int argc, char ** argv, std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now());
