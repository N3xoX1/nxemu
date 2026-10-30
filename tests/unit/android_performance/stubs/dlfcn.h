#pragma once
#define RTLD_DEFAULT nullptr
void* TestAndroidSymbol(void*, const char*);
#define dlsym TestAndroidSymbol
