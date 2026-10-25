#!/bin/bash
# Builds and runs the NCE macOS validation suite in a build tree configured with
# NXEMU_BUILD_TESTS=ON, and stores the results in
# tests/platform/nce_macos/results/<date>_<machine>.{txt,tsv}.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
BUILD="${NXEMU_BUILD_DIR:-$REPO/build}"
OUT="$BUILD/tests/platform"

cmake --build "$BUILD" --target nce_macos_validate || exit 1

machine="$(sysctl -n machdep.cpu.brand_string | tr ' ' '-')_macOS-$(sw_vers -productVersion)"
stamp="$(date +%Y%m%d-%H%M%S)"
mkdir -p "$HERE/results"
txt="$HERE/results/${stamp}_${machine}.txt"
tsv="$HERE/results/${stamp}_${machine}.tsv"

{
    echo "Machine : $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) cœurs, macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "Commit  : $(git -C "$REPO" rev-parse --short HEAD) $(git -C "$REPO" status --porcelain src/nxemu-cpu/nce | wc -l | tr -d ' ') fichier(s) NCE modifié(s)"
    echo
} > "$txt"

printf 'étape\tid\trésultat\tclasse\ttitre\tdétail\tchangement\n' > "$tsv"
NCEV_TSV="$tsv" "$OUT/nce_macos_validate" "${1:-all}" 2>&1 | grep -v -e '^\[log ' -e '^unreachable$' | tee -a "$txt"
status=${PIPESTATUS[0]}
echo
echo "Résultats : $txt"
exit "$status"
