/**
 * `audit-1052` capture: MCP protocol, OAuth and application integration against a private frozen pi
 * v1.3.0 checkout. The script refuses to write evidence unless the checkout is the exact baseline
 * revision and its tracked source is clean. It re-executes itself under the checkout's tsx so the
 * recorded `tsx` version belongs to the same install.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1052-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1052-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1052/capture-1052.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import {
    applicationCodemodePrompt,
    applicationDirectStartupTimeout,
    applicationDirectToolsAppear,
    closeWhileConnecting,
    closeWithoutRefresh,
    configUrlPolicy,
    connectListsToolsNotResources,
    crossOriginRedirect,
    discoveryCancellation,
    issuerMismatch,
    plainHttp,
    refreshCancellation,
    registrationCancellation,
    resourcesFailure,
    resourcesLazy,
    sameOriginRedirect,
    signInCancellation,
    structuredArray,
    structuredConversion,
    structuredErrorResult,
    structuredNull,
    structuredPrimitive,
    trustedHttps,
    untrustedHttps,
    type Scenario,
} from "./scenarios.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1052";
const TICKET = "1052";
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

if (!process.env.AUDIT_1052_TSX) {
    const child = execFileSync(
        TSX,
        [SCRIPT, ...process.argv.slice(2)],
        {
            cwd: REPO,
            env: {
                ...process.env,
                AUDIT_1052_TSX: "1",
                NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
                TSX_TSCONFIG_PATH: join(CHECKOUT, "tsconfig.json"),
            },
            stdio: "inherit",
        },
    );
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

interface ArtifactDefinition {
    path: string;
    sourceEndpoints: string[];
    scenarios: () => Promise<Scenario[]>;
}


/** Fixture inputs for each scenario; the capture itself is the executable description. */
function scenarioInputs(name: string): Record<string, unknown> {
    const inputs: Record<string, Record<string, unknown>> = {
        "client preserves an array structuredContent when content is absent": {
            tool: "array",
            content: "absent",
            structuredContent: [1, { x: true }, null],
        },
        "client preserves a primitive structuredContent": {
            tool: "primitive",
            content: [],
            structuredContent: 7,
        },
        "client preserves a null structuredContent": { tool: "null", content: [], structuredContent: null },
        "client and tool conversion preserve an error result": {
            tool: "error",
            content: [{ type: "text", text: "tool failed" }],
            isError: true,
            meta: { traceId: "t-1" },
        },
        "tool conversion serializes structuredContent into model text": {
            tool: "array",
            content: [],
            structuredContent: [{ id: 1 }, { id: 2 }],
        },
        "cancelling a tool call during OAuth refresh never sends it after the refresh completes": {
            mode: "abort",
            seedAccessToken: "access-0",
            seedRefreshToken: "refresh-0",
            stalledEndpoint: "/token",
        },
        "a tool call that times out during OAuth refresh never sends it after the refresh completes": {
            mode: "timeout",
            timeoutMs: 250,
            seedAccessToken: "access-0",
            seedRefreshToken: "refresh-0",
            stalledEndpoint: "/token",
        },
        "closing while a token refresh is in flight does not wait for the refresh": {
            seedAccessToken: "access-0",
            seedRefreshToken: "refresh-0",
            tokenExpiry: "within REFRESH_SKEW_MS",
            stalledEndpoint: "/token",
        },
        "authorization response from another issuer is rejected before token exchange": {
            authorizationCode: "code-forged",
            iss: "https://attacker.example",
        },
        "cancellation during protected-resource discovery aborts the flow": {
            stalledEndpoint: "/.well-known/oauth-protected-resource/mcp",
            cancellation: "AbortSignal",
        },
        "cancellation during dynamic client registration aborts the flow": {
            stalledEndpoint: "/register",
            cancellation: "AbortSignal",
        },
        "cancellation while waiting for the browser callback stops sign-in": {
            stalledAt: "promptForRedirectUrl",
            cancellation: "AbortSignal",
        },
        "application config accepts http and https but rejects another scheme": {
            urls: { plain: "http://127.0.0.1:1/mcp", tls: "https://127.0.0.1:1/mcp", rejected: "ftp://127.0.0.1/mcp" },
        },
        "streamable HTTP transport connects over plain http": { url: "http://127.0.0.1:<port>/mcp" },
        "HTTPS transport connects when its certificate is trusted": {
            url: "https://127.0.0.1:<port>/mcp",
            certificate: "self-signed RSA-2048, SAN IP:127.0.0.1",
        },
        "HTTPS transport rejects an untrusted certificate": {
            url: "https://127.0.0.1:<port>/mcp",
            certificate: "self-signed RSA-2048, SAN IP:127.0.0.1",
            trust: "default Node CA store",
        },
        "same-origin redirect is followed and keeps the request auth header": {
            entrypoint: "/redirect-same",
            target: "/mcp",
            redirectStatus: 307,
            authorization: "Bearer static",
        },
        "cross-origin redirect is followed without Authorization and is rejected": {
            entrypoint: "/redirect-cross",
            targetOrigin: "a second loopback fixture",
            redirectStatus: 307,
            authorization: "Bearer static",
        },
        "connecting a server lists tools but not resources": {
            serverCapabilities: { tools: {}, resources: {} },
            transport: "in-memory",
        },
        "resources are listed on demand after the connection is ready": {
            serverCapabilities: { tools: {}, resources: {} },
            transport: "in-memory",
        },
        "a resources/list failure leaves the server connected and usable for tools": {
            serverCapabilities: { tools: {}, resources: {} },
            resourcesList: "JSON-RPC error",
        },
        "closing while initialize is pending returns without waiting": {
            server: "never answers initialize",
            transport: "in-memory",
        },
        "first prompt does not wait for a codemode server still initializing": {
            exposure: "codemode",
            server: "never answers initialize",
            startupWaitMs: 10000,
        },
        "first prompt waits startupWaitMs for a direct server and reports still connecting": {
            exposure: "direct",
            server: "never answers initialize",
            startupWaitMs: 40,
        },
        "direct server tools become available when it connects within startupWaitMs": {
            exposure: "direct",
            initializeDelayMs: 30,
            startupWaitMs: 500,
        },
    };
    return inputs[name] ?? { fixture: "local loopback" };
}

