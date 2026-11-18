# Texture region regression tests

The fixture extracts the production CPU/GPU page enumeration and texture-region queries. It checks empty queries, page boundaries, aliases, duplicate mappings, callback reentrancy and randomized queries. Frozen references also demonstrate why the single-page shortcut was not adopted.

test_texture_descriptors extracts the descriptor table tracking used by draw descriptor reuse. It checks that the tables are tracked and registered in the flush cache filter, and that DMA writes to them, through an alias too, change the descriptor epoch while writes elsewhere don't. It also checks which GPU mapping changes move the tables, and that tables that can't be tracked turn reuse off.

Build test_texture_regions and test_texture_descriptors with NXEMU_BUILD_TESTS=ON, then run ctest -C Release -R test_texture_ --output-on-failure. An extra executable argument runs synthetic benchmarks. No GPU backend or game rendering is exercised.
