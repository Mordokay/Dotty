#!/usr/bin/env python3
"""Host tests for the companion cartridge's pure parts (and pcm_ring.h in dotty_core).

    python3 tools/companion_test/run.py [name ...]

Builds every test_*.cpp here with clang++ (C++17) against lib/dotty_core/src, cartridges/companion
and ArduinoJson from PlatformIO's libdeps, runs it, and exits non-zero if any build or check fails.
A test prints PASS/FAIL lines (check.h) and returns non-zero on failure.
"""
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def arduinojson():
    for path in sorted(glob.glob(os.path.join(REPO, ".pio", "libdeps", "*", "ArduinoJson", "src"))):
        if os.path.exists(os.path.join(path, "ArduinoJson.h")):
            return path
    sys.exit("ArduinoJson not found in .pio/libdeps: build any env once first (pio run -e music)")


def main():
    names = sys.argv[1:]
    tests = sorted(glob.glob(os.path.join(HERE, "test_*.cpp")))
    if names:
        tests = [t for t in tests if any(n in os.path.basename(t) for n in names)]
    includes = ["-I", HERE, "-I", os.path.join(REPO, "lib", "dotty_core", "src"),
                "-I", os.path.join(REPO, "cartridges", "companion"), "-I", arduinojson()]
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for test in tests:
            name = os.path.splitext(os.path.basename(test))[0]
            sources = [test] + sorted(glob.glob(os.path.join(HERE, f"{name}.extra", "*.cpp")))
            # A test may list production .cpp files it needs on its first line: // uses: a.cpp b.cpp
            first = open(test).readline()
            if first.startswith("// uses:"):
                for rel in first.split(":", 1)[1].split():
                    sources.append(os.path.join(REPO, rel))
            exe = os.path.join(tmp, name)
            build = subprocess.run(["clang++", "-std=c++17", "-O1", "-Wall", "-DDOTTY_HOST_TEST", *includes,
                                    *sources, "-o", exe], capture_output=True, text=True)
            if build.returncode:
                print(f"BUILD FAILED {name}\n{build.stderr[-3000:]}")
                failed += 1
                continue
            result = subprocess.run([exe], capture_output=True, text=True, cwd=HERE)
            print(result.stdout, end="")
            if result.returncode:
                failed += 1
    print(f"{len(tests) - failed}/{len(tests)} test files passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
