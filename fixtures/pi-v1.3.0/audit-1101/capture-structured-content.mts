/**
 * `audit-1101` capture: `tools/call` JSON result preservation and application conversion against a
 * private frozen pi v1.3.0 checkout. The script refuses to write evidence unless the checkout is the
 * exact baseline revision and its tracked source is clean. It re-executes itself under the checkout's
 * tsx so the recorded `tsx` version belongs to the same install.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1050-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1050-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1101/capture-structured-content.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { convertMcpResult, createInMemoryTransportPair, isJsonRpcRequest, McpClient } from "./pi.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1101";
const TICKET = "1101";
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

if (!process.env.AUDIT_1101_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1101_TSX: "1",
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
if (!existsSync(join(CHECKOUT, "packages/mcp/src/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/mcp/src/index.ts is missing");
    process.exit(1);
}

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

interface Scenario {
    name: string;
    inputs: Record<string, unknown>;
    expected: unknown;
}

function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

function rpcResult(id: number, result: unknown): unknown {
    return { jsonrpc: "2.0", id, result };
}

async function startScriptedClient(
    handler: (message: { id: number; method: string; params?: unknown }) => unknown,
): Promise<{ client: McpClient; close: () => Promise<void> }> {
    const pair = createInMemoryTransportPair();
    await pair.server.start();
    pair.server.onMessage((message: unknown) => {
        if (!isJsonRpcRequest(message)) return;
        if (message.method === "initialize") {
            void pair.server.send(
                rpcResult(message.id, {
                    protocolVersion: "2025-11-25",
                    capabilities: { tools: {} },
                    serverInfo: { name: "scripted", version: "1.0.0" },
                }),
            );
            return;
        }
        const response = handler(message as { id: number; method: string; params?: unknown });
        if (response !== undefined) void pair.server.send(response);
    });
    const client = new McpClient({ name: "pi", version: "1.3.0", roots: [] });
    await client.connect(pair.client);
    return { client, close: () => client.close() };
}

function observedClientResult(result: Record<string, unknown>): Record<string, unknown> {
    return {
        content: result.content,
        ...(Object.hasOwn(result, "structuredContent") ? { structuredContent: result.structuredContent } : {}),
    };
}

function observedConversion(result: Record<string, unknown>): Promise<Record<string, unknown>> {
    return convertMcpResult("structured", "structured", result as never).then((converted) => ({
        content: converted.content,
        structuredContent: converted.structuredContent,
        isError: converted.isError ?? null,
    }));
}

/** One normal `content` absent result observed through `callTool` and `convertMcpResult`. */
async function preservedScenario(
    name: string,
    tool: string,
    result: Record<string, unknown>,
): Promise<Scenario> {
    const { client, close } = await startScriptedClient((message) =>
        message.method === "tools/call" ? rpcResult(message.id, result) : rpcResult(message.id, { tools: [] }),
    );
    try {
        const called = (await client.callTool(tool)) as unknown as Record<string, unknown>;
        const converted = await observedConversion(called);
        return {
            name,
            inputs: { tool, result },
            expected: {
                client: observedClientResult(called),
                converted,
            },
        };
    } finally {
        await close();
    }
}

/** One malformed result observed through the public client `callTool` validation. */
async function rejectedScenario(
    name: string,
    tool: string,
    result: unknown,
): Promise<Scenario> {
    const { client, close } = await startScriptedClient((message) =>
        message.method === "tools/call" ? rpcResult(message.id, result) : rpcResult(message.id, { tools: [] }),
    );
    try {
        let failure: Record<string, unknown> | undefined;
        try {
            await client.callTool(tool);
        } catch (error) {
            const typed = error as { name?: string; message?: string; code?: number };
            failure = { name: typed.name, message: typed.message, code: typed.code };
        }
        assert(failure !== undefined, "the malformed result must be rejected");
        return {
            name,
            inputs: { tool, result },
            expected: { error: failure },
        };
    } finally {
        await close();
    }
}

