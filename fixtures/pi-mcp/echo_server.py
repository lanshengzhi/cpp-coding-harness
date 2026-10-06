#!/usr/bin/env python3
"""MCP stdio test server (spec #865, ticket #869 fixture).

Newline-delimited compact JSON-RPC 2.0 framing, exactly the shape pi v1.0.4's
`packages/mcp/src/transports/stdio.ts` speaks: one compact JSON object per
`\n`-terminated line, requests on stdin, responses on stdout, diagnostics on
stderr. No `Content-Length` framing.

Methods:

  initialize                  MCP handshake (protocolVersion, capabilities, serverInfo)
  notifications/initialized   notification; no response
  tools/list                  one tool per page, `nextCursor` until exhausted
  tools/call                  name = echo | fail | crash
  debug/emit_garbage          write a non-JSON line, then the valid response
  debug/emit_invalid_jsonrpc  write valid JSON that is not a JSON-RPC message

`tools/call` for `echo` returns the incoming `text`; for `fail` returns an
`isError: true` result; for `crash` exits immediately without responding, which
the client observes as the connection closing mid-request.
"""

import json
import os
import sys

TOOLS = [
    {
        "name": "echo",
        "description": "Echo the incoming text back.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
    {
        "name": "fail",
        "description": "Always report a tool-level error.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "crash",
        "description": "Exit the server without responding.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]

PAGE_SIZE = 1


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


def handle_tools_list(rid, params):
    cursor = params.get("cursor")
    start = 0
    if cursor is not None:
        try:
            start = int(cursor)
        except (TypeError, ValueError):
            respond_error(rid, -32602, "invalid cursor")
            return
    page = TOOLS[start : start + PAGE_SIZE]
    result = {"tools": page}
    next_start = start + PAGE_SIZE
    if next_start < len(TOOLS):
        result["nextCursor"] = str(next_start)
    respond(rid, result)


def handle_tools_call(rid, params):
    name = params.get("name")
    arguments = params.get("arguments") or {}
    if name == "echo":
        respond(rid, {"content": [{"type": "text", "text": str(arguments.get("text", ""))}]})
    elif name == "fail":
        respond(
            rid,
            {
                "isError": True,
                "content": [{"type": "text", "text": "fail: requested failure"}],
            },
        )
    elif name == "crash":
        sys.stderr.write("pi-mcp-echo: crashing on tools/call crash\n")
        sys.stderr.flush()
        os._exit(3)
    else:
        respond_error(rid, -32602, "Unknown tool: " + str(name))


def main():
    for raw in sys.stdin:
        line = raw.rstrip("\r\n")
        if not line.strip():
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            respond_error(None, -32700, "Parse error")
            continue

        method = message.get("method")
        rid = message.get("id")
        if rid is None:
            # `notifications/initialized` and other notifications: no response.
            continue

        if method == "initialize":
            respond(
                rid,
                {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "pi-mcp-echo", "version": "1.0.0"},
                },
            )
        elif method == "tools/list":
            handle_tools_list(rid, message.get("params") or {})
        elif method == "tools/call":
            handle_tools_call(rid, message.get("params") or {})
        elif method == "ping":
            respond(rid, {})
        elif method == "debug/emit_garbage":
            send_raw("this is not json")
            respond(rid, {"after_garbage": True})
        elif method == "debug/emit_invalid_jsonrpc":
            send_raw('{"jsonrpc":"2.0"}')
            respond(rid, {"after_invalid": True})
        else:
            respond_error(rid, -32601, "Method not found: " + str(method))


if __name__ == "__main__":
    main()
