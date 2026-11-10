"""Link the standalone harness against an isolated MSVC loader build.

Prerequisite: build nxemu-loader.vcxproj with NxLoaderOutputDir and
NxLoaderIntDir pointing at the 'loader' and 'obj' children of --build-dir.
Run from a VS x64 Developer shell. This does not launch a game or alter profiles.
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--trace-source", type=Path, help="NXFormat session_trace_benchmark.cpp; use actual NXEmu VFS")
    parser.add_argument("--lifecycle-dll", action="store_true", help="build test-only module unload probe")
    args = parser.parse_args()
    output = args.build_dir.resolve()
    objects = sorted((output / "obj").glob("*.obj"))
    if not objects:
        parser.error("No isolated loader objects found")
    log = (output / "obj/nxemu-loader.tlog/link.command.1.tlog").read_text(encoding="utf-16")
    libraries = []
    for line in log.splitlines():
        path = line.strip().strip('"')
        if path.lower().endswith(".lib") and Path(path).is_file() and path not in libraries:
            libraries.append(path)
    if not libraries:
        parser.error("Loader link log contains no dependency libraries")
    source = args.trace_source.resolve() if args.trace_source else Path(__file__).with_name("native_reader.cpp")
    name = "session_trace_benchmark" if args.trace_source else "native_test"
    if args.lifecycle_dll:
        source = Path(__file__).with_name("module_lifecycle.cpp")
        name = "nx_lifecycle"
    extension = '.dll' if args.lifecycle_dll else '.exe'
    trace_flags = (["/DNX_BENCH_NXEMU=1", f"/I{ROOT / 'src/nxemu-loader/nxformat'}",
                    f"/I{source.parents[1] / 'tests/support'}"] if args.trace_source else [])
    command = ["cl", "/nologo", "/std:c++20", "/EHsc", "/MT", "/O2", "/DNOMINMAX", *trace_flags,
               *(['/LD'] if args.lifecycle_dll else []),
               f"/I{ROOT / 'src/nxemu-loader'}", f"/I{ROOT / 'src'}",
               f"/I{ROOT / 'external/boost'}", f"/I{ROOT / 'external/fmt/include'}",
               str(source),
               f"/Fo:{output / (name + '.obj')}", f"/Fe:{output / (name + extension)}",
               *map(str, objects), *libraries, "/link", "/SUBSYSTEM:CONSOLE", "/IGNORE:4199",
               "user32.lib", "gdi32.lib", "advapi32.lib", "shell32.lib", "ole32.lib",
               "oleaut32.lib", "uuid.lib", "ws2_32.lib", "shlwapi.lib", "version.lib",
               "userenv.lib", "wtsapi32.lib", "delayimp.lib"]
    subprocess.run(command, cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
