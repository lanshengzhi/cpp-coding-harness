import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, relative } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// The audit-1190 capture: the real frozen pi-v1.3.0 MCP client answers a
// fixture server's `ping`, `roots/list`, and an unhandled method over both
// transports. The observation is independent of the C++ implementation: the
// fixture records the client's response bytes and this script fails unless
// the response says what the scenario name claims.
const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const checkout = process.env.PI_CHECKOUT!;
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: checkout, encoding: "utf8" }).trim();
if (
    revision !== "71023aec1546d6d677a65669c5665c0927ce0dd0" ||
    execFileSync("git", ["status", "--porcelain"], { cwd: checkout, encoding: "utf8" }).trim()
)
    throw new Error("frozen checkout is not clean at baseline");
const load = (path: string) => import(pathToFileURL(join(checkout, path)).href);
const { McpClient } = await load("packages/mcp/src/client.ts");
const { StdioTransport } = await load("packages/mcp/src/transports/stdio.ts");
const { StreamableHttpTransport } = await load("packages/mcp/src/transports/streamable-http.ts");

const echoServer = join(repo, "fixtures/pi-mcp/echo_server.py");
const httpServer = join(repo, "fixtures/pi-mcp/http_server.py");
const roots = [{ uri: "file:///workspace", name: "workspace" }];
const requestIds = { ping: 101, rootsList: 102, unknown: 103 };
const unknownMethod = "sampling/createMessage";

const directory = mkdtempSync(join(tmpdir(), "pi-1190-"));
const tracePath = join(directory, "roots-trace.jsonl");

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

/** Poll the fixture trace until it holds one response per server request. */
async function waitForResponses(): Promise<Record<string, unknown>[]> {
    const deadline = Date.now() + 20_000;
    while (Date.now() < deadline) {
        let text = "";
        try {
            text = readFileSync(tracePath, "utf8");
        } catch {}
        const lines = text.split("\n").filter((line) => line.trim().length > 0);
        if (lines.length >= 3) return lines.map((line) => JSON.parse(line));
        await sleep(25);
    }
    throw new Error("fixture never recorded all three client responses");
}

function responseFor(responses: Record<string, any>[], id: number): Record<string, any> {
    const found = responses.find((response) => response.id === id);
    if (found === undefined) throw new Error(`fixture recorded no response for id ${id}`);
    return found;
}

function checkObservations(responses: Record<string, any>[]): {
    ping: Record<string, unknown>;
    rootsList: Record<string, unknown>;
    unknown: Record<string, unknown>;
} {
    const ping = responseFor(responses, requestIds.ping);
    const rootsList = responseFor(responses, requestIds.rootsList);
    const unknown = responseFor(responses, requestIds.unknown);
    assert.deepStrictEqual(ping, { jsonrpc: "2.0", id: requestIds.ping, result: {} });
    assert.deepStrictEqual(rootsList, { jsonrpc: "2.0", id: requestIds.rootsList, result: { roots } });
    assert.deepStrictEqual(unknown, {
        jsonrpc: "2.0",
        id: requestIds.unknown,
        error: { code: -32601, message: `Method not found: ${unknownMethod}` },
    });
    return { ping, rootsList, unknown };
}

async function captureStdio() {
    const transport = new StdioTransport({
        command: "python3",
        args: [echoServer],
        env: { PIKE_MCP_SERVER_REQUESTS: "roots", PIKE_MCP_ROOTS_TRACE: tracePath },
    });
    const client = new McpClient({ name: "pi", version: "1.3.0", requestTimeoutMs: 10_000, roots });
    await client.connect(transport);
    const responses = await waitForResponses();
    await client.close();
    return checkObservations(responses);
}

async function captureHttp() {
    const child = spawn("python3", [httpServer], {
        stdio: ["ignore", "pipe", "pipe"],
        env: { ...process.env, PIKE_MCP_SERVER_REQUESTS: "roots", PIKE_MCP_ROOTS_TRACE: tracePath },
    });
    try {
        const port = await new Promise<number>((resolve, reject) => {
            let buffer = "";
            child.stdout!.on("data", (chunk: Buffer) => {
                buffer += chunk.toString("utf8");
                const match = /^PORT=(\d+)\n/.exec(buffer);
                if (match) resolve(Number(match[1]));
            });
            child.once("exit", () => reject(new Error("HTTP fixture exited before reporting its port")));
        });
        const transport = new StreamableHttpTransport({ url: `http://127.0.0.1:${port}/mcp` });
        const client = new McpClient({ name: "pi", version: "1.3.0", requestTimeoutMs: 10_000, roots });
        await client.connect(transport);
        // The server-to-client stream opens after `notifications/initialized`.
        const responses = await waitForResponses();
        await client.close();
        return checkObservations(responses);
    } finally {
        child.kill("SIGTERM");
    }
}

const stdio = await captureStdio();
rmSync(tracePath, { force: true });
const http = await captureHttp();
rmSync(directory, { recursive: true, force: true });

const normalizations = [
    "ephemeral loopback ports and local paths are omitted",
    "responses are recorded verbatim from the fixture trace",
];
const scenarios = [
    {
        name: "a stdio server ping is answered with an empty result and roots/list returns the connection's configured roots",
        inputs: { transport: "stdio", roots, serverRequests: ["ping", "roots/list"] },
        expected: { ping: stdio.ping, rootsList: stdio.rootsList },
        normalizations,
    },
    {
        name: "a streamable HTTP server ping is answered with an empty result and roots/list returns the connection's configured roots",
        inputs: { transport: "streamable-http", roots, serverRequests: ["ping", "roots/list"] },
        expected: { ping: http.ping, rootsList: http.rootsList },
        normalizations,
    },
    {
        name: "a server request with no matching handler is answered -32601 before the connection closes",
        inputs: { transports: ["stdio", "streamable-http"], method: unknownMethod },
        expected: { stdio: stdio.unknown, streamableHttp: http.unknown },
        normalizations,
    },
];
const capturedAt = new Date().toISOString();
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: (process.report!.getReport() as any).header.glibcVersionRuntime,
    node: process.version,
    tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" })
        .split("\n")[0]
        .replace(/^tsx v?/, ""),
};
const endpoints = [
    "packages/mcp/src/client.ts:McpClient constructor,connect,handleRequest",
    "packages/mcp/src/transports/stdio.ts:StdioTransport",
    "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport,openGetStream",
    "packages/mcp/test/client.test.ts:answers roots/list and dispatches notifications",
];
const base = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: "pi-v1.3.0",
    revision,
    ticket: "1190",
    family: "audit-1190",
    capturedAt,
    environment,
};
const artifact = JSON.stringify({ ...base, sourceEndpoints: endpoints, scenarios }, null, 2) + "\n";
const hash = (value: string | Buffer) => createHash("sha256").update(value).digest("hex");
writeFileSync(join(dirname(script), "server-requests.json"), artifact);
const sources = [
    { path: relative(repo, script), sha256: hash(readFileSync(script)) },
    { path: relative(repo, echoServer), sha256: hash(readFileSync(echoServer)) },
    { path: relative(repo, httpServer), sha256: hash(readFileSync(httpServer)) },
];
const generator = {
    path: relative(repo, script),
    sha256: hash(readFileSync(script)),
    command:
        "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1190/capture-1190.mts",
    sources,
};
const manifest = {
    ...base,
    generator,
    artifacts: [
        {
            path: "server-requests.json",
            family: "audit-1190",
            bytes: Buffer.byteLength(artifact),
            sha256: hash(artifact),
            sourceEndpoints: endpoints,
            scenarios,
        },
    ],
};
writeFileSync(join(dirname(script), "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
