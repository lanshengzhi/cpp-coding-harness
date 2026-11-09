#!/usr/bin/env python3
"""Report a redacted, read-only snapshot for a proposed code lane."""
import sys
sys.dont_write_bytecode = True

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess


class SafeParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, "preflight: unknown: invalid or incomplete arguments\n")


def git(directory, *args):
    env = {"PATH": os.defpath, "HOME": "/nonexistent", "XDG_CONFIG_HOME": "/nonexistent",
           "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
           "GIT_OPTIONAL_LOCKS": "0", "LC_ALL": "C"}
    try:
        result = subprocess.run(["git", "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false", "-C", str(directory), *args], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True)
        return result.stdout
    except (OSError, subprocess.CalledProcessError):
        return None


def status(directory, exclude_lock=False):
    # Filters can execute arbitrary commands during status. Inspect configuration
    # before status and fail closed if any filter is configured.
    config = git(directory, "config", "--null", "--list", "--includes")
    if config is None or any(item.split(b"\n", 1)[0].lower().startswith((b"filter.", b"include.", b"includeif.", b"trace2."))
                             for item in config.split(b"\0") if item):
        return None
    # Git otherwise hides executable-bit changes, and forcing filemode on a
    # filesystem that cannot represent them cannot establish cleanliness.
    if git(directory, "config", "--bool", "--get", "core.filemode") != b"true\n":
        return None
    flags = git(directory, "ls-files", "-v", "-z")
    entries = git(directory, "ls-files", "--stage", "-z")
    if flags is None or entries is None:
        return None
    if any(len(item) < 3 or item[1:2] != b" " or item[:1] != b"H"
           for item in flags.split(b"\0") if item):
        return None
    modes = [item.split(b" ", 1)[0] for item in entries.split(b"\0") if item]
    # Git's index supports regular files, symbolic links and gitlinks. Reject
    # gitlinks and unknown modes; false symlink support hides type replacements.
    if any(mode not in (b"100644", b"100755", b"120000") for mode in modes):
        return None
    if b"120000" in modes and git(directory, "config", "--bool", "--get", "core.symlinks") != b"true\n":
        return None
    raw = git(directory, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignore-submodules=none")
    if raw is None:
        return None
    records = raw.split(b"\0")
    counts = [0, 0, 0]
    i = 0
    while i < len(records) and records[i]:
        entry = records[i]
        i += 1
        if len(entry) < 4:
            return None
        flags, name = entry[:2], entry[3:]
        if b"R" in flags or b"C" in flags:
            if i >= len(records) or not records[i]:
                return None
            i += 1
        if exclude_lock and name == b".lane-lock.json" and flags == b"??":
            continue
        if flags == b"??":
            counts[2] += 1
        else:
            counts[0] += flags[:1] != b" "
            counts[1] += flags[1:] != b" "
    return counts


def lock_state(root, lane):
    path = root / ".lane-lock.json"
    try:
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except FileNotFoundError:
            return "absent"
        with os.fdopen(fd, "rb") as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                return "unknown"
            content = stream.read(65537)
        if len(content) > 65536:
            return "unknown"
        def unique(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError()
                result[key] = value
            return result
        data = json.loads(content.decode("utf-8"), object_pairs_hook=unique)
        if not isinstance(data, dict) or not {"lane", "agent", "startedAt"} <= data.keys() or data.keys() - {"lane", "agent", "startedAt", "taskId"}:
            return "unknown"
        holder, agent, started = data["lane"], data["agent"], data["startedAt"]
        if not isinstance(holder, str) or not re.fullmatch(r"[A-Za-z0-9._-]+", holder):
            return "unknown"
        if not isinstance(agent, str) or not re.fullmatch(r"[A-Za-z0-9._:@#-]+", agent):
            return "unknown"
        if "taskId" in data and (not isinstance(data["taskId"], str) or not re.fullmatch(r"[A-Za-z0-9._:@#-]+", data["taskId"])):
            return "unknown"
        if not isinstance(started, str) or not re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", started):
            return "unknown"
        datetime.strptime(started, "%Y-%m-%dT%H:%M:%SZ")
        return "matching" if holder == lane else "foreign"
    except (OSError, ValueError, UnicodeError, RecursionError):
        return "unknown"


def main():
    parser = SafeParser(description=__doc__)
    parser.add_argument("--lane", required=True)
    parser.add_argument("--revision", required=True, help="full expected upstream commit ID")
    parser.add_argument("--checkout", help="upstream checkout (default: PI_CHECKOUT)")
    parser.add_argument("--ctest-regex")
    parser.add_argument("--expected-count", type=int)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.lane) or not re.fullmatch(r"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}", args.revision):
        parser.error("invalid identity")
    if (args.ctest_regex is None) != (args.expected_count is None) or (args.expected_count is not None and (args.expected_count < 0 or not args.ctest_regex)):
        parser.error("incomplete selection")

    failed = False
    def report(level, message):
        nonlocal failed
        print(f"{level}: {message}")
        failed |= level != "pass"

    if not shutil.which("git", path=os.defpath):
        report("unknown", "git unavailable")
        return 1
    raw_root = git(Path.cwd(), "rev-parse", "--show-toplevel")
    if raw_root is None:
        report("unknown", "current checkout unavailable")
        return 1
    root = Path(os.fsdecode(raw_root.rstrip(b"\n")))
    head = git(root, "rev-parse", "--verify", "HEAD")
    branch = git(root, "symbolic-ref", "--quiet", "--short", "HEAD")
    counts = status(root, exclude_lock=True)
    if head is None or branch is None or counts is None or not re.fullmatch(rb"[0-9a-f]{40}|[0-9a-f]{64}", head.strip()):
        report("unknown", "current root, branch, HEAD or status unavailable")
    else:
        report("attention" if any(counts) else "pass", f"current checkout: attached HEAD {head.decode().strip()}; staged={counts[0]} unstaged={counts[1]} untracked={counts[2]}")
    lock = lock_state(root, args.lane)
    report("unknown" if lock == "unknown" else "attention" if lock == "foreign" else "pass", f"lock: {lock} (snapshot; matching identity does not prove ownership)")
    checkout = args.checkout or os.environ.get("PI_CHECKOUT")
    if not checkout:
        report("unknown", "upstream checkout not supplied")
    else:
        path = Path(checkout)
        upstream_root = git(path, "rev-parse", "--show-toplevel")
        upstream_head = git(path, "rev-parse", "--verify", "HEAD")
        upstream_status = status(path)
        try:
            at_root = upstream_root is not None and Path(os.fsdecode(upstream_root.rstrip(b"\n"))).resolve() == path.resolve()
        except OSError:
            at_root = False
        if not at_root or upstream_head is None or upstream_status is None:
            report("unknown", "upstream checkout unavailable or path is not its Git root")
        elif upstream_head.strip().decode("ascii", errors="replace").lower() != args.revision.lower():
            report("attention", "upstream revision mismatch")
        elif any(upstream_status):
            report("attention", f"upstream dirty: staged={upstream_status[0]} unstaged={upstream_status[1]} untracked={upstream_status[2]}")
        else:
            report("pass", "upstream revision and cleanliness match")
    if args.ctest_regex is None:
        print("pass: CTest selection skipped")
    else:
        report("unknown", "CTest selection unavailable: listing writes to the build tree; no listing was run")
    print("snapshot only; this does not authorize a lane or establish capture correctness")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
