/**
 * Local fixtures for the `audit-1191` capture.
 *
 * Every HTTP fixture binds an ephemeral loopback port and needs no credential or provider catalog.
 * The stdio fixture launches the committed `stdio-server.mjs` as a real child process. The
 * fixtures record HTTP methods, JSON-RPC messages and stream lifecycle events rather than asserting
 * on client internals.
 */

import { createServer as createHttpServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import type { Socket } from "node:net";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { StdioTransport } from "./pi.mts";

export const STDIO_SERVER = path.join(path.dirname(fileURLToPath(import.meta.url)), "stdio-server.mjs");

export function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

export async function waitFor(
    predicate: () => boolean,
    timeoutMs = 5_000,
    label = "condition",
): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!predicate()) {
        if (Date.now() > deadline) throw new Error(`timed out waiting for ${label}`);
        await new Promise((resolve) => setTimeout(resolve, 10));
    }
}

export function readTrace(tracePath: string): any[] {
    if (!existsSync(tracePath)) return [];
    const text = readFileSync(tracePath, "utf8").trim();
    return text ? text.split("\n").map((line) => JSON.parse(line)) : [];
}

export interface StdioFixture {
    transport: InstanceType<typeof StdioTransport>;
    tracePath: string;
    directory: string;
    trace(): any[];
    cleanup(): Promise<void>;
}

export async function startStdioFixture(
    mode: string,
    environment: Record<string, string> = {},
    options: Record<string, unknown> = {},
): Promise<StdioFixture> {
    const directory = mkdtempSync(path.join(tmpdir(), "audit-1191-stdio-"));
    const tracePath = path.join(directory, "trace.jsonl");
    const transport = new StdioTransport({
        command: process.execPath,
        args: [STDIO_SERVER],
        env: { AUDIT_1191_MODE: mode, AUDIT_1191_TRACE: tracePath, ...environment },
        inheritEnv: false,
        ...options,
    });
    return {
        transport,
        tracePath,
        directory,
        trace: () => readTrace(tracePath),
        cleanup: async () => {
            await transport.close().catch(() => {});
            rmSync(directory, { recursive: true, force: true });
        },
    };
}

export interface RecordedHttpRequest {
    method: string;
    path: string;
    headers: Record<string, string | string[] | undefined>;
    message?: any;
    at: number;
}

export interface RecordedHttpEvent {
    type: string;
    index?: number;
    at: number;
}

export interface HttpMcpApi {
    fixture: HttpMcpFixture;
    writeSse(response: ServerResponse, message: unknown): void;
    writeSseRaw(response: ServerResponse, chunk: string): void;
}

export interface HttpMcpFixtureOptions {
    protocolVersion?: string;
    sessionId?: string;
    toolsList?: (cursor: string | undefined, index: number, message: any) => unknown;
    toolCall?: (message: any, response: ServerResponse, api: HttpMcpApi) => void | Promise<void>;
    getStream?: (index: number, request: IncomingMessage, response: ServerResponse, api: HttpMcpApi) => void | Promise<void>;
}

export interface HttpMcpFixture {
    origin: string;
    url: string;
    requests: RecordedHttpRequest[];
    events: RecordedHttpEvent[];
    getCount(): number;
    close(): Promise<void>;
}

function readBody(request: IncomingMessage): Promise<string> {
    return new Promise((resolve, reject) => {
        const chunks: Buffer[] = [];
        request.on("data", (chunk) => chunks.push(Buffer.from(chunk)));
        request.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
        request.on("error", reject);
    });
}

function sendJson(response: ServerResponse, status: number, body: unknown, headers: Record<string, string> = {}): void {
    response.writeHead(status, { "content-type": "application/json", ...headers }).end(JSON.stringify(body));
}

function replyResult(id: number, result: unknown): unknown {
    return { jsonrpc: "2.0", id, result };
}

function replyError(id: number, code: number, message: string): unknown {
    return { jsonrpc: "2.0", id, error: { code, message } };
}

