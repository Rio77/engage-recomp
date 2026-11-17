#!/usr/bin/env python3
"""Watch preparation status and show progress for known pipelines."""
import os
from pathlib import Path
import subprocess
import sys
import time
import signal

def stop(signum, frame):
    raise SystemExit(0)
signal.signal(signal.SIGTERM, stop)

data = Path(sys.argv[1])
game_pid = int(sys.argv[2])
dialog = None
address = None
started = time.monotonic()
environment = dict(os.environ, QT_SCALE_FACTOR="1", QT_AUTO_SCREEN_SCALE_FACTOR="0")
try:
    while time.monotonic() - started < 180:
        try:
            os.kill(game_pid, 0)
        except ProcessLookupError:
            break
        try:
            total, remaining = map(int, (data / "cache/preparation.txt").read_text().split())
        except (OSError, ValueError):
            time.sleep(0.2)
            continue
        if remaining <= 0:
            break
        if address is None:
            dialog = subprocess.run(
                ["kdialog", "--title", "Engage Recomp", "--icon", str(Path(sys.argv[3])),
                 "--progressbar", ("Preparando gráficos conocidos…" if os.environ.get('LANG', '').startswith('es')
                                    else "Preparing known graphics…"), str(max(total, 1))],
                capture_output=True, text=True, env=environment, timeout=10)
            if dialog.returncode:
                break
            address = dialog.stdout.split()
            if len(address) != 2:
                address = None
                break
        subprocess.run(["qdbus", *address, "Set", "org.kde.kdialog.ProgressDialog",
                        "value", str(max(total - remaining, 0))],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
        time.sleep(0.2)
finally:
    if address:
        subprocess.run(["qdbus", *address, "close"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=3)
