#!/usr/bin/env python3
"""Exercise real largepox games and verify the resulting saved rules."""

import gzip
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def read_save(directory, suffix):
    saves = list(directory.glob("*" + suffix))
    assert len(saves) == 1, f"Expected one {suffix} save, found {saves}"
    saved = gzip.decompress(saves[0].read_bytes()).decode("utf-8")
    assert 'rulesetdir="classic-warclient"' in saved
    assert re.search(r'^"ailargepox",TRUE,', saved, re.MULTILINE)
    assert re.search(r'^"trade_revenue_style","WARCLIENT",', saved, re.MULTILINE)
    assert re.search(r'^"topology","",', saved, re.MULTILINE)
    return saves[0], saved


def run_sessions(executable, script, directory, environment, *arguments):
    directory.mkdir()
    result = subprocess.run(
        [str(executable), str(script), str(directory), *map(str, arguments)],
        env=environment, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=150,
    )
    if result.returncode:
        print(result.stdout[-16000:])
        raise SystemExit(result.returncode)
    return result.stdout


def main():
    executable = Path(sys.argv[1]).resolve()
    source = Path(sys.argv[2]).resolve()
    environment = dict(os.environ, FREECIV_DATA_PATH=str(source / "data"),
                       FREECIV_LOCAL_ENCODING="UTF-8")
    with tempfile.TemporaryDirectory(prefix="warciv-embedded-") as temporary:
        temporary = Path(temporary)
        output = run_sessions(executable, source / "tests/embedded_server.serv",
                              temporary / "autogames", environment)
        _, saved = read_save(temporary / "autogames", "-final.sav.gz")
        assert re.search(r'^turn=7$', saved, re.MULTILINE)
        assert output.count("Game ended as the turn limit was exceeded.") == 2

        stop_script = temporary / "stop.serv"
        stop_script.write_text(
            '/read switch-largepox.serv\n/set minplayers 0\n/set aifill 2\n'
            '/set timeout 0\n/set endturn 1000\n/set savename embedded-stop\n'
            '/set compresstype LIBZ\n/start\n'
        )
        stop_directory = temporary / "stopped"
        marker = stop_directory / "embedded-stop-T0001-Y-4000-auto.sav.gz"
        run_sessions(executable, stop_script, stop_directory, environment,
                     "--stop-after", marker)
        stopped_file, saved = read_save(stop_directory, "-interrupted.sav.gz")
        stopped_turn = int(re.search(r'^turn=(\d+)$', saved, re.MULTILINE).group(1))

        resume_script = temporary / "resume.serv"
        resume_script.write_text(
            f'/set endturn {stopped_turn + 2}\n/set timeout -1\n'
            '/set savename embedded-resume\n/start\n'
        )
        resumed = temporary / "resumed"
        output = run_sessions(executable, resume_script, resumed, environment,
                              "--load", stopped_file)
        _, saved = read_save(resumed, "-final.sav.gz")
        assert re.search(rf'^turn={stopped_turn + 3}$', saved, re.MULTILINE)
        assert output.count("Game ended as the turn limit was exceeded.") == 2
        print("Local games, interrupted save, two reloads and client isolation: OK")


if __name__ == "__main__":
    main()
