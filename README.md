# NxEmu

NxEmu is a free, open-source Nintendo Switch emulator for Windows, Linux and macOS, written in C++.

## Installation

The latest stable releases are available [here](https://www.nxemu.com/downloads.html).

## Building

```bash
git clone --recursive https://github.com/N3xoX1/nxemu.git
```

Build the RmlUi frontend with CMake (Visual Studio 2022 build tools and the Vulkan SDK on Windows):

```powershell
./src/script/build_windows.ps1 Release
```

On Linux, use `sh src/script/build_linux.sh Release`.
On macOS with Xcode installed, use `sh src/script/build_macos.sh Release`.
For Android, use `sh src/script/build_android.sh Release` with JDK 17, Android SDK 36 and NDK 30.0.16248370.
Each script accepts a build directory after the configuration; the desktop scripts also accept extra CMake options.
The former frontend, its resource tools and hand-written Visual Studio projects have been removed.

GitHub Actions builds Windows x64, Linux x64, macOS arm64 and Android arm64 on pull requests and pushes to master.
Desktop jobs run the frontend-settings compatibility test; Android runs its Gradle unit tests and checks native libraries in the APK.
The jobs publish desktop archives or an APK and cache compiler outputs (sccache on Windows, ccache elsewhere).
The macOS job also caches the pinned MoltenVK runtime; Linux, macOS and Android retain supported ThinLTO caches.

To run the desktop settings test locally, configure with `-DNXEMU_BUILD_UI_SETTINGS_TESTS=ON`,
build `nxemu_ui_settings_tests`, then run `ctest --test-dir <build-directory> -C Release --output-on-failure`.

## Contributing

Contributions are welcome via pull requests.

## Links

- [Website](https://www.nxemu.com)
- [About](https://www.nxemu.com/about.html)
- [Discord](https://discord.gg/hEa4hNyFWU)

## License

NxEmu is licensed under the [GNU General Public License v3.0](./license.txt).