const artifacts: ArtifactDefinition[] = [
    {
        path: "structured-content.json",
        sourceEndpoints: [
            "packages/mcp/src/client.ts:McpClient.callTool,validateCallToolResult",
            "packages/mcp/src/protocol/content.ts:CallToolResult,toLlmContent",
            "packages/coding-agent/src/extensions/mcp/tools.ts:convertMcpResult,createMcpResultSchema",
            "packages/mcp/test/client.test.ts:accepts structured content that is not an object",
            "packages/mcp/test/content.test.ts:falls back to the structured content as JSON when there are no blocks",
            "packages/coding-agent/test/mcp-extension.test.ts:convertMcpResult",
        ],
        scenarios: async () => [
            await structuredArray(),
            await structuredPrimitive(),
            await structuredNull(),
            await structuredErrorResult(),
            await structuredConversion(),
        ],
    },
    {
        path: "oauth-lifecycle.json",
        sourceEndpoints: [
            "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport.authorizedFetch,StreamableHttpTransport.close",
            "packages/mcp/src/oauth/discovery.ts:discoverOAuthServerInfo,discoverProtectedResourceMetadata",
            "packages/mcp/src/oauth/flow.ts:authorizeMcp,adaptOAuthProvider,startAuthorization,exchangeAuthorizationCode",
            "packages/mcp/src/oauth/provider.ts:McpOAuthProvider",
            "packages/coding-agent/src/extensions/mcp/oauth.ts:createMcpAuthProvider,signInMcpServer,McpOAuthCredentialStore",
            "packages/coding-agent/test/mcp-oauth-refresh.test.ts:refresh",
            "packages/coding-agent/test/suite/agent-session-mcp-oauth.test.ts:OAuth",
            "packages/mcp/test/oauth.test.ts:authorizeMcp",
        ],
        scenarios: async () => [
            await refreshCancellation("abort"),
            await refreshCancellation("timeout"),
            await closeWithoutRefresh(),
            await issuerMismatch(),
            await discoveryCancellation(),
            await registrationCancellation(),
            await signInCancellation(),
        ],
    },
    {
        path: "transports.json",
        sourceEndpoints: [
            "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport,McpAuthRequiredError",
            "packages/mcp/src/transports/transport.ts:McpTransport",
            "packages/coding-agent/src/extensions/mcp/config.ts:loadMcpConfig,validateMcpServerConfig",
            "packages/mcp/test/streamable-http.test.ts:StreamableHttpTransport",
            "packages/coding-agent/test/mcp-extension.test.ts:loadMcpConfig",
        ],
        scenarios: async () => [
            configUrlPolicy(),
            await plainHttp(),
            await trustedHttps(),
            await untrustedHttps(),
            await sameOriginRedirect(),
            await crossOriginRedirect(),
        ],
    },
    {
        path: "application-initialization.json",
        sourceEndpoints: [
            "packages/coding-agent/src/extensions/mcp/index.ts:createMcpExtension,startConnection,before_agent_start,waitForDirectServers",
            "packages/coding-agent/src/extensions/mcp/runtime.ts:McpServerConnection.connectOnce,McpServerConnection.allResources,McpServerConnection.close",
            "packages/coding-agent/src/extensions/mcp/resources.ts:createMcpResourceToolDefinitions",
            "packages/coding-agent/test/mcp-extension.test.ts:McpExtension",
            "packages/coding-agent/test/suite/agent-session-mcp.test.ts:MCP",
        ],
        scenarios: async () => [
            await connectListsToolsNotResources(),
            await resourcesLazy(),
            await resourcesFailure(),
            await closeWhileConnecting(),
            await applicationCodemodePrompt(),
            await applicationDirectStartupTimeout(),
            await applicationDirectToolsAppear(),
        ],
    },
];

