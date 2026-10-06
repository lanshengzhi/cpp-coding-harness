#!/usr/bin/env bash
# Builds (if needed) and runs the standalone MCP stdio probe.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -x "$here/probe_stdio" || ! -x "$here/probe_boost_process" ]]; then
  "$here/build.sh"
fi

status=0
# The outer timeout is a hard guard: a wedged child can never hang the run.
timeout 60 "$here/probe_stdio" "$here/echo_server.py" || status=$?
timeout 60 "$here/probe_boost_process" "$here/echo_server.py" || status=$?
exit "$status"
