#!/usr/bin/env python3
"""Extract one gzip+base64 SQLite fixture from a registered audit artifact."""
import base64
import gzip
import json
import sys


def main() -> int:
    artifact, output, scenario_name = sys.argv[1:4]
    with open(artifact, encoding="utf-8") as stream:
        document = json.load(stream)
    for scenario in document["scenarios"]:
        if scenario["name"] != scenario_name:
            continue
        sqlite = scenario["expected"]["fixture"]["sqlite"]
        if sqlite.get("encoding") != "gzip+base64":
            raise SystemExit(f"unsupported fixture encoding: {sqlite.get('encoding')}")
        with open(output, "wb") as stream:
            stream.write(gzip.decompress(base64.b64decode(sqlite["gzipBase64"])))
        return 0
    raise SystemExit(f"scenario not found: {scenario_name}")


if __name__ == "__main__":
    raise SystemExit(main())
