#!/usr/bin/env python3
"""Local MCP resource isolation fixture for #1105; the trace records wire order."""

import json
import sys

trace_path = sys.argv[1]
fail_resources = sys.argv[2] == "fail"
change_tools = sys.argv[2] in {"change", "hold-change"}
changed = False

for line in sys.stdin:
    message = json.loads(line)
    method = message.get("method")
    with open(trace_path, "a", encoding="utf-8") as trace:
        trace.write(method + "\n")
    if "id" not in message:
        continue
    response = {"jsonrpc": "2.0", "id": message["id"]}
    if method == "initialize":
        result = {
            "protocolVersion": "2025-11-25",
            "capabilities": {"tools": {}, "resources": {}},
            "serverInfo": {"name": "resources", "version": "1"},
        }
    elif method == "tools/list":
        if changed and sys.argv[2] == "hold-change":
            continue  # Remain responsive to EOF while discovery is unanswered.
        names = ["late", "echo"] if changed else ["echo"]
        field = "message" if changed else "text"
        schema = {"type": "object"}
        if change_tools:
            schema.update({"additionalProperties": False, "required": [field],
                           "properties": {field: {"type": "string"}}})
        result = {"tools": [{"name": name, "description": "updated" if changed else "original",
                             "inputSchema": schema} for name in names]}
    elif method == "resources/list" and fail_resources:
        response["error"] = {"code": -32603, "message": "resource boom"}
        result = None
    elif method == "resources/list":
        result = {"resources": [{"uri": "docs://intro", "name": "intro"}]}
    elif method == "resources/templates/list":
        result = {"resourceTemplates": []}
    elif method == "tools/call":
        text = "echo after resources"
        if change_tools:
            params = message["params"]
            arguments = params.get("arguments", {})
            text = params["name"] + ":" + arguments.get("message", arguments.get("text", ""))
        result = {"content": [{"type": "text", "text": text}]}
    else:
        result = {}
    if "error" not in response:
        response["result"] = result
    print(json.dumps(response), flush=True)
    if method == "tools/call" and change_tools and not changed:
        changed = True
        print(json.dumps({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}), flush=True)
