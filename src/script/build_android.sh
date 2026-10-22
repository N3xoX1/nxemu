#!/bin/sh
# Usage: build_android.sh [Release|Debug] [build-directory] [Gradle options...]
set -eu

configuration="${1:-Release}"
build_directory="${2:-build/android}"
jobs="${NXEMU_BUILD_JOBS:-2}"
if [ "$#" -gt 0 ]; then shift; fi
if [ "$#" -gt 0 ]; then shift; fi
case "$configuration" in
    Release|release) task=assembleRelease ;;
    Debug|debug) task=assembleDebug ;;
    *) echo "Unsupported Android build configuration: $configuration" >&2; exit 1 ;;
esac
case "$jobs" in
    ''|0|*[!0-9]*) echo "NXEMU_BUILD_JOBS must be a positive integer" >&2; exit 1 ;;
esac
NXEMU_BUILD_JOBS="$jobs"
export NXEMU_BUILD_JOBS

cd "$(dirname "$0")/../.."
repo="$(pwd)"
case "$build_directory" in
    /*) ;;
    *) build_directory="$repo/$build_directory" ;;
esac
if [ -z "${JAVA_HOME:-}" ] && [ "$(uname -s)" = Darwin ]; then
    JAVA_HOME="$(/usr/libexec/java_home -v 17)"
    export JAVA_HOME
fi
cd src/android
sh ./gradlew ":app:$task" --build-cache --parallel --max-workers="$jobs" \
    "-PnxemuBuildDirectory=$build_directory" "$@"
