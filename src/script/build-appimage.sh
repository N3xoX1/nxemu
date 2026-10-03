#!/usr/bin/env bash
# Build NxEmu and pack it as a single AppImage.
# Usage: src/script/build-appimage.sh
# Output: build/appimage/NxEmu-<arch>.AppImage

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_type="${CMAKE_BUILD_TYPE:-Release}"
build_dir="${BUILD_DIR:-"$root/build/appimage-build"}"
work_dir="${APPIMAGE_WORK:-"$root/build/appimage"}"
appdir="$work_dir/AppDir"
tools="$work_dir/tools"
meta="$work_dir/meta"

case "$(uname -m)" in
    x86_64)
        arch="x86_64"
        platform="x64"
        sciter_lib="$root/external/sciterui/src/3rd_party/sciter-js-sdk-main/bin/linux/x64/libsciter.so"
        ;;
    aarch64 | arm64)
        arch="aarch64"
        platform="arm64"
        sciter_lib="$root/external/sciterui/src/3rd_party/sciter-js-sdk-main/bin/linux/arm64/libsciter.so"
        ;;
    *)
        echo "Unsupported machine: $(uname -m)" >&2
        exit 1
        ;;
esac

out="$work_dir/NxEmu-${arch}.AppImage"

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required tool: $1" >&2
        exit 1
    fi
}

need cmake
need ninja
need g++
need pkg-config
need curl
need python3

if ! pkg-config --exists x11 wayland-client dbus-1; then
    echo "Missing build packages: x11, wayland-client, and dbus-1 development files." >&2
    exit 1
fi

if [[ ! -f "$sciter_lib" ]]; then
    echo "Sciter engine not found: $sciter_lib" >&2
    exit 1
fi

echo "Configuring ${build_type} (${arch})"
cmake -S "$root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type"

echo "Building"
cmake --build "$build_dir" --parallel

exe="$root/bin/${platform}/${build_type}/nxemu"
if [[ ! -x "$exe" ]]; then
    echo "Built executable not found: $exe" >&2
    exit 1
fi

install_module() {
    local dir_name="$1"
    local file_name="$2"
    local src="$root/modules/${platform}/${dir_name}/${file_name}"
    local dest="$appdir/usr/bin/modules/${dir_name}"
    if [[ ! -f "$src" ]]; then
        echo "Built module not found: $src" >&2
        exit 1
    fi
    mkdir -p "$dest"
    cp -L "$src" "$dest/${file_name}"
    chmod 755 "$dest/${file_name}"
}

fetch_tool() {
    local url="$1"
    local dest="$2"
    if [[ -x "$dest" ]]; then
        return
    fi
    mkdir -p "$(dirname "$dest")"
    echo "Downloading $(basename "$dest")"
    curl -fL --retry 3 -o "${dest}.partial" "$url"
    mv "${dest}.partial" "$dest"
    chmod +x "$dest"
}

fetch_tool \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${arch}.AppImage" \
    "$tools/linuxdeploy.AppImage"
fetch_tool \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${arch}.AppImage" \
    "$tools/appimagetool.AppImage"

mkdir -p "$meta"
cat >"$meta/nxemu.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=NxEmu
Comment=NxEmu
Exec=nxemu
Icon=nxemu
Categories=Game;
Terminal=false
EOF

python3 - "$meta/nxemu.png" <<'PY'
import struct
import sys
import zlib

def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

width = height = 48
rgb = bytes((0x1B, 0x3A, 0x4B))
raw = b"".join(b"\x00" + rgb * width for _ in range(height))
png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(raw, 9))
    + chunk(b"IEND", b"")
)
with open(sys.argv[1], "wb") as out:
    out.write(png)
PY

rm -rf "$appdir"
mkdir -p "$appdir"

export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH="$arch"

echo "Collecting libraries"
# OpenGL, EGL, and GLES stay on the system so the AppImage uses the host GPU driver.
"$tools/linuxdeploy.AppImage" --appdir "$appdir" \
    --executable "$exe" \
    --desktop-file "$meta/nxemu.desktop" \
    --icon-file "$meta/nxemu.png" \
    --exclude-library 'libGL.so*' \
    --exclude-library 'libGLESv2.so*' \
    --exclude-library 'libEGL.so*' \
    --exclude-library 'libGLdispatch.so*' \
    --exclude-library 'libvulkan.so*'

# Sciter loads libsciter.so from the executable directory (/proc/self/exe).
# Modules load from ./modules/<name>/ next to the executable.
# lang/ is the default UI directory. user/ is omitted so data goes to XDG dirs
# instead of the read-only AppImage mount.
cp -L "$sciter_lib" "$appdir/usr/bin/libsciter.so"
chmod 755 "$appdir/usr/bin/libsciter.so"
install_module loader libnxemu-loader.so
install_module cpu libnxemu-cpu.so
install_module video libnxemu-video.so
install_module operating_system libnxemu-os.so
rm -rf "$appdir/usr/bin/lang" "$appdir/usr/bin/user"
cp -a "$root/lang" "$appdir/usr/bin/lang"

# Deploy dependencies of the files that stay beside the executable, and point
# their runpath at usr/lib. linuxdeploy does not copy those files again.
"$tools/linuxdeploy.AppImage" --appdir "$appdir" \
    --deploy-deps-only "$appdir/usr/bin/libsciter.so" \
    --deploy-deps-only "$appdir/usr/bin/modules/loader/libnxemu-loader.so" \
    --deploy-deps-only "$appdir/usr/bin/modules/cpu/libnxemu-cpu.so" \
    --deploy-deps-only "$appdir/usr/bin/modules/video/libnxemu-video.so" \
    --deploy-deps-only "$appdir/usr/bin/modules/operating_system/libnxemu-os.so" \
    --exclude-library 'libGL.so*' \
    --exclude-library 'libGLESv2.so*' \
    --exclude-library 'libEGL.so*' \
    --exclude-library 'libGLdispatch.so*' \
    --exclude-library 'libvulkan.so*'

echo "Packing $out"
"$tools/appimagetool.AppImage" "$appdir" "$out"
chmod +x "$out"

echo "AppImage ready: $out"
if [[ ! -e /usr/lib/libfuse.so.2 && ! -e /lib/libfuse.so.2 ]]; then
    echo "This machine has no libfuse.so.2. Run it with:"
    echo "  APPIMAGE_EXTRACT_AND_RUN=1 $out"
fi
