#!/usr/bin/env python3
"""MCP stdio test server (spec #865, tickets #869 and #872 fixture).

Newline-delimited compact JSON-RPC 2.0 framing, exactly the shape pi v1.0.4's
`packages/mcp/src/transports/stdio.ts` speaks: one compact JSON object per
`\n`-terminated line, requests on stdin, responses on stdout, diagnostics on
stderr. No `Content-Length` framing.

Methods:

  initialize                  MCP handshake (protocolVersion, capabilities, serverInfo)
  notifications/initialized   notification; no response
  tools/list                  one tool per page, `nextCursor` until exhausted
  resources/list              one resource per page, `nextCursor` until exhausted;
                              carries a `_meta`/icons resource, a `ui://` MCP App
                              resource, and a resource with no `name`
  resources/templates/list    one template page
  resources/read              text and base64-blob contents for a known URI
  tools/call                  name = echo | fail | crash | hang | slow
  debug/emit_garbage          write a non-JSON line, then the valid response
  debug/emit_invalid_jsonrpc  write valid JSON that is not a JSON-RPC message
  debug/exit                  respond, then exit 0 (simulates a server that
                              dies between tools/list and tools/call)

`tools/call` for `echo` returns the incoming `text`; for `fail` returns an
`isError: true` result; for `crash` exits immediately without responding; for
`hang` never responds and, once the client sends `notifications/cancelled` for
it, keeps streaming notification frames without ever completing the request;
for `slow` sleeps `arguments.ms` (default 50 ms) and then echoes `arguments.text`.

`notifications/cancelled` is handled on the read loop so the server can prove
the client propagated cancellation. When `PIKE_MCP_TRACE` names a file, the
server appends one flushed line per observed event (`recv <method> ...`,
`hang-start id=<n>`, `stream id=<n>`, `cancelled requestId=<n>`), which the
tests poll to assert ordering.
"""

import json
import os
import sys
import time

TRACE = os.environ.get("PIKE_MCP_TRACE")

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
    {
        "name": "hang",
        "description": "Never respond; keep streaming after a cancellation.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "slow",
        "description": "Respond after a delay.",
        "inputSchema": {
            "type": "object",
            "properties": {"ms": {"type": "number"}, "text": {"type": "string"}},
        },
    },
]

PAGE_SIZE = 1

# Resources the fixture lists, in order. The first carries `_meta` and `icons`
# (stripped by the tools), the second is an MCP App UI (`ui://` plus a
# `profile=mcp-app` mime, filtered out), and the third has no `name` (defaulted
# from its `uri`). The list is paged one per page so client pagination is
# exercised.
RESOURCES = [
    {
        "uri": "file:///docs/readme.md",
        "name": "readme.md",
        "title": "Readme",
        "description": "Project readme",
        "mimeType": "text/markdown",
        "size": 42,
        "_meta": {"hidden": True},
        "icons": [{"src": "data:image/png;base64,AA=="}],
    },
    {
        "uri": "ui://widget/app.html",
        "name": "app widget",
        "mimeType": "text/html;profile=mcp-app",
    },
    {"uri": "notes://scratch"},
]

RESOURCE_TEMPLATES = [
    {
        "uriTemplate": "db://{table}/rows",
        "name": "table rows",
        "description": "Rows of a table",
        "mimeType": "application/json",
    },
    {"uriTemplate": "ui://widget/{id}", "mimeType": "text/html;profile=mcp-app"},
]

READ_RESULTS = {
    "file:///docs/readme.md": [
        {"uri": "file:///docs/readme.md", "mimeType": "text/markdown", "text": "# readme\n", "_meta": {"x": 1}},
    ],
    "blob://image": [
        {"uri": "blob://image", "mimeType": "image/png", "blob": "aGVsbG8="},
    ],
}


def trace(line):
    if not TRACE:
        return
    with open(TRACE, "a", encoding="utf-8") as handle:
        handle.write(line + "\n")
        handle.flush()


