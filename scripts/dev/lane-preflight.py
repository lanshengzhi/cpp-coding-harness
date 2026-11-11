#!/usr/bin/env python3
"""Report a redacted, read-only snapshot for a proposed code lane."""
import sys
sys.dont_write_bytecode = True

import argparse
from datetime import datetime
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess


DELIVERY_DIR = "docs/research/pi-v1.3.0"
DELIVERY_MANIFEST = "*-deliveries.json"
DELIVERY_SCHEMA = "pike/pi-v1.3.0-capability-deliveries/1"
MAX_MANIFEST_BYTES = 64 * 1024 * 1024
MAX_REPORTED_PATHS = 20


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


def pinned_path(value):
    """Return a validated repository-relative pinned path, or None when the pin is unsafe.

    Receipt paths are joined onto the checkout root and handed to Git, so anything that could
    escape the checkout, name a device or break the line-oriented report is refused outright.
    """
    if not isinstance(value, str) or not value or len(value) > 4096:
        return None
    if any(character in value for character in "\0\r\n\\"):
        return None
    if any(not part or part in (".", "..") for part in value.split("/")):
        return None
    return value


def read_delivery_manifest(path):
    """Return [(ticket, [(pinned path, digest)])]; raise ValueError on any unsafe shape.

    The manifest is untrusted input and is never rewritten. Duplicate JSON keys, a foreign
    schema, a repeated pin and a path outside the checkout are all treated as unreadable rather
    than guessed at, so the caller can report unknown instead of a clean drift count.
    """
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError("manifest is not a regular file")
        content = stream.read(MAX_MANIFEST_BYTES + 1)
    if len(content) > MAX_MANIFEST_BYTES:
        raise ValueError("manifest is too large")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate key")
            result[key] = value
        return result
    data = json.loads(content.decode("utf-8"), object_pairs_hook=unique)
    if not isinstance(data, dict) or data.get("schema") != DELIVERY_SCHEMA:
        raise ValueError("unexpected manifest schema")
    records = data.get("records")
    if not isinstance(records, list):
        raise ValueError("manifest records are not a list")
    parsed = []
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("manifest record is not an object")
        ticket = record.get("ticket")
        if not isinstance(ticket, str) or not re.fullmatch(r"[0-9]+", ticket):
            raise ValueError("manifest record names no ticket")
        inputs = record.get("inputs")
        if not isinstance(inputs, list) or not inputs:
            raise ValueError("manifest record pins no inputs")
        entries, seen = [], set()
        for entry in inputs:
            if not isinstance(entry, dict):
                raise ValueError("manifest input is not an object")
            pinned = pinned_path(entry.get("path"))
            digest = entry.get("sha256")
            if pinned is None or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise ValueError("manifest input is not a checkout-relative path with a sha256")
            if pinned in seen:
                raise ValueError("manifest repeats a pinned path")
            seen.add(pinned)
            entries.append((pinned, digest))
        parsed.append((ticket, entries))
    return parsed


def head_probe(root, path):
    """Return ('file', sha256) | ('absent', None) | ('other', None) | ('unknown', None).

    The raw committed blob is read, with no smudge filter, text conversion or external
    command, so a checkout-time end-of-line setting cannot turn a clean pin into drift. Only
    regular-file blobs can satisfy a pin; a symbolic link, a tree or a gitlink at the path is
    reported as other.
    """
    listing = git(root, "--literal-pathspecs", "ls-tree", "-z", "HEAD", "--", path)
    if listing is None:
        return "unknown", None
    entries = [item for item in listing.split(b"\0") if item]
    if not entries:
        return "absent", None
    if len(entries) != 1:
        return "other", None
    meta, separator, name = entries[0].partition(b"\t")
    if not separator or name != os.fsencode(path):
        return "other", None
    fields = meta.split(b" ")
    if (len(fields) != 3 or fields[0] not in (b"100644", b"100755") or fields[1] != b"blob"
            or not re.fullmatch(rb"[0-9a-f]{40}|[0-9a-f]{64}", fields[2])):
        return "other", None
    content = git(root, "cat-file", "blob", fields[2].decode("ascii"))
    if content is None:
        return "unknown", None
    return "file", hashlib.sha256(content).hexdigest()


