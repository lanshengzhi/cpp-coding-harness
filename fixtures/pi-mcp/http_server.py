#!/usr/bin/env python3
"""MCP streamable-http TLS test server (spec #865, ticket #873 fixture).

A self-contained HTTPS MCP server for the streamable-http transport. The
protocol shape mirrors pi v1.0.4
`packages/mcp/src/transports/streamable-http.ts` for the request path: one
JSON-RPC POST per request, an `application/json` or `text/event-stream`
response, and an `mcp-session-id` the client must echo on every request after
`initialize`.

`--cert`/`--key` secure the listener (the committed test PKI under
`tests/ai/providers/tls/`; SAN IP 127.0.0.1, DNS localhost). The server binds
an ephemeral loopback port and prints `PORT=<n>` on stdout so the test can
reach it. It is never reachable from outside the loopback interface.

Methods (`tools/list` reports one tool per page with `nextCursor`):

  initialize                  MCP handshake; issues `Mcp-Session-Id`
  notifications/initialized   notification; 202, no body
  tools/list                  echo | fail | sse_echo | hang | slow | crash
  tools/call echo             JSON response echoing arguments.text
  tools/call fail             JSON `isError: true` result
  tools/call sse_echo         response delivered over `text/event-stream`
  tools/call hang             never responds; holds the request open until cancelled
  tools/call slow             sleeps arguments.ms (default 50), then echoes arguments.text
  tools/call crash            exits the server without responding

When `PIKE_MCP_TRACE` names a file, the server appends one flushed line per
observed event, so a test can prove a request was genuinely in flight before it
cancels it.

Debug methods exercise the separation cases:

  debug/non_mcp_body          200 application/json that is not a JSON-RPC reply
  debug/http_status           500 with a plain error body
  debug/redirect              302 whose Location is a non-TLS `http://` target
  debug/json_rpc_error        a well-formed JSON-RPC error response
  debug/authorization_present JSON-RPC result carrying the received Authorization header
  debug/unauthorized          401 with a `WWW-Authenticate: Bearer` challenge
"""

import argparse
import json
import os
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SESSION_ID = "pike-test-session"
PAGE_SIZE = 1

# When `PIKE_MCP_TRACE` names a file, the server appends one flushed line per
# observed event, which the failure-isolation tests poll to assert ordering.
TRACE = os.environ.get("PIKE_MCP_TRACE")


