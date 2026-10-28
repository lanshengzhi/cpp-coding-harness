#!/usr/bin/env python3
"""Capture fresh pi and C++ SQLite stores and assemble the candidate evidence."""
import os
from pathlib import Path
import sqlite3
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
source = Path(__file__).resolve().parent
checkout = Path(os.environ["PI_CHECKOUT"]).resolve()
capture = Path(sys.argv[1]).resolve()
# A fresh directory avoids accidentally reusing stale fixture inputs.
capture.mkdir(parents=True, exist_ok=False)
pi = capture / "pi"
cpp = capture / "cpp"
pi.mkdir()
cpp.mkdir()
env = os.environ.copy()
env.update({"NODE_OPTIONS": "--conditions=source", "TSX_TSCONFIG_PATH": str(checkout / "tsconfig.json")})
driver = [str(checkout / "node_modules/.bin/tsx"), str(source / "continue-1086-sqlite.mts")]
cpp_env = env | {"CCH_1086_CPP_STORE_OUT": str(cpp / "storage.sqlite")}
subprocess.run([str(root / "build/cch_tests_durable"),
                r"C++ creates a SQLite store\, pi advances it\, and C++ finishes it"],
               cwd=root, env=cpp_env, check=True, timeout=90)
# Read and validate the generated fixture's IDs rather than assuming allocation order.
with sqlite3.connect(f"file:{cpp / 'storage.sqlite'}?mode=ro", uri=True) as db:
    tasks = db.execute("SELECT id, conversation_id FROM tasks").fetchall()
    submissions = db.execute("SELECT id, conversation_id FROM submissions").fetchall()
    if len(tasks) != 1 or len(submissions) != 1 or tasks[0][1] != submissions[0][1]:
        raise SystemExit("unexpected C++ fixture task/submission IDs")
    task_id, conversation_id = tasks[0]
    submission_id = submissions[0][0]
subprocess.run(driver + [str(pi), "capture-pi"], cwd=root, env=env, check=True, timeout=30)
subprocess.run(driver + [str(cpp), "capture-cpp", str(task_id), str(submission_id), str(conversation_id)],
               cwd=root, env=env, check=True, timeout=30)
subprocess.run([sys.executable, str(source / "build-1086-evidence.py"), str(capture)],
               cwd=root, env=env, check=True, timeout=30)
