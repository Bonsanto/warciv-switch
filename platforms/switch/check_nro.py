#!/usr/bin/env python3
"""Check NRO/ASET bounds and compare embedded game assets to their sources.

NRO assets: https://switchbrew.org/wiki/NRO0#Assets
RomFS layout: switchbrew/libnx, nx/source/runtime/devices/romfs_dev.c
"""

import argparse
import hashlib
import json
import mmap
from pathlib import Path
import struct


def inspect_nro(path, source):
    with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[0x10:0x14] != b"NRO0":
            raise ValueError("Missing NRO0 header")
        assets = struct.unpack_from("<I", data, 0x18)[0]
        if data[assets:assets + 4] != b"ASET":
            raise ValueError("Missing homebrew asset header")
        sections = {}
        for name, offset in [("icon", 8), ("nacp", 24), ("romfs", 40)]:
            start, size = struct.unpack_from("<QQ", data, assets + offset)
            if start < 56 or size == 0 or assets + start + size > len(data):
                raise ValueError(f"Invalid {name} bounds")
            sections[name] = (assets + start, size)
        nacp, _ = sections["nacp"]
        title = data[nacp:nacp + 512].split(b"\0")[0].decode("utf-8")
        if title != "Warciv: Largepox":
            raise ValueError(f"Unexpected application title: {title}")

        romfs, romfs_size = sections["romfs"]
        header = struct.unpack_from("<10Q", data, romfs)
        if header[0] != 0x50:
            raise ValueError("Invalid RomFS header")
        for offset, size in zip(header[1:9:2], header[2:10:2]):
            if offset + size > romfs_size:
                raise ValueError("Invalid RomFS table bounds")
        directories = romfs + header[3]
        files = romfs + header[7]
        payload = romfs + header[9]
        seen_directories = set()
        seen_files = set()
        entries = {}
        invalid = 0xffffffff

        def walk(directory, parent):
            if directory in seen_directories or directory + 24 > header[4]:
                raise ValueError("Invalid RomFS directory link")
            seen_directories.add(directory)
            _, sibling, child, file, _, length = struct.unpack_from("<6I", data, directories + directory)
            if directory + 24 + length > header[4]:
                raise ValueError("Invalid directory name bounds")
            name = data[directories + directory + 24:directories + directory + 24 + length].decode()
            current = parent / name if name else parent
            while file != invalid:
                if file in seen_files or file + 32 > header[8]:
                    raise ValueError("Invalid RomFS file link")
                seen_files.add(file)
                _, next_file, offset, size, _, length = struct.unpack_from("<IIQQII", data, files + file)
                if file + 32 + length > header[8] or header[9] + offset + size > romfs_size:
                    raise ValueError("Invalid RomFS file bounds")
                name = data[files + file + 32:files + file + 32 + length].decode()
                entries[(current / name).as_posix()] = (payload + offset, size)
                file = next_file
            while child != invalid:
                child = walk(child, current)
            return sibling

        walk(0, Path())
        required = ["data/trident.tilespec", "data/trident/tiles.png",
                    "data/trident/units.png", "data/classic-warclient/game.ruleset",
                    "data/classic-warclient/effects.ruleset",
                    "data/classic-warclient-competitive.serv",
                    "data/switch-largepox.serv", "COPYING"]
        for name in required:
            if name not in entries:
                raise ValueError(f"Missing embedded asset: {name}")
            offset, size = entries[name]
            if data[offset:offset + size] != (source / name).read_bytes():
                raise ValueError(f"Embedded asset differs from source: {name}")
        if "COPYING.ICU" not in entries:
            raise ValueError("Missing ICU license")
        return {"format": "NRO0", "title": title, "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "romfs_files": len(entries), "verified_assets": required,
                "hardware_tested": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nro", type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect_nro(args.nro, Path(__file__).resolve().parents[2]), indent=2))


if __name__ == "__main__":
    main()
