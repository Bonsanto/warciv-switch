#!/usr/bin/env python3
"""Give an embedded ELF engine its own Freeciv and Lua symbols/state."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ld", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("output", type=Path)
    parser.add_argument("archives", nargs="+")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="freeciv-engine-") as temporary:
        combined = Path(temporary) / "server.o"
        mapping = Path(temporary) / "symbols.txt"
        layout = Path(temporary) / "server.ld"
        # All writable engine state has a separate lifetime from the client.
        # Restoring these sections between joined server threads resets even
        # process-lifetime caches and module registries in upstream Freeciv.
        layout.write_text("""SECTIONS {
  .data.fc_embedded : {
    fc_embedded_data_start = .;
    *(.data .data.* .sdata .sdata.*)
    fc_embedded_data_end = .;
  }
  .bss.fc_embedded (NOLOAD) : {
    fc_embedded_bss_start = .;
    *(.bss .bss.* .sbss .sbss.* COMMON)
    fc_embedded_bss_end = .;
  }
}
""")
        subprocess.run(
            [args.ld, "-r", "-T", str(layout), "-o", str(combined), "--whole-archive",
             *args.archives, "--no-whole-archive"], check=True
        )
        symbols = subprocess.check_output(
            [args.nm, "-g", "--defined-only", "--format=posix", str(combined)],
            text=True,
        )
        renames = {}
        for line in symbols.splitlines():
            name = line.split()[0]
            if not name.startswith("fc_embedded_"):
                renames[name] = "fc_server_" + name
        # Only the isolated engine's exits are intercepted. libc, SDL and
        # the client retain their normal process-level behavior.
        renames.update({"exit": "fc_embedded_exit",
                        "_exit": "fc_embedded_emergency_exit",
                        "atexit": "fc_embedded_atexit",
                        "strtok": "fc_embedded_strtok"})
        mapping.write_text("".join(f"{old} {new}\n"
                                   for old, new in sorted(renames.items())))
        subprocess.run(
            [args.objcopy, "--redefine-syms=" + str(mapping),
             str(combined), str(args.output)], check=True
        )


if __name__ == "__main__":
    main()
