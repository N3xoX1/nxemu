# Texture region regression tests

The fixture extracts the production CPU/GPU page enumeration and texture-region queries. It checks empty queries, page boundaries, aliases, duplicate mappings, callback reentrancy and randomized queries. Frozen references also demonstrate why the single-page shortcut was not adopted.

Build test_texture_regions with NXEMU_BUILD_TESTS=ON, then run ctest -C Release -R test_texture_regions --output-on-failure. An extra executable argument runs synthetic benchmarks. No GPU backend or game rendering is exercised.
