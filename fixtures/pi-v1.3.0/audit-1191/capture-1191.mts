/**
 * `audit-1191` capture: source-only MCP protocol observations against a private frozen pi v1.3.0
 * checkout.
 *
 * The five `cch_mcp` families captured here (`mcp-tools-list`, `mcp-progress`,
 * `mcp-request-cancellation`, `mcp-http-session-stream`, `mcp-stdio-stderr-tail`) had delivered C++
 * implementations and regression cases but no frozen observation. This capture records the missing
 * observations independently: real loopback HTTP fixtures and a real stdio child, never a C++
 * expectation and never a scripted transport where the row is about transport behavior.
 *
 * The script refuses a checkout whose HEAD is not the registered baseline revision, a modified
 * source tree, a non-Linux/glibc host, and a family already registered. It re-executes itself under
 * the checkout's tsx so the recorded `tsx` version belongs to the same install.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1080-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1080-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1191/capture-1191.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1191";
const TICKET = "1191";
const BASELINE = process.env.PI_BASELINE ?? "pi-v1.3.0";
const CHECKOUT = process.env.PI_CHECKOUT;
const EXPECTED_REVISION = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const TSX = CHECKOUT ? join(CHECKOUT, "node_modules/.bin/tsx") : "";

if (!CHECKOUT) {
    console.error("PI_CHECKOUT must name the private frozen pi v1.3.0 checkout");
    process.exit(1);
}
if (!existsSync(TSX)) {
    console.error(`the checkout has no tsx: ${TSX}`);
    process.exit(1);
}

function run(command: string, arguments_: string[], cwd = REPO): string {
    return execFileSync(command, arguments_, { cwd, encoding: "utf8" }).trim();
}

function sha256(content: Buffer | string): string {
    return createHash("sha256").update(content).digest("hex");
}

if (!process.env.AUDIT_1191_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1191_TSX: "1",
            NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
            TSX_TSCONFIG_PATH: join(CHECKOUT, "tsconfig.json"),
        },
        stdio: "inherit",
    });
    process.exit(0);
}

const revision = run("git", ["rev-parse", "HEAD"], CHECKOUT);
if (revision !== EXPECTED_REVISION) {
    console.error(`checkout revision ${revision} is not ${EXPECTED_REVISION}`);
    process.exit(1);
}
const dirty = run("git", ["status", "--porcelain"], CHECKOUT);
if (dirty !== "") {
    console.error(`checkout is not clean:\n${dirty}`);
    process.exit(1);
}
if (process.platform !== "linux" || process.arch !== "x64") {
    console.error(`unsupported platform ${process.platform}-${process.arch}`);
    process.exit(1);
}
const report = process.report?.getReport() as { header?: { glibcVersionRuntime?: string } } | undefined;
if (!report?.header?.glibcVersionRuntime) {
    console.error("the captured environment is not glibc Linux");
    process.exit(1);
}

const registryPath = join(REPO, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { bundles?: Record<string, { manifest?: string }> }>;
};
const registered = registry.baselines[BASELINE]?.bundles?.[FAMILY];
if (registered) {
    console.error(`${FAMILY} is already registered: ${registered.manifest}`);
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/mcp/src/client.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/mcp/src/client.ts is missing");
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/coding-agent/src/extensions/mcp/runtime.ts"))) {
    console.error("the checkout is not pi v1.3.0: the MCP application runtime is missing");
    process.exit(1);
}

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

const {
    alreadyCancelledRequest,
    applicationToolNameAssignment,
    failedRequestNoCancellation,
    httpCancellation,
    httpCancelsServerRequest,
    httpCloseAbortsStream,
    httpGetStreamNotification,
    httpOutOfRangeRetryHint,
    httpPingAndRoots,
    httpProgressDelivery,
    httpProgressRearmsDeadline,
    httpRetryHint,
    httpSessionHeadersAnd405,
    httpTimeoutReason,
    progressText,
    stdioCancellation,
    stdioFailedRequestNoCancellation,
    stdioFailureTail,
    stdioProgressDelivery,
    stdioProgressRearmsDeadline,
    stdioStderrRing,
    stdioTimeoutReason,
    toolNameSanitization,
    toolsListDuplicateCursor,
    toolsListEmptyCursor,
    toolsListFollowsCursor,
    toolsListNullCursor,
    toolsListPageBound,
} = await import("./scenarios.mts");

interface ArtifactDefinition {
    path: string;
    sourceEndpoints: string[];
    scenarios: () => Promise<any[]>;
    normalizations: string[];
}

const COMMON_NORMALIZATIONS = [
    "ephemeral loopback ports, stdio child pids and temporary directories are omitted or replaced by placeholders",
    "elapsed times are recorded as measured millisecond values with a bounded assertion, never as an exact schedule",
];

const ARTIFACTS: ArtifactDefinition[] = [
    {
        path: "mcp-tools-list.json",
        sourceEndpoints: [
            "packages/mcp/src/client.ts:McpClient.listTools,listAll,listPage,validateListPage,MAX_LIST_PAGES",
            "packages/coding-agent/src/extensions/mcp/tools.ts:createMcpToolName,MAX_TOOL_NAME_LENGTH",
            "packages/coding-agent/src/extensions/mcp/index.ts:registerTools",
            "packages/mcp/test/client.test.ts:paginates tools and preserves protocol tool definitions",
            "packages/coding-agent/test/mcp-extension.test.ts:creates provider-safe tool names",
        ],
        scenarios: async () => [
            await toolsListFollowsCursor(),
            await toolsListNullCursor(),
            await toolsListEmptyCursor(),
            await toolsListDuplicateCursor(),
            await toolsListPageBound(),
            await toolNameSanitization(),
            await applicationToolNameAssignment(),
        ],
        normalizations: [...COMMON_NORMALIZATIONS, "tool listings are compared by name and cursor sequence"],
    },
    {
        path: "mcp-progress.json",
        sourceEndpoints: [
            "packages/mcp/src/client.ts:McpClient.requestInternal,handleProgress,armTimeout",
            "packages/mcp/src/transports/stdio.ts:StdioTransport",
            "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport,consumeSseStream",
            "packages/coding-agent/src/extensions/mcp/tools.ts:createMcpToolDefinition.execute,onProgress",
            "packages/mcp/test/client.test.ts:renews the timeout on progress",
        ],
        scenarios: async () => [
            await stdioProgressDelivery(),
            await httpProgressDelivery(),
            await stdioProgressRearmsDeadline(),
            await httpProgressRearmsDeadline(),
            await progressText(),
        ],
        normalizations: [...COMMON_NORMALIZATIONS, "progress tokens are compared to the JSON-RPC request id"],
    },
    {
        path: "mcp-request-cancellation.json",
        sourceEndpoints: [
            "packages/mcp/src/client.ts:McpClient.requestInternal,cancelPending,armTimeout,handleResponse",
            "packages/mcp/src/protocol/jsonrpc.ts:McpAbortError,McpTimeoutError",
            "packages/mcp/src/transports/stdio.ts:StdioTransport.send",
            "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport.send",
            "packages/mcp/test/client.test.ts:cancels aborted and timed-out requests",
            "packages/mcp/test/client.test.ts:does not send notifications/cancelled for a timed-out initialize",
        ],
        scenarios: async () => [
            await stdioCancellation(),
            await httpCancellation(),
            await stdioTimeoutReason(),
            await httpTimeoutReason(),
            await alreadyCancelledRequest(),
            await failedRequestNoCancellation(),
            await stdioFailedRequestNoCancellation(),
        ],
        normalizations: [
            ...COMMON_NORMALIZATIONS,
            "the caller's abort reason is the literal token passed to AbortController.abort",
        ],
    },
    {
        path: "mcp-http-session-stream.json",
        sourceEndpoints: [
            "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport.start,setProtocolVersion,send,captureSession,buildHeaders,openSseStream,runGetStream,consumeSse,close",
            "packages/mcp/src/client.ts:McpClient.handleMessage,handleRequest,handleCancelled,handleNotification",
            "packages/mcp/test/streamable-http.test.ts:handles JSON and SSE responses with session and protocol headers",
            "packages/mcp/test/streamable-http.test.ts:opens the GET stream after initialization and sends Last-Event-ID only when resuming",
            "packages/mcp/test/streamable-http.test.ts:reconnects the GET stream after it drops",
            "packages/mcp/test/client.test.ts:answers roots/list and dispatches notifications",
        ],
        scenarios: async () => [
            await httpSessionHeadersAnd405(),
            await httpRetryHint(),
            await httpOutOfRangeRetryHint(),
            await httpGetStreamNotification(),
            await httpPingAndRoots(),
            await httpCancelsServerRequest(),
            await httpCloseAbortsStream(),
        ],
        normalizations: [
            ...COMMON_NORMALIZATIONS,
            "a server-controlled retry value above the runtime timer range is recorded as the measured bounded reconnect gap",
        ],
    },
    {
        path: "mcp-stdio-stderr-tail.json",
        sourceEndpoints: [
            "packages/mcp/src/transports/stdio.ts:StdioTransport.handleStderr,DEFAULT_MAX_STDERR_BYTES",
            "packages/coding-agent/src/extensions/mcp/runtime.ts:McpServerConnection.connectOnce,connectFailed,STDERR_TAIL_CHARS",
            "packages/mcp/test/stdio.test.ts:connects to a newline-delimited MCP server and captures stderr",
        ],
        scenarios: async () => [await stdioStderrRing(), await stdioFailureTail()],
        normalizations: [
            ...COMMON_NORMALIZATIONS,
            "the synthetic stderr payload is generated deterministically from its declared byte count",
        ],
    },
];

/** Fixture inputs for each scenario; the capture itself is the executable description. */
function scenarioInputs(name: string): Record<string, unknown> {
    const inputs: Record<string, Record<string, unknown>> = {
        "tools/list follows nextCursor across pages and ends on an empty cursor": {
            pages: [
                { cursor: null, tools: ["search"], nextCursor: "page-2" },
                { cursor: "page-2", tools: ["read"], nextCursor: "" },
            ],
            transport: "loopback HTTP",
        },
        "a null nextCursor ends tools/list without another request": {
            pages: [{ cursor: null, tools: ["only"], nextCursor: null }],
            transport: "loopback HTTP",
        },
        "an empty nextCursor ends tools/list without another request": {
            pages: [{ cursor: null, tools: ["only"], nextCursor: "" }],
            transport: "loopback HTTP",
        },
        "a repeated nextCursor is an explicit duplicate-cursor error": {
            pages: "every page returns nextCursor loop",
            transport: "loopback HTTP",
        },
        "tools/list stops after pi's 1000-page bound": {
            pages: "each page returns a new page-<n> cursor",
            maxPages: 1000,
            transport: "loopback HTTP",
        },
        "application tool names hash every colliding sanitized name order-independently": {
            tools: ["a-b", "a_b"],
            plainSanitized: "mcp__srv__a_b",
            listOrders: ["a-b then a_b", "a_b then a-b"],
        },
        "application tool names sanitize punctuation, bound at 64 and hash collisions from the raw pair": {
            cases: [
                "punctuation outside [A-Za-z0-9_] becomes _",
                "a 64-character sanitized name stays plain",
                "a longer sanitized name is cut to 64 with an 8-hex sha256 suffix",
                "a taken plain name takes the raw-pair hash suffix",
                "distinct short names stay unchanged",
            ],
        },
        "stdio: onProgress injects _meta.progressToken and delivers only matching progress": {
            transport: "stdio child",
            tool: "progress",
            notifications: [
                { delayMs: 10, progressToken: "request id + 1000", progress: 0 },
                { delayMs: 40, progressToken: "request id", progress: 1, total: 2, message: "halfway" },
                { delayMs: 80, progressToken: "request id", progress: 2 },
                { delayMs: 170, progressToken: "request id", progress: 99, afterSettle: true },
            ],
        },
        "HTTP: onProgress injects _meta.progressToken and delivers only matching progress": {
            transport: "loopback HTTP SSE",
            tool: "progress",
            notifications: [
                { delayMs: 10, progressToken: "request id + 1000", progress: 0 },
                { delayMs: 40, progressToken: "request id", progress: 1, total: 2, message: "halfway" },
                { delayMs: 80, progressToken: "request id", progress: 2 },
                { delayMs: 170, progressToken: "request id", progress: 99, afterSettle: true },
            ],
        },
        "stdio: a progress notification re-arms the request deadline": {
            transport: "stdio child",
            timeoutMs: 70,
            progressBeforeResultMs: [40, 80],
            resultAtMs: 110,
        },
        "HTTP: a progress notification re-arms the request deadline": {
            transport: "loopback HTTP SSE",
            timeoutMs: 70,
            progressBeforeResultMs: [40, 80],
            resultAtMs: 110,
        },
        "application progress text uses the message verbatim or Progress <n>[/<total>]": {
            cases: [
                { message: "custom message", progress: 1, total: 5 },
                { progress: 2, total: 10 },
                { progress: 3 },
                { message: "", progress: 4 },
            ],
        },
        "stdio: aborting an in-flight call notifies the server and leaves the child usable": {
            transport: "stdio child",
            tool: "hang",
            abortReason: "stop",
            reuseTool: "echo",
        },
        "HTTP: aborting an in-flight call posts notifications/cancelled and reuses the session": {
            transport: "loopback HTTP",
            tool: "hang",
            abortReason: "stop",
            reuseTool: "echo",
        },
        "stdio: a timed-out request fails with pi's timeout error and reason": {
            transport: "stdio child",
            tool: "hang",
            timeoutMs: 80,
        },
        "HTTP: a timed-out request fails with pi's timeout error and reason": {
            transport: "loopback HTTP",
            tool: "hang",
            timeoutMs: 80,
        },
        "an already-aborted request is never sent and emits no notifications/cancelled": {
            transport: "loopback HTTP",
            signal: "already aborted before request",
        },
        "a failed HTTP request does not emit notifications/cancelled": {
            transport: "loopback HTTP",
            responseStatus: 500,
        },
        "a failed stdio request does not emit notifications/cancelled": {
            transport: "stdio child",
            response: "JSON-RPC error -32000",
            abortAfterSettle: true,
        },
        "HTTP: session headers carry through and a 405 GET is feature-absent": {
            transport: "loopback HTTP",
            getStatus: 405,
            close: "DELETE with session id",
        },
        "HTTP: a server retry hint overrides the client reconnect delay and resumes with Last-Event-ID": {
            retryField: 20,
            clientBackoffMs: 500,
            firstGet: "priming id 1 then end",
            secondGet: "resume with Last-Event-ID and hold open",
        },
        "HTTP: an out-of-range retry hint reconnects without throwing or hanging": {
            retryField: "99999999999999999999999999",
            clientBackoffMs: 20,
            firstGet: "priming id 1 then end",
            secondGet: "resume with Last-Event-ID and hold open",
        },
        "HTTP: an event on the server-to-client GET stream reaches the notification listener": {
            transport: "loopback HTTP GET SSE",
            event: "notifications/message",
        },
        "HTTP: the client answers ping and roots/list requests delivered on the GET stream": {
            transport: "loopback HTTP GET SSE",
            serverRequests: [
                { id: 7, method: "ping" },
                { id: 8, method: "roots/list" },
            ],
            configuredRoots: [{ uri: "file:///workspace", name: "workspace" }],
        },
        "HTTP: notifications/cancelled aborts an in-flight server request handler": {
            transport: "loopback HTTP GET SSE",
            serverRequest: { id: 9, method: "test/wait" },
            cancellationDelayMs: 80,
        },
        "HTTP: close aborts the GET stream without a dropped-stream error": {
            transport: "loopback HTTP GET SSE",
            close: "client.close() sends DELETE",
        },
        "stdio: the transport ring keeps the last 64 KiB of a larger stderr stream": {
            transport: "stdio child",
            stderrBytes: 80_000,
            ringBytes: 65_536,
        },
        "stdio: a failed connect appends pi's bounded 2000-character stderr tail": {
            transport: "stdio child through McpServerConnection",
            stderrBytes: 80_000,
            tailCharacters: 2_000,
        },
    };
    return inputs[name] ?? { fixture: "local loopback" };
}