def send(message):
    sys.stdout.write(json.dumps(message, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def send_raw(text):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def notify(method, params):
    send({"jsonrpc": "2.0", "method": method, "params": params})


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


# Tool calls in flight, so a later cancellation can prove propagation.
hanging_requests = set()


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
    elif name == "hang":
        hanging_requests.add(rid)
        trace("hang-start id=" + str(rid))
    elif name == "slow":
        delay_ms = arguments.get("ms", 50)
        time.sleep(max(0.0, float(delay_ms)) / 1000.0)
        respond(rid, {"content": [{"type": "text", "text": str(arguments.get("text", ""))}]})
    else:
        respond_error(rid, -32602, "Unknown tool: " + str(name))


def handle_resources_list(rid, params):
    cursor = params.get("cursor")
    start = 0
    if cursor is not None:
        try:
            start = int(cursor)
        except (TypeError, ValueError):
            respond_error(rid, -32602, "invalid cursor")
            return
    result = {"resources": RESOURCES[start : start + PAGE_SIZE]}
    next_start = start + PAGE_SIZE
    if next_start < len(RESOURCES):
        result["nextCursor"] = str(next_start)
    respond(rid, result)


def handle_resource_templates_list(rid, params):
    cursor = params.get("cursor")
    start = 0
    if cursor is not None:
        try:
            start = int(cursor)
        except (TypeError, ValueError):
            respond_error(rid, -32602, "invalid cursor")
            return
    result = {"resourceTemplates": RESOURCE_TEMPLATES[start : start + PAGE_SIZE]}
    next_start = start + PAGE_SIZE
    if next_start < len(RESOURCE_TEMPLATES):
        result["nextCursor"] = str(next_start)
    respond(rid, result)


def handle_resource_read(rid, params):
    uri = params.get("uri")
    if uri not in READ_RESULTS:
        respond_error(rid, -32602, "Unknown resource: " + str(uri))
        return
    respond(rid, {"contents": READ_RESULTS[uri]})


def handle_cancelled(params):
    request_id = params.get("requestId")
    trace("cancelled requestId=" + str(request_id))
    if request_id in hanging_requests:
        # Acknowledge the cancellation but keep streaming: the client must
        # still surface the cancellation to its caller, not wait for a
        # response that never comes.
        for _ in range(3):
            notify("notifications/message", {"level": "info", "data": "still streaming"})
        trace("stream id=" + str(request_id))


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
        trace("recv " + str(method) + " id=" + str(rid))
        if rid is None:
            # `notifications/initialized`, `notifications/cancelled`, and other
            # notifications: no response, but cancellation is still observed.
            if method == "notifications/cancelled":
                handle_cancelled(message.get("params") or {})
            continue

        if method == "initialize":
            respond(
                rid,
                {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {"tools": {}, "resources": {"listChanged": True}},
                    "serverInfo": {"name": "pi-mcp-echo", "version": "1.0.0"},
                },
            )
        elif method == "tools/list":
            handle_tools_list(rid, message.get("params") or {})
        elif method == "tools/call":
            handle_tools_call(rid, message.get("params") or {})
        elif method == "resources/list":
            handle_resources_list(rid, message.get("params") or {})
        elif method == "resources/templates/list":
            handle_resource_templates_list(rid, message.get("params") or {})
        elif method == "resources/read":
            handle_resource_read(rid, message.get("params") or {})
        elif method == "ping":
            respond(rid, {})
        elif method == "debug/emit_garbage":
            send_raw("this is not json")
            respond(rid, {"after_garbage": True})
        elif method == "debug/emit_invalid_jsonrpc":
            send_raw('{"jsonrpc":"2.0"}')
            respond(rid, {"after_invalid": True})
        elif method == "debug/exit":
            respond(rid, {"exiting": True})
            sys.stdout.flush()
            os._exit(0)
        else:
            respond_error(rid, -32601, "Method not found: " + str(method))


if __name__ == "__main__":
    main()
