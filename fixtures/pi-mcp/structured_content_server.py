#!/usr/bin/env python3
"""MCP stdio fixture for #1101 `tools/call` JSON result preservation.

Newline-delimited compact JSON-RPC 2.0 framing, like the ordinary
`echo_server.py`. One advertised tool, `structured`, returns the result shape
selected by `arguments.kind`:

  object         content absent, structuredContent = {ok, nested}
  array          content absent, structuredContent = [1, {x}, null]
  empty-array    content absent, structuredContent = []
  string         content absent, structuredContent = "text"
  empty-string   content absent, structuredContent = ""
  false          content absent, structuredContent = false
  zero           content absent, structuredContent = 0
  null           content absent, structuredContent = null
  missing        content absent, structuredContent absent
  wrong-content  content = "not a list"
  null-content   content = null
  malformed      result = []

The server never validates the result shape: it is the raw wire fact the owner
and application layers must preserve or reject themselves.
"""

import json
import sys

TOOL = {
    "name": "structured",
    "description": "Return one JSON structured result shape.",
    "inputSchema": {
        "type": "object",
        "properties": {"kind": {"type": "string"}},
        "required": ["kind"],
    },
}

RESULTS = {
    "object": {"structuredContent": {"ok": True, "nested": [True, None, "x"]}},
    "object-wire": {"structuredContent": {"ok": True, "nested": [True, None, "x"]}},
    "object-reverse": {"structuredContent": {"z": "last", "a": "first", "m": "middle"}},
    "object-nested": {"structuredContent": {"outer": {"z": 1, "a": 2}, "first": True}},
    "object-integer-keys": {"structuredContent": {"10": "ten", "2": "two", "b": True, "a": False}},
    "object-numbers": {
        "structuredContent": {
            "integer": 42,
            "negative": -7,
            "float": 1.5,
            "large": 9007199254740993,
            "exponent": 1e21,
        }
    },
    "object-number-shapes": {
        "structuredContent": {
            "zero": 0,
            "negativeZero": -0.0,
            "small": 0.000001,
            "smaller": 1e-7,
            "large": 1e20,
            "exponent": 1e21,
            "fraction": 123.45,
            "precise": 1.2345678901234567e30,
        }
    },
    "array": {"structuredContent": [1, {"x": True}, None]},
    "empty-array": {"structuredContent": []},
    "string": {"structuredContent": "text"},
    "empty-string": {"structuredContent": ""},
    "false": {"structuredContent": False},
    "zero": {"structuredContent": 0},
    "null": {"structuredContent": None},
    "missing": {},
    "content-text-structured": {
        "content": [{"type": "text", "text": "alpha"}],
        "structuredContent": {"ok": True},
    },
    "empty-content-structured": {"content": [], "structuredContent": {"ok": True}},
    "missing-content-isError": {"isError": True},
    "empty-text-isError": {"content": [{"type": "text", "text": ""}], "isError": True},
    "wrong-content": {"content": "not a list"},
    "null-content": {"content": None},
}


def send(message):
    sys.stdout.write(json.dumps(message, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def respond(rid, result):
    send({"jsonrpc": "2.0", "id": rid, "result": result})


def respond_error(rid, code, message):
    send({"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}})


def handle_tools_call(rid, params):
    if params.get("name") != "structured":
        respond_error(rid, -32602, "Unknown tool: " + str(params.get("name")))
        return
    arguments = params.get("arguments") or {}
    kind = arguments.get("kind")
    if kind == "malformed":
        respond(rid, [])
        return
    if kind not in RESULTS:
        respond_error(rid, -32602, "Unknown kind: " + str(kind))
        return
    respond(rid, RESULTS[kind])


def main():
    for raw in sys.stdin:
        line = raw.rstrip("\r\n")
        if not line.strip():
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        method = message.get("method")
        rid = message.get("id")
        if method == "initialize":
            respond(
                rid,
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "structured", "version": "1.0.0"},
                },
            )
        elif method == "notifications/initialized":
            continue
        elif method == "tools/list":
            respond(rid, {"tools": [TOOL]})
        elif method == "tools/call":
            handle_tools_call(rid, message.get("params") or {})
        elif rid is not None:
            respond_error(rid, -32601, "Method not found: " + str(method))


if __name__ == "__main__":
    main()
