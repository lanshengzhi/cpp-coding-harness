/**
 * `audit-1053` capture: codemode declarations, execution, deterministic replay and generic Remote
 * contracts against a private frozen pi v1.3.0 checkout. The script refuses to write evidence unless
 * the checkout is the exact baseline revision and its tracked source is clean. It re-executes itself
 * under the checkout's tsx so the recorded tsx version belongs to the same install.
 *
 * Run from the Pike repository:
 *   NODE_OPTIONS=--conditions=source PI_CHECKOUT=/tmp/cch-1053-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1053-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1053/capture-1053.mts
 *
 * The checkout needs `node_modules/quickjs-wasi@3.6.2` (the codemode package's exact dependency).
 * No shared checkout, `~/.pi` state or live provider is read: scripts, tools and remote exchanges are
 * local and deterministic.
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import type { Scenario } from "./scenarios.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1053";
const TICKET = "1053";
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

if (!process.env.AUDIT_1053_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1053_TSX: "1",
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
if (!existsSync(join(CHECKOUT, "packages/codemode/src/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/codemode/src/index.ts is missing");
    process.exit(1);
}

const registryPath = join(REPO, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { bundles?: Record<string, { manifest?: string }> }>;
};
if (registry.baselines[BASELINE]?.bundles?.[FAMILY]) {
    console.error(`${FAMILY} is already registered: ${registry.baselines[BASELINE]!.bundles![FAMILY]!.manifest}`);
    process.exit(1);
}

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0]!.replace(/^tsx v?/, "").trim();
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

interface ArtifactDefinition {
    path: string;
    sourceEndpoints: string[];
    scenarios: () => Promise<Scenario[]>;
}

const {
    sourceScenarios,
    declarationScenarios,
    executionScenarios,
    workerLifetimeScenarios,
    deterministicScenarios,
    inlineScenarios,
    remoteScenarios,
    applicationScenarios,
} = await import("./scenarios.mts");

const artifacts: ArtifactDefinition[] = [
    {
        path: "source-options.json",
        sourceEndpoints: [
            "packages/codemode/src/source.ts:parseCodemodeSource,parseOptions,CodemodeSourceError,CODEMODE_OPTIONS_PREFIX,CODEMODE_SOURCE_GRAMMAR",
            "packages/codemode/test/source.test.ts:parseCodemodeSource",
        ],
        scenarios: sourceScenarios,
    },
    {
        path: "declarations.json",
        sourceEndpoints: [
            "packages/codemode/src/declarations.ts:schemaToType,renderDeclarations,renderToolSignature,renderToolSample,renderToolOutputType,constraintTags",
            "packages/codemode/src/identifier.ts:toCodemodeIdentifier",
            "packages/codemode/src/runtime/prelude-source.ts:MAX_OUTPUT_CHARS,MAX_OUTPUT_ITEMS,MAX_STORE_TOTAL_CHARS,MAX_STORE_VALUE_CHARS",
            "packages/codemode/test/declarations.test.ts:schemaToType,renderDeclarations",
        ],
        scenarios: declarationScenarios,
    },
    {
        path: "script-execution.json",
        sourceEndpoints: [
            "packages/codemode/src/runtime/execution.ts:CodemodeSandboxBase.execute,CodemodeSandboxBase.registerTool,CodemodeSandboxBase.close",
            "packages/codemode/src/runtime/prelude-source.ts:MAX_OUTPUT_CHARS,MAX_OUTPUT_ITEMS,MAX_STORE_TOTAL_CHARS,MAX_STORE_VALUE_CHARS",
            "packages/codemode/src/runtime/vm.ts:startVm",
            "packages/codemode/test/sandbox.test.ts:script execution,tools,output",
        ],
        scenarios: executionScenarios,
    },
    {
        path: "worker-lifetime.json",
        sourceEndpoints: [
            "packages/codemode/src/runtime/host.ts:workerLauncher,CodemodeSandbox",
            "packages/codemode/src/runtime/execution.ts:Execution.finish,CodemodeSandboxBase.close",
            "packages/codemode/src/types.ts:CodemodeSandboxOptions.acquireWorker,CodemodeSandboxOptions.onWorkerStarted",
            "packages/codemode/test/worker-lifetime.test.ts:worker lifetime",
            "packages/codemode/test/fixtures/worker-process-exit.ts:worker process exit",
        ],
        scenarios: workerLifetimeScenarios,
    },
    {
        path: "deterministic-replay.json",
        sourceEndpoints: [
            "packages/codemode/src/runtime/execution.ts:CodemodeSandboxBase.start,CodemodeRun.deliver",
            "packages/codemode/src/runtime/vm.ts:startVm,deterministicWasi,DETERMINISTIC_VERSION",
            "packages/codemode/src/runtime/protocol.ts:VmStartData,WorkerToHostMessage",
            "packages/codemode/src/types.ts:CodemodeRunOptions,CodemodeResult.during",
            "packages/codemode/test/deterministic.test.ts:deterministic runs",
        ],
        scenarios: deterministicScenarios,
    },
    {
        path: "inline-interrupt.json",
        sourceEndpoints: [
            "packages/codemode/src/runtime/inline.ts:InlineCodemodeSandbox,inlineLauncher,DEFAULT_INTERRUPT_BUDGET,INLINE_MAX_STACK_SIZE",
            "packages/codemode/test/portable.test.ts:InlineCodemodeSandbox",
        ],
        scenarios: inlineScenarios,
    },
    {
        path: "remote-exchange.json",
        sourceEndpoints: [
            "packages/codemode/src/runtime/remote.ts:RemoteCodemodeSandbox,remoteLauncher,serveCodemodeRemote,CodemodeRemoteExchange",
            "packages/codemode/src/types.ts:CodemodeRemoteRequest,CodemodeRemoteMessage,CodemodeRemoteReply",
            "packages/codemode/test/portable.test.ts:RemoteCodemodeSandbox",
            "packages/codemode/test/deterministic.test.ts:deterministic runs (remote transport)",
        ],
        scenarios: remoteScenarios,
    },
    {
        path: "application-catalog.json",
        sourceEndpoints: [
            "packages/coding-agent/src/extensions/codemode/tool.ts:createCodemodeDescription,toCodemodeDeclaration,getCodemodeCallableTools,createCodemodeToolDefinition",
            "packages/coding-agent/src/extensions/codemode/index.ts:createCodemodeExtension",
            "packages/coding-agent/test/codemode-extension.test.ts:codemode extension",
        ],
        scenarios: applicationScenarios,
    },
];

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
    join(artifactDirectory, "pi.mts"),
].map((path) => ({ path: relative(REPO, path), sha256: sha256(readFileSync(path)) }));
const generatorCommand =
    "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1053/capture-1053.mts";

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
    const records = scenarios.map((scenario) => ({
        name: scenario.name,
        inputs: scenario.inputs,
        expected: scenario.expected,
        observed: scenario.observed,
        normalizations: scenario.normalizations,
        ...(scenario.upstreamTest ? { upstreamTest: scenario.upstreamTest } : {}),
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

const captureRelative = relative(REPO, SCRIPT);
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    generator: {
        path: captureRelative,
        sha256: sha256(readFileSync(SCRIPT)),
        command: generatorCommand,
        sources: pinnedSources,
    },
    artifacts: manifestArtifacts,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
