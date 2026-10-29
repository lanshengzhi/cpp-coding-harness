/**
 * `audit-1104` capture: the real MCP management UI during connection, against a private frozen pi
 * v1.3.0 checkout. The script refuses to write evidence unless the checkout is the exact baseline
 * revision and its tracked source is clean. It re-executes itself under the checkout's tsx so the
 * recorded `tsx` version belongs to the same install.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1104-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1104-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1104/capture-1104.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import {
    externalSignInPickup,
    managerUsableDuringConnection,
    printStatusSettlesConnections,
    shutdownWhileConnecting,
    type Scenario,
} from "./scenarios.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1104";
const TICKET = "1104";
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

if (!process.env.AUDIT_1104_TSX) {
    execFileSync(
        TSX,
        [SCRIPT, ...process.argv.slice(2)],
        {
            cwd: REPO,
            env: {
                ...process.env,
                AUDIT_1104_TSX: "1",
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
if (!existsSync(join(CHECKOUT, "packages/coding-agent/src/extensions/mcp/ui.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/coding-agent/src/extensions/mcp/ui.ts is missing");
    process.exit(1);
}

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

/** Fixture inputs for each scenario; the capture itself is the executable description. */
function scenarioInputs(name: string): Record<string, unknown> {
    const inputs: Record<string, Record<string, unknown>> = {
        "the /mcp manager lists connecting servers and completes a sign-in while another server connects": {
            servers: [
                { name: "slow", transport: "in-memory", behavior: "never answers initialize", exposure: "direct" },
                { name: "auth", transport: "streamable-http", behavior: "401 OAuth challenge", exposure: "direct" },
            ],
            panel: "real McpManagerView on a headless terminal double",
        },
        "plain /mcp status waits for pending connections and then reports settled states": {
            servers: [{ name: "slow", transport: "in-memory", initializeDelayMs: 400, exposure: "codemode" }],
            command: "/mcp with no arguments in print mode",
        },
        "a sign-in performed outside the session is picked up on the next turn": {
            servers: [{ name: "auth", transport: "streamable-http", behavior: "401 OAuth challenge", exposure: "direct" }],
            externalSignIn: "credentials stored directly in mcp-auth.json, as `pi mcp login` in another process would",
        },
        "closing the session while initialize is pending returns without waiting": {
            servers: [{ name: "slow", transport: "in-memory", behavior: "never answers initialize", exposure: "direct" }],
            event: "session_shutdown while the first initialize is unanswered",
        },
    };
    const input = inputs[name];
    if (!input) throw new Error(`no pinned inputs for scenario ${name}`);
    return input;
}

const artifacts: Array<{ path: string; sourceEndpoints: string[]; scenarios: () => Promise<Scenario[]> }> = [
    {
        path: "management-during-connection.json",
        sourceEndpoints: [
            "packages/coding-agent/src/extensions/mcp/index.ts:createMcpExtension,session_start,startConnection,manage,serversMenu,serverMenu,runAction,signInWithUi,formatStatus,onConnectionChange,tokensAtSignIn,reconnectSignedIn,turn_start,session_shutdown,registerCommand(mcp)",
            "packages/coding-agent/src/extensions/mcp/runtime.ts:McpServerConnection.close,connectFailed,markNeedsAuth",
            "packages/coding-agent/src/extensions/mcp/ui.ts:McpManagerView.menu,status,redirectUrl,showMcpManager",
            "packages/coding-agent/test/mcp-manager-view.test.ts:McpManagerView",
        ],
        scenarios: async () => [
            await managerUsableDuringConnection(),
            await printStatusSettlesConnections(),
            await externalSignInPickup(),
            await shutdownWhileConnecting(),
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
    const normalize = (scenario: Scenario) => ({
        name: scenario.name,
        inputs: scenarioInputs(scenario.name),
        expected: scenario.expected,
        normalizations: [
            "ephemeral loopback ports and local paths are omitted",
            "elapsed times and errors are reported as bounded values or names",
        ],
    });
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
        scenarios: scenarios.map(normalize),
    };
    const content = `${JSON.stringify(artifact, null, 2)}\n`;
    writeFileSync(join(artifactDirectory, definition.path), content);
    manifestArtifacts.push({
        path: definition.path,
        family: FAMILY,
        bytes: Buffer.byteLength(content),
        sha256: sha256(content),
        sourceEndpoints: definition.sourceEndpoints,
        scenarios: scenarios.map(normalize),
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
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1104/capture-1104.mts",
        sources: pinnedSources,
    },
    artifacts: manifestArtifacts,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
