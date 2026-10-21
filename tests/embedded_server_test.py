#!/usr/bin/env python3
"""Exercise real largepox games and verify the resulting saved rules."""

import gzip
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    source = Path(sys.argv[2]).resolve()
    environment = dict(os.environ, FREECIV_DATA_PATH=str(source / "data"))
    with tempfile.TemporaryDirectory(prefix="warciv-embedded-") as temporary:
        result = subprocess.run(
            [str(executable), str(source / "tests/embedded_server.serv"), temporary],
            env=environment, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=150,
        )
        if result.returncode:
            print(result.stdout[-12000:])
            raise SystemExit(result.returncode)
        saves = list(Path(temporary).glob("*-final.sav.gz"))
        assert len(saves) == 1, "No final game was saved"
        saved = gzip.decompress(saves[0].read_bytes()).decode("utf-8")
        assert 'rulesetdir="classic-warclient"' in saved
        assert re.search(r'^turn=7$', saved, re.MULTILINE)
        assert re.search(r'^"ailargepox",TRUE,', saved, re.MULTILINE)
        assert re.search(r'^"trade_revenue_style","WARCLIENT",', saved, re.MULTILINE)
        assert re.search(r'^"topology","",', saved, re.MULTILINE)
        assert result.stdout.count("Game ended as the turn limit was exceeded.") == 2
        print("Two local largepox games, final save and independent client state: OK")


if __name__ == "__main__":
    main()