const capturedAt = new Date().toISOString();
const pinnedSources = [
    SCRIPT,
    join(artifactDirectory, "scenarios.mts"),
    join(artifactDirectory, "fixtures.mts"),
    join(artifactDirectory, "pi.mts"),
].map((path) => ({ path: relative(REPO, path), sha256: sha256(readFileSync(path)) }));

const manifestArtifacts: Array<Record<string, unknown>> = [];
for (const definition of artifacts) {
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
    const artifact = {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: BASELINE,
        revision,
        ticket: TICKET,
        family: FAMILY,
        capturedAt,
        environment: {
            platform: process.platform,
            arch: process.arch,
            libc: "glibc",
            glibcVersion: report.header.glibcVersionRuntime,
            node: process.version,
            tsx: tsxVersion,
        },
        sourceEndpoints: definition.sourceEndpoints,
        scenarios: scenarios.map((scenario) => ({
            name: scenario.name,
            inputs: scenarioInputs(scenario.name),
            expected: scenario.expected,
            normalizations: [
                "ephemeral loopback ports and local paths are omitted",
                "elapsed times and errors are reported as bounded values or names",
            ],
        })),
    };
    const content = `${JSON.stringify(artifact, null, 2)}\n`;
    writeFileSync(join(artifactDirectory, definition.path), content);
    manifestArtifacts.push({
        path: definition.path,
        family: FAMILY,
        bytes: Buffer.byteLength(content),
        sha256: sha256(content),
        sourceEndpoints: definition.sourceEndpoints,
        scenarios: scenarios.map((scenario) => ({
            name: scenario.name,
            inputs: scenarioInputs(scenario.name),
            expected: scenario.expected,
            normalizations: [
                "ephemeral loopback ports and local paths are omitted",
                "elapsed times and errors are reported as bounded values or names",
            ],
        })),
    });
    console.log(`captured ${definition.path} (${scenarios.length} scenarios)`);
}

const captureRelative = relative(REPO, SCRIPT);
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment: {
        platform: process.platform,
        arch: process.arch,
        libc: "glibc",
        glibcVersion: report.header.glibcVersionRuntime,
        node: process.version,
        tsx: tsxVersion,
    },
    generator: {
        path: captureRelative,
        sha256: sha256(readFileSync(SCRIPT)),
        command:
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1052/capture-1052.mts",
        sources: pinnedSources,
    },
    artifacts: manifestArtifacts,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
