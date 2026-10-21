#!/usr/bin/env python3
"""Build the Unicode dependency with devkitA64, without a system install."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

VERSION = "76_1"
SHA512 = (
    "b702ab62fb37a1574d5f4a768326d0f8fa30d9db5b015605b5f8215b5d8547f83"
    "d84880c586d3dcc7b6c76f8d47ef34e04b0f51baa55908f737024dd79a42a6c"
)


def run(command, directory, environment=None):
    log = directory / "build.log"
    print(f"ICU: {directory.name}: {' '.join(map(str, command))}", flush=True)
    with log.open("a") as output:
        result = subprocess.run(command, cwd=directory, env=environment,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text()[-12000:])
        raise SystemExit(result.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workdir", type=Path, default=Path("build-switch-deps"))
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    work = args.workdir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    prefix = work / "install"
    if (prefix / "lib/libicuuc.a").exists():
        print(f"ICU already installed in {prefix}")
        return

    archive = work / f"icu4c-{VERSION}-src.tgz"
    if not archive.exists():
        url = ("https://github.com/unicode-org/icu/releases/download/"
               f"release-76-1/{archive.name}")
        print(f"Downloading {url}", flush=True)
        urllib.request.urlretrieve(url, archive)
    if hashlib.sha512(archive.read_bytes()).hexdigest() != SHA512:
        raise SystemExit("ICU archive checksum mismatch")
    source = work / "icu/source"
    if not source.exists():
        with tarfile.open(archive) as package:
            for member in package.getmembers():
                destination = (work / member.name).resolve()
                if not destination.is_relative_to(work) or member.issym() or member.islnk():
                    raise SystemExit(f"Unsafe ICU archive member: {member.name}")
            package.extractall(work)

    host = work / "host"
    target = work / "switch"
    host.mkdir(exist_ok=True)
    target.mkdir(exist_ok=True)
    configure = str(source / "configure")
    options = ["--disable-shared", "--enable-static", "--disable-extras",
               "--disable-tests", "--disable-samples", "--disable-icuio"]
    if not (host / "Makefile").exists():
        run([configure, *options], host)
    run(["make", f"-j{args.jobs}"], host)

    devkit = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
    compiler = devkit / "devkitA64/bin/aarch64-none-elf-"
    architecture = "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
    environment = dict(os.environ)
    environment.update({
        "icu_cv_host_frag": "mh-linux",
        "CC": str(compiler) + "gcc", "CXX": str(compiler) + "g++",
        "AR": str(compiler) + "ar", "RANLIB": str(compiler) + "ranlib",
        "CFLAGS": "-O2 " + architecture, "CXXFLAGS": "-O2 " + architecture,
        "CPPFLAGS": (f"-D__SWITCH__ -I{devkit}/libnx/include "
                     "-DUCONFIG_NO_DYLOAD=1 -DU_HAVE_NL_LANGINFO_CODESET=0 "
                     "-DU_HAVE_CHAR16_T=0"),
        "LDFLAGS": (f"-specs={devkit}/libnx/switch.specs "
                    f"-L{devkit}/libnx/lib"),
        "LIBS": "-lnx -lpthread",
    })
    run([configure, *options, "--disable-tools", "--host=aarch64-none-elf",
         "--with-cross-build=" + str(host), "--with-data-packaging=static",
         "--prefix=" + str(prefix)], target, environment)
    run(["make", f"-j{args.jobs}"], target, environment)
    run(["make", "install"], target, environment)
    print(f"ICU installed in {prefix}")


if __name__ == "__main__":
    main()
