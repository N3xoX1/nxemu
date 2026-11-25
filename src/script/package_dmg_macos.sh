#!/bin/sh
set -eu

dmg_file="${1:-NxEmu-macos-arm64.dmg}"
configuration="${2:-Release}"

cd "$(dirname "$0")/../.."

app="bin/arm64/$configuration/NxEmu.app"
codesign --verify --deep --strict "$app"

staging_directory="$(mktemp -d "${TMPDIR:-/tmp}/nxemu-dmg.XXXXXX")"
trap 'rm -rf "$staging_directory"' EXIT
trap 'exit 1' HUP INT TERM

ditto "$app" "$staging_directory/NxEmu.app"
ln -s /Applications "$staging_directory/Applications"

mkdir -p package
hdiutil create -volname NxEmu -srcfolder "$staging_directory" \
    -fs HFS+ -format UDZO -ov "package/$dmg_file"
