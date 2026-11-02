/**
 * A real stdio MCP child for the `audit-1191` capture.
 *
 * It is a newline-delimited JSON-RPC server (the protocol pi's `StdioTransport` speaks), not a
 * scripted transport: the capture launches it as a child process and observes the same wire the
 * production transport sees. Every mode writes its receipt to `AUDIT_1191_TRACE` synchronously.
 */

import { appendFileSync } from "node:fs";
import { createInterface } from "node:readline";

const mode = process.env.AUDIT_1191_MODE ?? "protocol";
const tracePath = process.env.AUDIT_1191_TRACE;
const protocolVersion = process.env.AUDIT_1191_PROTOCOL ?? "2025-06-18";
const stderrBytes = Number(process.env.AUDIT_1191_STDERR_BYTES ?? "80000");

const stderrHead = "AUDIT1191-START-";
const stderrTail = "-AUDIT1191-END";

function trace(entry) {
    if (tracePath) appendFileSync(tracePath, `${JSON.stringify(entry)}\n`);
}

function send(message) {
    process.stdout.write(`${JSON.stringify(message)}\n`);
}

function reply(id, result) {
    send({ jsonrpc: "2.0", id, result });
}

function replyError(id, code, message) {
    send({ jsonrpc: "2.0", id, error: { code, message } });
}

function progressTokenOf(params) {
    const meta = params && typeof params === "object" ? params._meta : undefined;
    return meta && typeof meta === "object" ? meta.progressToken : undefined;
}

if (mode === "stderr") {
    // A payload larger than the transport's 64 KiB ring. The head falls out of the ring, the tail
    // remains, so the capture can separate "keeps the last 64 KiB" from "keeps the whole stream".
    const fillerSize = Math.max(0, stderrBytes - stderrHead.length - stderrTail.length);
    const payload = `${stderrHead}${"s".repeat(fillerSize)}${stderrTail}`;
    process.stderr.write(payload, () => process.exit(1));
    await new Promise(() => {});
}

function progressTool(id, token) {
    const right = (progress, total, message) => {
        const params = { progressToken: token, progress };
        if (total !== undefined) params.total = total;
        if (message !== undefined) params.message = message;
        send({ jsonrpc: "2.0", method: "notifications/progress", params });
    };
    // A token that belongs to no pending request must not deliver or re-arm the deadline.
    setTimeout(() => {
        send({
            jsonrpc: "2.0",
            method: "notifications/progress",
            params: { progressToken: Number(token) + 1000, progress: 0 },
        });
    }, 10);
    setTimeout(() => right(1, 2, "halfway"), 40);
    setTimeout(() => right(2), 80);
    setTimeout(() => reply(id, { content: [{ type: "text", text: "done" }] }), 110);
    // After the request settles: delivered progress must be dropped.
    setTimeout(() => right(99), 170);
}

const lines = createInterface({ input: process.stdin });

for await (const line of lines) {
    if (!line.trim()) continue;
    let message;
    try {
        message = JSON.parse(line);
    } catch (error) {
        trace({ type: "invalid-json", line });
        continue;
    }
    const method = message.method;
    if (method === "initialize") {
        trace({ type: "request", method, id: message.id });
        reply(message.id, {
            protocolVersion,
            capabilities: { tools: {} },
            serverInfo: { name: "audit-1191-stdio", version: "1.0.0" },
        });
        continue;
    }
    if (method === "notifications/initialized") {
        trace({ type: "notification", method });
        continue;
    }
    if (method === "tools/list") {
        trace({ type: "request", method, id: message.id });
        reply(message.id, {
            tools: ["echo", "hang", "fail", "progress"].map((name) => ({
                name,
                inputSchema: { type: "object" },
            })),
        });
        continue;
    }
    if (method === "tools/call") {
        const name = message.params?.name;
        const token = progressTokenOf(message.params);
        trace({ type: "call", id: message.id, name, progressToken: token, params: message.params });
        if (name === "echo") {
            const text = message.params?.arguments?.text ?? "";
            reply(message.id, { content: [{ type: "text", text: `echo:${text}` }] });
        } else if (name === "fail") {
            replyError(message.id, -32000, "tool failed");
        } else if (name === "progress") {
            progressTool(message.id, token);
        } else if (name === "hang") {
            // Wait for notifications/cancelled or for the transport to close.
        } else {
            replyError(message.id, -32601, `Method not found: ${name}`);
        }
        continue;
    }
    if (method === "notifications/cancelled") {
        trace({
            type: "cancelled",
            requestId: message.params?.requestId,
            reason: message.params?.reason,
            params: message.params,
        });
        continue;
    }
    if (message.id !== undefined) {
        trace({ type: "unknown", method, id: message.id });
        replyError(message.id, -32601, `Method not found: ${method}`);
    }
}
