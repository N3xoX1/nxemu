// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <thread>

int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    for(unsigned cycle=0;cycle<8;++cycle) {
        HMODULE module=LoadLibraryW(argv[1]);
        if(!module) return 3;
        const auto run=reinterpret_cast<int(*)()>(GetProcAddress(module,"RunReadback"));
        if(!run) return 4;
        int result=5;
        std::thread caller{[&]{result=run();}};
        caller.join();
        if(result) return 5;
        if(!FreeLibrary(module)) return 6;
    }
    std::cout<<"PASS eight DLL load, readback, caller join and unload cycles\n";
    return 0;
}
