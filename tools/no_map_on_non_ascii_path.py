# PlatformIO post-script. The ESP32 linker (binutils on Windows) can't open the .map file when the
# project path has non-ASCII characters ("Estación", "Informática"), and the link fails. The map file is
# only a build report, so drop it in that case.
Import("env")  # noqa: F821  (SCons injects it)

build_dir = env.subst("$BUILD_DIR")  # noqa: F821
if not build_dir.isascii():
    flags = list(env["LINKFLAGS"])  # noqa: F821
    out = []
    skip = False
    for f in flags:
        if skip:
            skip = False
            continue
        if str(f) in ("-Wl,--Map", "-Wl,-Map"):
            skip = True
            continue
        if str(f).startswith(("-Wl,--Map=", "-Wl,-Map=")):
            continue
        out.append(f)
    env.Replace(LINKFLAGS=out)  # noqa: F821
    print("non-ASCII project path: linker map file disabled")
