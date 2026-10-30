#!/usr/bin/env python3
"""Run the Agent clock discriminator with process-local realtime jumps."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

fixture = Path(__file__).with_name("RealtimeJump.c")
with tempfile.TemporaryDirectory(prefix="cch-realtime-jump-") as directory:
    library = Path(directory) / "clock.so"
    subprocess.run([sys.argv[2], "-x", "c", "-shared", "-fPIC", str(fixture), "-o", str(library)], check=True)
    environment = os.environ.copy()
    existing_preload = environment.get("LD_PRELOAD")
    environment["LD_PRELOAD"] = f"{existing_preload}:{library}" if existing_preload else str(library)
    environment["CCH_TEST_REALTIME_JUMP"] = "1"
    subprocess.run([sys.argv[1], "tool duration ignores a changed realtime clock"], env=environment, check=True)