def worktree_probe(root, path):
    """Return ('file', sha256) | ('absent', None) | ('other', None) | ('unknown', None).

    Only a regular file inside the checkout can satisfy a pin: a directory, a symbolic link, a
    FIFO or a device at the path is other, a parent directory that resolves outside the checkout
    is unknown, and an unreadable path is unknown rather than absent.
    """
    target = os.path.join(str(root), path)
    if not inside_root(root, os.path.dirname(target)):
        return "unknown", None
    try:
        info = os.lstat(target)
    except (FileNotFoundError, NotADirectoryError):
        return "absent", None
    except OSError:
        return "unknown", None
    if not stat.S_ISREG(info.st_mode):
        return "other", None
    try:
        fd = os.open(target, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError:
        return "unknown", None
    try:
        with os.fdopen(fd, "rb") as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                return "other", None
            digest = hashlib.sha256()
            for chunk in iter(lambda: stream.read(1 << 20), b""):
                digest.update(chunk)
    except OSError:
        return "unknown", None
    return "file", digest.hexdigest()


def inside_root(root, directory):
    """Whether a directory resolves inside the checkout root, so a pinned path cannot reach out."""
    try:
        resolved = os.path.realpath(directory)
        base = os.path.realpath(root)
    except OSError:
        return False
    return resolved == base or resolved.startswith(base + os.sep)


def receipt_drift(root):
    """Classify every pinned delivery input against HEAD and the working tree.

    Three classes stay distinct: committed drift is a pin HEAD already contradicts, so a clean
    checkout of HEAD cannot satisfy the receipt and a rebind is owed; uncommitted drift is a pin
    HEAD satisfies that only the working tree changes, which is temporary state; missing is a
    pinned path present in neither. Read or shape failures land in problems/unreadable, and the
    caller reports unknown rather than a clean count.
    """
    result = {"absent": False, "manifests": [], "records": 0, "inputs": 0, "committed": [],
              "uncommitted": [], "missing": [], "unreadable": [], "problems": []}
    directory = root / DELIVERY_DIR
    try:
        info = os.lstat(directory)
    except FileNotFoundError:
        result["absent"] = True
        return result
    except OSError:
        result["problems"].append("the delivery directory cannot be read")
        return result
    if not stat.S_ISDIR(info.st_mode):
        result["problems"].append("the delivery directory is not a directory")
        return result
    try:
        names = sorted(name for name in os.listdir(directory) if fnmatch.fnmatchcase(name, DELIVERY_MANIFEST))
    except OSError:
        result["problems"].append("the delivery directory cannot be listed")
        return result
    if not names:
        result["problems"].append("no " + DELIVERY_MANIFEST + " manifest was found")
        return result
    for name in names:
        label = name if re.fullmatch(r"[A-Za-z0-9._-]+", name) else "unnamed manifest"
        try:
            if not stat.S_ISREG(os.lstat(directory / name).st_mode):
                raise ValueError("manifest is not a regular file")
            parsed = read_delivery_manifest(directory / name)
        except (OSError, ValueError, UnicodeError, RecursionError):
            result["problems"].append(f"{label}: unreadable or invalid manifest")
            continue
        result["manifests"].append(label)
        result["records"] += len(parsed)
        for ticket, entries in parsed:
            for pinned, digest in entries:
                result["inputs"] += 1
                head_state, head_digest = head_probe(root, pinned)
                tree_state, tree_digest = worktree_probe(root, pinned)
                if "unknown" in (head_state, tree_state):
                    result["unreadable"].append((ticket, pinned))
                elif head_state == "absent" and tree_state == "absent":
                    result["missing"].append((ticket, pinned))
                elif head_state == "file" and head_digest == digest:
                    if tree_state == "file" and tree_digest == digest:
                        continue
                    result["uncommitted"].append((ticket, pinned))
                else:
                    result["committed"].append((ticket, pinned))
    return result


def receipt_summary(label, entries):
    """One line naming every affected ticket and a bounded list of the pinned paths."""
    tickets = sorted({ticket for ticket, _ in entries})
    paths = sorted({path for _, path in entries})
    shown = ", ".join(paths[:MAX_REPORTED_PATHS])
    if len(paths) > MAX_REPORTED_PATHS:
        shown += f" (+{len(paths) - MAX_REPORTED_PATHS} more)"
    noun = "pinned input" if len(entries) == 1 else "pinned inputs"
    return f"delivery receipts: {label}: {len(entries)} {noun}, tickets {' '.join(tickets)}; paths: {shown}"


def report_receipts(report, root):
    """Report delivery receipt drift without writing, rebinding or repairing anything."""
    drift = receipt_drift(root)
    if drift["absent"]:
        report("pass", "delivery receipts: no delivery directory in this checkout")
        return
    summary = (f"delivery receipts: {len(drift['manifests'])} manifests, {drift['records']} records, "
               f"{drift['inputs']} pinned inputs; committed drift {len(drift['committed'])}, "
               f"uncommitted drift {len(drift['uncommitted'])}, missing {len(drift['missing'])}")
    if drift["unreadable"]:
        summary += f", unreadable {len(drift['unreadable'])}"
    if drift["problems"] or drift["unreadable"]:
        level = "unknown"
    elif drift["committed"] or drift["uncommitted"] or drift["missing"]:
        level = "attention"
    else:
        level = "pass"
    report(level, summary)
    for label, entries in (("committed drift", drift["committed"]),
                           ("uncommitted drift", drift["uncommitted"]),
                           ("missing", drift["missing"])):
        if entries:
            report("attention", receipt_summary(label, entries))
    if drift["unreadable"]:
        report("unknown", receipt_summary("unreadable pinned inputs", drift["unreadable"]))
    for problem in drift["problems"]:
        report("unknown", f"delivery receipts: {problem}")


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
    report_receipts(report, root)
    lock = lock_state(root, args.lane)
    report("unknown" if lock == "unknown" else "attention" if lock == "foreign" else "pass", f"lock: {lock} (snapshot; matching identity does not prove ownership)")
    if lock == "matching":
        # Holding the lock without the matching environment is the accident that made the
        # consolidation CTest cases refuse to run for the lane that already owned the checkout.
        # The environment value itself is never echoed.
        lane_name = os.environ.get("CCH_LANE_NAME", "")
        if lane_name == args.lane:
            report("pass", "lane environment: CCH_LANE_NAME matches the held lane")
        elif not lane_name:
            report("attention", "lane environment: CCH_LANE_NAME is unset while this lane holds the lock; run work through scripts/dev/with-lane.sh so child processes prove the lane")
        else:
            report("attention", "lane environment: CCH_LANE_NAME names another lane while this lane holds the lock; run work through scripts/dev/with-lane.sh")
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
