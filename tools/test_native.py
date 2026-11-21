"""Builds and runs the unit tests on the PC without PlatformIO's native toolchain.

CI runs `pio test -e native` (gcc on Linux). On Windows there is usually no gcc, so this script uses
any C/C++ compiler it finds: g++/clang++ on PATH, or Zig's bundled clang (`pip install ziglang`).
It also compiles the host build (host/station_host.cpp) used by the end-to-end demo.

    python tools/test_native.py          # tests
    python tools/test_native.py --host   # tests + build/station_host(.exe)
"""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "lib" / "station_core" / "src"
BUILD = ROOT / "build"
EXE = ".exe" if os.name == "nt" else ""
FLAGS = ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]


def compilers() -> tuple[list[str], list[str]]:
    for cxx, cc in (("g++", "gcc"), ("clang++", "clang")):
        if shutil.which(cxx) and shutil.which(cc):
            return [cxx], [cc]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"], [sys.executable, "-m", "ziglang", "cc"]
    except ImportError:
        sys.exit("no C++ compiler: install gcc/clang or `pip install ziglang`")


def unity_dir() -> Path:
    cands = [ROOT / ".pio" / "libdeps" / "native" / "Unity" / "src",
             Path.home() / ".platformio" / "lib" / "Unity" / "src",
             *map(Path, glob.glob(str(Path.home() / ".platformio" / "packages" / "tool-unity*")))]
    for c in cands:
        if (c / "unity.c").exists():
            return c
    sys.exit("Unity not found: `pio pkg install -g -l throwtheswitch/Unity`")


def run(cmd: list[str]):
    print(" ".join(str(c) for c in cmd[-6:]) if len(cmd) > 6 else " ".join(map(str, cmd)))
    subprocess.run(cmd, check=True)


def main():
    cxx, cc = compilers()
    extra = ["-fno-sanitize=undefined"] if "ziglang" in cc else []   # zig enables UBSan without its runtime
    FLAGS.extend(extra)
    BUILD.mkdir(exist_ok=True)
    u = unity_dir()
    core = sorted(str(p) for p in CORE.glob("*.cpp"))
    unity_obj = BUILD / "unity.o"
    run(cc + ["-O1", *extra, "-c", str(u / "unity.c"), "-I", str(u), "-o", str(unity_obj)])
    test_exe = BUILD / f"test_core{EXE}"
    run(cxx + FLAGS + ["-I", str(CORE), "-I", str(u), str(ROOT / "test" / "test_core" / "test_main.cpp"), *core,
                       str(unity_obj), "-o", str(test_exe)])
    r = subprocess.run([str(test_exe)])
    if "--host" in sys.argv:
        run(cxx + FLAGS + ["-I", str(CORE), str(ROOT / "host" / "station_host.cpp"), *core,
                           "-o", str(BUILD / f"station_host{EXE}")])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
