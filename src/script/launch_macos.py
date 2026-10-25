#!/usr/bin/env python3
"""Launch NxEmu with explicit diagnostics and no inherited malloc debug options.

MallocScribble reproduced corruption in macOS 26.6.2's legacy allocator,
independently of NxEmu. Use heap checks separately for targeted investigations.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess


def build_environment(source, heap_check=False, metal_validation=False):
    env = {
        key: value for key, value in source.items()
        if not key.startswith(("Malloc", "NXEMU_DIAG_")) and key != "MTL_DEBUG_LAYER"
    }
    if heap_check:
        env.update(
            MallocCheckHeapStart="100000",
            MallocCheckHeapEach="100000",
            MallocCheckHeapAbort="1",
            MallocErrorAbort="1",
        )
    if metal_validation:
        env["MTL_DEBUG_LAYER"] = "1"
    return env


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--app", type=Path,
        default=root / "bin/arm64/Release/NxEmu.app",
        help="Application bundle (default: this checkout's ARM64 Release build)",
    )
    parser.add_argument("--log", type=Path, help="New output log; existing files are preserved")
    parser.add_argument("--heap-check", action="store_true", help="Enable periodic heap checks")
    parser.add_argument("--metal-validation", action="store_true", help="Enable Metal validation")
    parser.add_argument("--dry-run", action="store_true", help="Show settings without launching")
    args = parser.parse_args()
    app = args.app.expanduser().resolve()
    executable = app / "Contents/MacOS/NxEmu"
    if not executable.is_file():
        parser.error(f"NxEmu executable not found: {executable}")
    env = build_environment(os.environ, args.heap_check, args.metal_validation)
    diagnostics = {
        key: value for key, value in env.items()
        if key.startswith("Malloc") or key == "MTL_DEBUG_LAYER"
    }
    print(json.dumps({"executable": str(executable), "diagnostics": diagnostics}, indent=2), flush=True)
    if args.dry_run:
        return 0
    output = None
    try:
        if args.log:
            output = args.log.expanduser().open("x")
        proc = subprocess.Popen(
            [str(executable)], cwd=app.parent, env=env,
            stdout=output, stderr=subprocess.STDOUT if output else None,
        )
        print(f"NxEmu PID {proc.pid}", flush=True)
        status = proc.wait()
        return status if status >= 0 else 128 - status
    finally:
        if output:
            output.close()


if __name__ == "__main__":
    raise SystemExit(main())