const presenceScenarios: Scenario[] = [
    await preservedScenario(
        "client and conversion preserve an object structuredContent when content is absent",
        "object",
        { structuredContent: { nested: [true, null, "x"], ok: true } },
    ),
    await preservedScenario(
        "client and conversion preserve a non-empty array structuredContent when content is absent",
        "array",
        { structuredContent: [1, { x: true }, null] },
    ),
    await preservedScenario(
        "client and conversion preserve an empty-array structuredContent when content is absent",
        "empty-array",
        { structuredContent: [] },
    ),
    await preservedScenario(
        "client and conversion preserve a string structuredContent when content is absent",
        "string",
        { structuredContent: "text" },
    ),
    await preservedScenario(
        "client and conversion preserve an empty-string structuredContent when content is absent",
        "empty-string",
        { structuredContent: "" },
    ),
    await preservedScenario(
        "client and conversion preserve a false structuredContent when content is absent",
        "false",
        { structuredContent: false },
    ),
    await preservedScenario(
        "client and conversion preserve a number-zero structuredContent when content is absent",
        "zero",
        { structuredContent: 0 },
    ),
    await preservedScenario(
        "client and conversion preserve a null structuredContent when content is absent",
        "null",
        { structuredContent: null },
    ),
    await preservedScenario(
        "client and conversion distinguish missing structuredContent from JSON null when content is absent",
        "missing",
        {},
    ),
    await rejectedScenario(
        "client rejects JSON null content instead of treating it as absent",
        "null-content",
        { content: null },
    ),
    await rejectedScenario(
        "client rejects non-array content instead of treating it as absent",
        "string-content",
        { content: "not a list" },
    ),
    await rejectedScenario(
        "client rejects a non-object tools/call result",
        "malformed",
        [],
    ),
];
// Wire-order matrix (#1101 review): the presence matrix above answers whether
// `structuredContent` survives at all; these answer what its JSON TEXT looks
// like, which is the defect #1101 actually fixed - C++ used std::map and
// sorted members, so the rendered text disagreed with pi's insertion order.
const orderScenarios: Scenario[] = [
    await preservedScenario(
        "client and conversion preserve the wire order of a reverse object structuredContent",
        "object-wire",
        { structuredContent: { ok: true, nested: [true, null, "x"] } },
    ),
    await preservedScenario(
        "client and conversion preserve insertion order for three reverse-sorted object members",
        "object-reverse",
        { structuredContent: { z: "last", a: "first", m: "middle" } },
    ),
    await preservedScenario(
        "client and conversion preserve order in nested objects and their parent",
        "object-nested",
        { structuredContent: { outer: { z: 1, a: 2 }, first: true } },
    ),
    await preservedScenario(
        "client and conversion preserve JavaScript integer-like key ordering",
        "object-integer-keys",
        { structuredContent: { "10": "ten", "2": "two", b: true, a: false } },
    ),
    await preservedScenario(
        "client and conversion render numbers with JavaScript JSON.stringify notation",
        "object-numbers",
        {
            structuredContent: {
                integer: 42,
                negative: -7,
                float: 1.5,
                large: 9007199254740993,
                exponent: 1e21,
            },
        },
    ),
    await preservedScenario(
        "client and conversion render every JSON number shape with JavaScript JSON.stringify notation",
        "object-number-shapes",
        {
            structuredContent: {
                zero: 0,
                negativeZero: -0,
                small: 0.000001,
                smaller: 1e-7,
                large: 1e20,
                exponent: 1e21,
                fraction: 123.45,
                precise: 1.2345678901234567e30,
            },
        },
    ),
    await preservedScenario(
        "client and conversion keep non-empty content blocks and structuredContent separate",
        "content-text-structured",
        { content: [{ type: "text", text: "alpha" }], structuredContent: { ok: true } },
    ),
    await preservedScenario(
        "client and conversion fall back to structuredContent for an empty content list",
        "empty-content-structured",
        { content: [], structuredContent: { ok: true } },
    ),
    await preservedScenario(
        "client and conversion add pi's error text when missing content isError has no model text",
        "missing-content-isError",
        { isError: true },
    ),
];

const capturedAt = new Date().toISOString();
const sourceEndpoints = [
    "packages/mcp/src/client.ts:McpClient.callTool,validateCallToolResult",
    "packages/mcp/src/protocol/content.ts:CallToolResult,toLlmContent",
    "packages/coding-agent/src/extensions/mcp/tools.ts:convertMcpResult,createMcpResultSchema",
    "packages/mcp/test/client.test.ts:defaults missing tool result content to an empty list",
    "packages/mcp/test/client.test.ts:accepts structured content that is not an object",
    "packages/mcp/test/content.test.ts:falls back to the structured content as JSON when there are no blocks",
    "packages/coding-agent/test/mcp-extension.test.ts:converts results, passing the CallToolResult to scripts and flagging errors",
];
const normalizations = [
    "ephemeral loopback ports and local paths are omitted",
    "elapsed times and errors are reported as bounded values or names",
    "object member order is captured from the JavaScript value as JSON.stringify observes it; integer-like keys sort ascending, other keys keep insertion order",
];
const orderSourceEndpoints = [
    "packages/mcp/src/client.ts:McpClient.callTool,validateCallToolResult",
    "packages/mcp/src/protocol/content.ts:CallToolResult,toLlmContent",
    "packages/coding-agent/src/extensions/mcp/tools.ts:convertMcpResult,createMcpResultSchema",
];
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};
function buildArtifact(scenarios: Scenario[], endpoints: string[]) {
    return {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: BASELINE,
        revision,
        ticket: TICKET,
        family: FAMILY,
        capturedAt,
        environment,
        sourceEndpoints: endpoints,
        scenarios: scenarios.map((scenario) => ({
            name: scenario.name,
            inputs: scenario.inputs,
            expected: scenario.expected,
            normalizations,
        })),
    };
}

const presenceArtifact = buildArtifact(presenceScenarios, sourceEndpoints);
const presenceName = "structured-content.json";
const presenceContent = `${JSON.stringify(presenceArtifact, null, 2)}\n`;
writeFileSync(join(artifactDirectory, presenceName), presenceContent);

const orderArtifact = buildArtifact(orderScenarios, orderSourceEndpoints);
const orderName = "structured-content-order.json";
const orderContent = `${JSON.stringify(orderArtifact, null, 2)}\n`;
writeFileSync(join(artifactDirectory, orderName), orderContent);

const pinnedSources = [SCRIPT, join(artifactDirectory, "pi.mts")].map((path) => ({
    path: relative(REPO, path),
    sha256: sha256(readFileSync(path)),
}));
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
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1101/capture-structured-content.mts",
        sources: pinnedSources,
    },
    artifacts: [
        {
            path: presenceName,
            family: FAMILY,
            bytes: Buffer.byteLength(presenceContent),
            sha256: sha256(presenceContent),
            sourceEndpoints,
            scenarios: presenceArtifact.scenarios,
        },
        {
            path: orderName,
            family: FAMILY,
            bytes: Buffer.byteLength(orderContent),
            sha256: sha256(orderContent),
            sourceEndpoints: orderSourceEndpoints,
            scenarios: orderArtifact.scenarios,
        },
    ],
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(
    `captured ${presenceName} (${presenceArtifact.scenarios.length} scenarios) and ` +
        `${orderName} (${orderArtifact.scenarios.length} scenarios)`,
);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
