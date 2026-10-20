# Host1x command routing regression

Build separately without the emulator's external dependencies:

```sh
cmake -S tests/host1x -B build/tests/host1x
cmake --build build/tests/host1x --config Release
ctest --test-dir build/tests/host1x -C Release --output-on-failure
```

The executable compiles the production CDMA parser with recording devices.
It checks NVDEC SetClass masks, sparse masks, masks split across submissions,
Host1x sparse masks, and unaffected NVDEC incrementing/nonincrementing and VIC
commands. Four cases fail before the register-offset correction; all seven
pass after it. Decoder behavior, real fences and full playback are outside
this isolated test's scope.