const capturedAt = new Date().toISOString();
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};

const pinnedSources = [
    SCRIPT,
    join(artifactDirectory, "scenarios.mts"),
    join(artifactDirectory, "fixtures.mts"),
    join(artifactDirectory, "pi.mts"),
    join(artifactDirectory, "stdio-server.mjs"),
].map((path) => ({ path: relative(REPO, path), sha256: sha256(readFileSync(path)) }));

const manifestArtifacts: Array<Record<string, unknown>> = [];
for (const definition of ARTIFACTS) {
    console.error(`START ${definition.path}`);
    const scenarios = await definition.scenarios();
    console.error(`DONE ${definition.path}`);
    const names = new Set<string>();
    for (const scenario of scenarios) {
        if (!scenario.name.trim() || !scenario.source.trim() || scenario.expected === undefined) {
            throw new Error(`${definition.path}: a scenario needs a name, source and expected value`);
        }
        if (names.has(scenario.name)) throw new Error(`${definition.path}: duplicate scenario ${scenario.name}`);
        names.add(scenario.name);
    }
    const records = scenarios.map((scenario) => ({
        name: scenario.name,
        inputs: scenarioInputs(scenario.name),
        expected: scenario.expected,
        normalizations: definition.normalizations,
    }));
    const artifact = {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: BASELINE,
        revision,
        ticket: TICKET,
        family: FAMILY,
        capturedAt,
        environment,
        sourceEndpoints: definition.sourceEndpoints,
        scenarios: records,
    };
    const content = `${JSON.stringify(artifact, null, 2)}\n`;
    writeFileSync(join(artifactDirectory, definition.path), content);
    manifestArtifacts.push({
        path: definition.path,
        family: FAMILY,
        bytes: Buffer.byteLength(content),
        sha256: sha256(content),
        sourceEndpoints: definition.sourceEndpoints,
        scenarios: records,
    });
    console.log(`captured ${definition.path} (${scenarios.length} scenarios)`);
}

const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    generator: {
        path: relative(REPO, SCRIPT),
        sha256: sha256(readFileSync(SCRIPT)),
        command:
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1191/capture-1191.mts",
        sources: pinnedSources,
    },
    artifacts: manifestArtifacts,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
