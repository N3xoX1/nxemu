"""Isolated Windows unload probe; caller threads outlive all loader sessions."""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import threading
import gc


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--dll', type=Path, required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--cycles', type=int, default=12)
    args = parser.parse_args()
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.SetErrorMode(0x0001 | 0x0002)  # Probe failures must not open a crash dialog.
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    kernel.FreeLibrary.restype = wintypes.BOOL
    kernel.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    kernel.GetModuleHandleW.restype = wintypes.HMODULE
    class Memory(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('faults', wintypes.DWORD),
                    *[(name, ctypes.c_size_t) for name in ('peak', 'rss', 'peak_paged', 'paged',
                       'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile', 'private')]]
    psapi = ctypes.WinDLL('psapi')
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD]
    start, finished = threading.Barrier(5), threading.Barrier(5)
    stop = False
    callback = None
    errors = []
    path = str(args.archive.resolve()).encode('utf-8')

    def handles():
        value = wintypes.DWORD()
        if not kernel.GetProcessHandleCount(kernel.GetCurrentProcess(), ctypes.byref(value)):
            raise ctypes.WinError()
        return value.value

    def worker():
        while True:
            start.wait()
            if stop:
                return
            result = callback(path)
            if result:
                errors.append(result)
            finished.wait()

    threads = [threading.Thread(target=worker) for _ in range(4)]
    for thread in threads:
        thread.start()
    baseline = handles()
    counts = []
    residents = []
    for cycle in range(args.cycles):
        module = ctypes.WinDLL(str(args.dll.resolve()))
        callback = module.NXTestRead
        callback.argtypes = [ctypes.c_char_p]
        callback.restype = ctypes.c_int
        start.wait()
        finished.wait()  # No live sessions/VFS handles/calls remain in the DLL.
        if not kernel.FreeLibrary(module._handle):
            raise ctypes.WinError()
        count = handles()
        retained = bool(kernel.GetModuleHandleW(args.dll.name))
        callback = None
        module = None
        gc.collect()
        memory = Memory()
        memory.cb = ctypes.sizeof(memory)
        if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(memory), memory.cb):
            raise ctypes.WinError()
        counts.append(count)
        residents.append(memory.rss)
        print(json.dumps({'cycle': cycle, 'handles': count, 'module_retained': retained,
                          'rss': memory.rss, 'peak_rss': memory.peak, 'private_bytes': memory.private}), flush=True)
        if retained:
            errors.append('module remained loaded')
    stop = True
    start.wait()
    for thread in threads:
        thread.join()
    print(json.dumps({'baseline_handles': baseline, 'final_handles': handles(), 'errors': errors}), flush=True)
    if errors or max(counts[-3:]) > min(counts[:3]) + 8:
        raise SystemExit('FAIL progressive handles across module unload/reload')
    if len(residents) >= 20 and max(residents[-10:]) > max(residents[5:15]) + 4 * 1024 * 1024:
        raise SystemExit('FAIL progressive RSS across module unload/reload')


if __name__ == '__main__':
    main()