export async function startHttpMcp(options: HttpMcpFixtureOptions = {}): Promise<HttpMcpFixture> {
    const requests: RecordedHttpRequest[] = [];
    const events: RecordedHttpEvent[] = [];
    const sockets = new Set<Socket>();
    const sessionId = options.sessionId ?? "session-1";
    const protocolVersion = options.protocolVersion ?? "2025-06-18";
    let gets = 0;
    let lists = 0;

    const fixture: HttpMcpFixture = {
        origin: "",
        url: "",
        requests,
        events,
        getCount: () => gets,
        close: async () => {
            for (const socket of sockets) socket.destroy();
            await new Promise<void>((resolve) => server.close(() => resolve()));
        },
    };

    const api: HttpMcpApi = {
        fixture,
        writeSse(response, message) {
            if (!response.writableEnded) response.write(`data: ${JSON.stringify(message)}\n\n`);
        },
        writeSseRaw(response, chunk) {
            if (!response.writableEnded) response.write(chunk);
        },
    };

    const handle = async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
        requests.push({
            method: request.method ?? "",
            path: request.url ?? "/",
            headers: request.headers,
            at: Date.now(),
        });
        const record = requests[requests.length - 1];
        if (request.method === "DELETE") {
            response.writeHead(200).end();
            return;
        }
        if (request.method === "GET") {
            gets += 1;
            const index = gets;
            request.on("close", () => events.push({ type: "get-close", index, at: Date.now() }));
            if (!options.getStream) {
                response.writeHead(405, { "content-type": "text/plain" }).end();
                return;
            }
            const openedAt = Date.now();
            events.push({ type: "get-open", index, at: openedAt });
            await options.getStream(index, request, response, api);
            return;
        }
        const body = await readBody(request);
        let message: any;
        try {
            message = JSON.parse(body);
        } catch {
            sendJson(response, 400, { error: "invalid json" });
            return;
        }
        record.message = message;
        if (message.method === "initialize") {
            sendJson(
                response,
                200,
                replyResult(message.id, {
                    protocolVersion,
                    capabilities: { tools: {} },
                    serverInfo: { name: "audit-1191-http", version: "1.0.0" },
                }),
                { "mcp-session-id": sessionId },
            );
            return;
        }
        if (message.method === "notifications/initialized") {
            response.writeHead(202).end();
            return;
        }
        if (message.id === undefined) {
            // A notification or a client answer to a server request.
            response.writeHead(202).end();
            return;
        }
        if (message.method === "tools/list") {
            lists += 1;
            const cursor = message.params?.cursor as string | undefined;
            const result = options.toolsList
                ? options.toolsList(cursor, lists, message)
                : { tools: [{ name: "echo", inputSchema: { type: "object" } }] };
            sendJson(response, 200, replyResult(message.id, result), { "mcp-session-id": sessionId });
            return;
        }
        if (message.method === "tools/call") {
            if (options.toolCall) {
                await options.toolCall(message, response, api);
                return;
            }
            sendJson(
                response,
                200,
                replyResult(message.id, { content: [{ type: "text", text: "ok" }] }),
                { "mcp-session-id": sessionId },
            );
            return;
        }
        sendJson(response, 200, replyError(message.id, -32601, `Method not found: ${message.method}`), {
            "mcp-session-id": sessionId,
        });
    };

    const server: Server = createHttpServer((request, response) => {
        void handle(request, response).catch((error) => {
            if (!response.writableEnded) sendJson(response, 500, { error: String(error) });
        });
    });
    server.on("connection", (socket) => {
        sockets.add(socket);
        socket.on("close", () => sockets.delete(socket));
    });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (address === null || typeof address === "string") throw new Error("HTTP fixture has no loopback port");
    fixture.origin = `http://127.0.0.1:${address.port}`;
    fixture.url = `${fixture.origin}/mcp`;
    return fixture;
}

export function errorRecord(error: unknown): Record<string, unknown> {
    if (!(error instanceof Error)) return { name: "NonError", message: String(error) };
    const record: Record<string, unknown> = {
        name: error.name,
        message: error.message.replace(/https?:\/\/127\.0\.0\.1:\d+/g, "<loopback>"),
    };
    const code = (error as { code?: unknown }).code;
    if (typeof code === "number") record.code = code;
    const status = (error as { status?: unknown }).status;
    if (typeof status === "number") record.status = status;
    const timeoutMs = (error as { timeoutMs?: unknown }).timeoutMs;
    if (typeof timeoutMs === "number") record.timeoutMs = timeoutMs;
    return record;
}

export function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

export function header(record: RecordedHttpRequest, name: string): string | undefined {
    const value = record.headers[name.toLowerCase()];
    return Array.isArray(value) ? value[0] : value;
}
