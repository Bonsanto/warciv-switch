#!/usr/bin/env python3
"""Build a self-contained Switch 1 homebrew application with devkitPro."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from check_nro import inspect_nro


def run(command, root, log):
    print("Running: " + " ".join(map(str, command)), flush=True)
    with log.open("w") as output:
        result = subprocess.run(command, cwd=root, stdout=output,
                                stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text()[-14000:])
        raise SystemExit(result.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    devkit = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
    tools = devkit / "devkitA64/bin"
    portlibs = devkit / "portlibs/switch"
    libnx = devkit / "libnx"
    build = root / "build-switch"
    host = root / "build-switch-host"
    dependencies = root / "build-switch-deps/install"
    build.mkdir(exist_ok=True)

    subprocess.run([sys.executable, str(root / "platforms/switch/build_icu.py"),
                    "--workdir", str(dependencies.parent), "--jobs", str(args.jobs)],
                   cwd=root, check=True)
    options = ["-Dsyslua=false", "-Dfcdb=[]", "-Dfcmp=[]", "-Dtools=[]",
               "-Dmwand=false", "-Dreadline=false", "-Dnls=false",
               "-Dsvgflags=false"]
    if not (host / "build.ninja").exists():
        run(["meson", "setup", str(host), "-Dclients=[]", "-Dserver=disabled",
             "-Daudio=none", *options], root, build / "host-setup.log")
    run(["ninja", "-C", str(host), f"-j{args.jobs}", "tolua"],
        root, build / "host-build.log")

    cross = build / "switch.ini"
    flags = ["-D__SWITCH__", "-DU_HAVE_CHAR16_T=0", "-march=armv8-a+crc+crypto",
             "-mtune=cortex-a57", "-mtp=soft", "-fPIE", "-ftls-model=local-exec",
             "-I" + str(libnx / "include"), "-I" + str(portlibs / "include"),
             "-I" + str(dependencies / "include")]
    links = ["-specs=" + str(libnx / "switch.specs"),
             "-L" + str(libnx / "lib"), "-L" + str(portlibs / "lib"),
             "-L" + str(dependencies / "lib"), "-lstdc++", "-lnx", "-lpthread"]
    binaries = {name: str(tools / ("aarch64-none-elf-" + executable))
                for name, executable in {"c": "gcc", "cpp": "g++", "ar": "ar",
                                         "strip": "strip", "ld": "ld", "nm": "nm",
                                         "objcopy": "objcopy"}.items()}
    binaries.update({"pkgconfig": shutil.which("pkg-config"),
                     "tolua": str(host / "tolua")})
    cross.write_text(
        "[binaries]\n" + "".join(f"{key} = {value!r}\n" for key, value in binaries.items())
        + "\n[host_machine]\nsystem = 'horizon'\ncpu_family = 'aarch64'\n"
          "cpu = 'cortex-a57'\nendian = 'little'\n\n[properties]\n"
          "needs_exe_wrapper = true\n"
        + f"cross_inc_path = {str(portlibs / 'include')!r}\n"
        + f"cross_lib_path = {str(portlibs / 'lib')!r}\n"
        + f"pkg_config_libdir = {[str(dependencies / 'lib/pkgconfig'), str(portlibs / 'lib/pkgconfig')]!r}\n"
        + "\n[built-in options]\n"
        + f"c_args = {flags!r}\ncpp_args = {flags!r}\n"
        + f"c_link_args = {links!r}\ncpp_link_args = {links!r}\n"
    )
    setup = ["meson", "setup", str(build), "--cross-file", str(cross),
             "-Dclients=sdl2", "-Dserver=enabled", "-Dembedded-server=true",
             "-Ddefault_library=static", "-Daimodules=classic", "-Daudio=sdl2",
             "--buildtype=release", *options]
    if (build / "meson-private/coredata.dat").exists():
        setup.append("--reconfigure")
    run(setup, root, build / "setup.log")
    run(["ninja", "-C", str(build), f"-j{args.jobs}", "freeciv-sdl2"],
        root, build / "compile.log")

    romfs = build / "romfs"
    shutil.copytree(root / "data", romfs / "data", dirs_exist_ok=True)
    shutil.copy2(root / "COPYING", romfs / "COPYING")
    shutil.copy2(dependencies.parent / "icu/LICENSE", romfs / "COPYING.ICU")
    output = root / "dist-switch/switch/warciv"
    output.mkdir(parents=True, exist_ok=True)
    nacp = build / "warciv.nacp"
    switch_tools = devkit / "tools/bin"
    subprocess.run([str(switch_tools / "nacptool"), "--create", "Warciv: Largepox",
                    "Freeciv contributors", "0.1.0", str(nacp)], check=True)
    run([str(switch_tools / "elf2nro"), str(build / "freeciv-sdl2"),
         str(output / "warciv.nro"), "--nacp=" + str(nacp),
         "--icon=" + str(libnx / "default_icon.jpg"),
         "--romfsdir=" + str(romfs)], root, build / "package.log")
    report = inspect_nro(output / "warciv.nro", root)
    (output / "build-info.json").write_text(json.dumps(report, indent=2) + "\n")
    shutil.copy2(root / "platforms/switch/README.md", output / "README.md")
    print(f"Switch application: {output / 'warciv.nro'}")


if __name__ == "__main__":
    main()
