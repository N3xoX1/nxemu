# NxEmu

NxEmu is a free, open-source Nintendo Switch emulator for Windows, written in C++.

## Installation

The latest stable releases are available [here](https://www.nxemu.com/downloads.html).

## Building

```bash
git clone --recursive https://github.com/N3xoX1/nxemu.git
```

The build uses CMake on every platform. On Windows, with Visual Studio 2022 and the Vulkan SDK installed:

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64-release
```

The first command writes `build/windows-x64/nxemu.sln`; Visual Studio can also open the repository folder directly.

Build scripts for macOS, Windows (CMake/Ninja), Linux and Android, prerequisites
and LTO/cache options are documented in [Building NxEmu](docs/building.md).

## Contributing

Contributions are welcome via pull requests.

## Links

- [Website](https://www.nxemu.com)
- [About](https://www.nxemu.com/about.html)
- [Discord](https://discord.gg/hEa4hNyFWU)

## License

NxEmu is licensed under the [GNU General Public License v3.0](./license.txt).
