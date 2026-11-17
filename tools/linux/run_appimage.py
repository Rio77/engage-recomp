#!/usr/bin/env python3
"""Give each extraction-mode launch its own temporary directory (MIT)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

def main():
    application = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='engage-recomp-') as temporary:
        environment = dict(os.environ, TMPDIR=temporary, APPIMAGE_EXTRACT_AND_RUN='1')
        return subprocess.call([str(application), *sys.argv[2:]], env=environment)
if __name__ == '__main__': sys.exit(main())
