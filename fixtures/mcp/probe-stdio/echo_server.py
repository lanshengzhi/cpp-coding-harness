#!/usr/bin/env python3
"""Minimal newline-delimited JSON-RPC 2.0 MCP echo server (#866 probe fixture).

Framing mirrors pi v1.0.4 `packages/mcp/src/transports/stdio.ts`: one compact
JSON object per line, terminated by `\n` (not `Content-Length` headers). The
server is a stdio process: requests arrive on stdin, responses are written to
stdout, diagnostics to stderr.

Beyond the three MCP methods under test (`initialize`, `tools/list`,
`tools/call`), it exposes `debug/*` methods so the probe can drive the two
fault cases:

  debug/emit_garbage         write a non-JSON line, then the valid response
  debug/emit_invalid_jsonrpc write valid JSON that is not a valid JSON-RPC message
  debug/crash                exit without responding (server-crash-mid-request)
"""

import json
import os
import sys


def send(message):
    sys.stdout.write(json.dumps(message, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def send_raw(text):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def respond(rid, result):
    send({"jsonrpc": "2.0", "id": rid, "result": result})


def respond_error(rid, code, message):
    send({"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}})


def main():
    for raw in sys.stdin:
        line = raw.rstrip("\r\n")
        if not line.strip():
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}})
            continue

        method = message.get("method")
        rid = message.get("id")
        if rid is None:
            # Notification (for example `notifications/initialized`): no response.
            continue

        if method == "initialize":
            respond(
                rid,
                {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "probe-stdio-echo", "version": "0.1.0"},
                },
            )
        elif method == "tools/list":
            respond(
                rid,
                {
                    "tools": [
                        {
                            "name": "echo",
                            "description": "Echo the incoming text back.",
                            "inputSchema": {
                                "type": "object",
                                "properties": {"text": {"type": "string"}},
                                "required": ["text"],
                            },
                        }
                    ]
                },
            )
        elif method == "tools/call":
            params = message.get("params") or {}
            arguments = params.get("arguments") or {}
            respond(rid, {"content": [{"type": "text", "text": str(arguments.get("text", ""))}]})
        elif method == "ping":
            respond(rid, {})
        elif method == "debug/emit_garbage":
            send_raw("this is not json")
            respond(rid, {"after_garbage": True})
        elif method == "debug/emit_invalid_jsonrpc":
            send_raw('{"jsonrpc":"2.0"}')
            respond(rid, {"after_invalid": True})
        elif method == "debug/crash":
            sys.stderr.write("probe-stdio-echo: crashing on debug/crash\n")
            sys.stderr.flush()
            os._exit(3)
        else:
            respond_error(rid, -32601, "Method not found: " + str(method))


if __name__ == "__main__":
    main()
