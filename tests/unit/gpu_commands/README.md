# GPU command regression tests

The fixture executes the production DMA pusher and Maxwell register methods extracted by extract.py. It compares register, dirty, shadow and executable-method state against a frozen baseline, including shadow/replay modes and command boundaries. Backend and macro rendering dependencies are simulated; this is not a gameplay rendering test.

Build test_gpu_commands with NXEMU_BUILD_TESTS=ON, then run ctest -C Release -R test_gpu_commands --output-on-failure. Passing an extra argument to the executable runs synthetic command benchmarks. Benchmark improvements do not establish a game FPS improvement.
