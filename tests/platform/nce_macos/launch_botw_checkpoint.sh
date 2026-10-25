#!/bin/bash
# Reproduce the BOTW checkpoint measured at 106.7 sustained FPS on the plain save.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
APP="${1:-$REPO/bin/arm64/Release/NxEmu.app}"
RUN_DIR="${NXEMU_BOTW_RUN_DIR:-$REPO/build/nce_address_space/run}"
if [[ ! -x "$APP/Contents/MacOS/NxEmu" || ! -d "$RUN_DIR" ]]; then
    echo "Built app and configured BOTW run directory required." >&2
    exit 1
fi
cd "$RUN_DIR"
exec env -u NXEMU_NCE_ALL_READ_BASES -u NXEMU_NCE_WRITE_TARGET_ONLY \
    -u NXEMU_NCE_DEMAND_READBACK -u NXEMU_NCE_PRECISE_READ_TRACKING \
    -u NXEMU_NCE_BATCH_READBACK -u NXEMU_NCE_RELEASE_READBACK \
    NXEMU_NCE_READ_THUNKS=1 NXEMU_NCE_FLOAT_READ_THUNKS=1 \
    NXEMU_NCE_STORE_THUNKS=1 NXEMU_NCE_OBSERVED_GPU_COPIES=1 \
    NXEMU_NCE_OBSERVED_READBACK=1 NXEMU_NCE_PAGE_READ_FASTPATH=1 \
    NXEMU_NCE_READBACK_PROFILE=1 NXEMU_NCE_FAULT_INSTRUCTIONS=1 \
    NXEMU_NCE_STALL_SUMMARY=1 \
    NXEMU_NCE_READ_INSTRUCTIONS=bd4f5183,bd4f5583,bd4f5d84,bd400901,bd400100,bd428001,bd400383,bd400342,ad000901,bc2b6991,d50b7e2a,fd000166,ad000420,f9000a74,39028001,bd001a62,f9001d4c,bd0000c7,f9001a7f,b9000288,f9005a7f,f9002ae8,f9004b58,f9000788,f9000109,390aa41f \
    "$APP/Contents/MacOS/NxEmu"