def trace(line):
    if not TRACE:
        return
    with open(TRACE, "a", encoding="utf-8") as handle:
        handle.write(line + "\n")
        handle.flush()


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
        "name": "sse_echo",
        "description": "Echo the incoming text over an SSE response stream.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
    {
        "name": "hang",
        "description": "Never respond; hold the request open.",
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
    {
        "name": "crash",
        "description": "Exit the server without responding.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # keep the test output quiet
        pass

    def do_GET(self):
        # The fixture offers no server-to-client GET stream: 405 is the
        # transport's "feature absent" answer (pi `openSseStream`).
        trace("recv GET")
        self._raw(405, "text/plain", b"")

    def do_DELETE(self):
        # pi `close()` ends the session with a DELETE; acknowledge it.
        trace("recv DELETE")
        self._raw(200, "application/json", b"")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            message = json.loads(raw)
        except ValueError:
            self._raw(400, "application/json", b'{"error":"invalid json"}')
            return

        method = message.get("method")
        rid = message.get("id")
        trace("recv " + str(method) + " id=" + str(rid))
        if method == "notifications/cancelled":
            # A cancelled request must name the id and carry the pi reason
            # (`Aborted` / `Request timed out`); trace them so a test can diff
            # the emitted notification.
            trace("cancelled " + json.dumps(message.get("params") or {}, separators=(",", ":")))

        if method != "initialize" and self.headers.get("Mcp-Session-Id") != SESSION_ID:
            # The client must capture the session id from `initialize` and echo
            # it on every later request; a client that drops it fails here.
            self._raw(400, "application/json", b'{"error":"missing session id"}')
            return

        if rid is None:
            # Notifications (`notifications/initialized`) are acknowledged with
            # 202 and no reply body.
            self._raw(202, "application/json", b"")
            return

        handler = {
            "initialize": self._initialize,
            "tools/list": self._tools_list,
            "tools/call": self._tools_call,
            "debug/non_mcp_body": self._non_mcp_body,
            "debug/http_status": self._http_status,
            "debug/redirect": self._redirect,
            "debug/json_rpc_error": self._json_rpc_error,
            "debug/authorization_present": self._authorization_present,
            "debug/unauthorized": self._unauthorized,
        }.get(method)
        if handler is None:
            self._json(rid, error={"code": -32601, "message": "Method not found: " + str(method)})
            return
        handler(rid, message.get("params") or {})

    # --- MCP methods -------------------------------------------------------

    def _initialize(self, rid, _params):
        self._json(
            rid,
            result={
                "protocolVersion": "2025-06-18",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "pi-mcp-http-echo", "version": "1.0.0"},
            },
            session=SESSION_ID,
        )

    def _tools_list(self, rid, params):
        cursor = params.get("cursor")
        start = 0
        if cursor is not None:
            try:
                start = int(cursor)
            except (TypeError, ValueError):
                self._json(rid, error={"code": -32602, "message": "invalid cursor"})
                return
        result = {"tools": TOOLS[start : start + PAGE_SIZE]}
        next_start = start + PAGE_SIZE
        if next_start < len(TOOLS):
            result["nextCursor"] = str(next_start)
        self._json(rid, result=result)

    def _tools_call(self, rid, params):
        name = params.get("name")
        arguments = params.get("arguments") or {}
        if name == "echo":
            self._json(rid, result={"content": [{"type": "text", "text": str(arguments.get("text", ""))}]})
        elif name == "fail":
            self._json(
                rid,
                result={
                    "isError": True,
                    "content": [{"type": "text", "text": "fail: requested failure"}],
                },
            )
        elif name == "sse_echo":
            response = {
                "jsonrpc": "2.0",
                "id": rid,
                "result": {"content": [{"type": "text", "text": "sse:" + str(arguments.get("text", ""))}]},
            }
            body = ("event: message\ndata: " + json.dumps(response, separators=(",", ":")) + "\n\n").encode()
            self._raw(200, "text/event-stream", body)
        elif name == "hang":
            # Hold the request open: the client must cancel it rather than wait
            # for a response that never comes.
            trace("hang-start id=" + str(rid))
            time.sleep(3600)
        elif name == "slow":
            time.sleep(max(0.0, float(arguments.get("ms", 50))) / 1000.0)
            self._json(rid, result={"content": [{"type": "text", "text": str(arguments.get("text", ""))}]})
        elif name == "crash":
            # Exit without responding: the client observes the connection drop.
            os._exit(3)
        else:
            self._json(rid, error={"code": -32602, "message": "Unknown tool: " + str(name)})

    # --- separation cases --------------------------------------------------

    def _non_mcp_body(self, rid, _params):
        # 200 with a well-formed JSON body that is not a JSON-RPC reply.
        self._raw(200, "application/json", b'{"hello":"world"}')

    def _http_status(self, rid, _params):
        self._raw(500, "text/plain", b"boom")

    def _redirect(self, rid, _params):
        self.send_response(302)
        self.send_header("Location", "http://127.0.0.1:1/mcp")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _json_rpc_error(self, rid, _params):
        self._json(rid, error={"code": -32000, "message": "server exploded"})

    def _authorization_present(self, rid, _params):
        # Reports exactly what `Authorization` header reached the server, so a
        # client that attaches a credential only when the server needs one is
        # observable over the wire.
        self._json(rid, result={"authorization": self.headers.get("Authorization") or ""})

    def _unauthorized(self, rid, _params):
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Bearer error="invalid_token"')
        self.send_header("Content-Type", "application/json")
        body = b'{"error":"invalid_token"}'
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # --- helpers -----------------------------------------------------------

    def _json(self, rid, result=None, error=None, session=None):
        payload = {"jsonrpc": "2.0", "id": rid}
        if error is not None:
            payload["error"] = error
        else:
            payload["result"] = result if result is not None else {}
        self._raw(200, "application/json", json.dumps(payload, separators=(",", ":")).encode(), session)

    def _raw(self, status, content_type, body, session=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        if session is not None:
            self.send_header("Mcp-Session-Id", session)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    args = parser.parse_args()

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print("PORT=%d" % server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
