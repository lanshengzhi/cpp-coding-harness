/**
 * `audit-1056` capture: public durable Harness workflows, conversations, compaction, observation,
 * Linux environments/tools/images, code mode/tool search, retry and JSONL storage against a private
 * frozen pi v1.3.0 checkout.
 *
 * The script refuses to write evidence unless the checkout is the exact baseline revision and its
 * tracked source is clean. It re-executes itself under the checkout's tsx so the recorded runtime
 * belongs to the same install.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1056-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1056-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1056/capture-1056.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { allCodemodeToolSearchScenarios } from "./codemode-toolsearch.mts";
import { allCompactionScenarios } from "./compaction.mts";
import { allConversationScenarios } from "./conversations.mts";
import { allEnvironmentScenarios } from "./environment.mts";
import { allGenerationStorageScenarios } from "./generation-storage.mts";
import { allObservabilityScenarios } from "./observability.mts";
import { allWorkflowScenarios } from "./workflow.mts";
import type { Scenario } from "./pi.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1056";
const TICKET = "1056";
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

if (!process.env.AUDIT_1056_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1056_TSX: "1",
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
if (!existsSync(join(CHECKOUT, "packages/durable/src/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/durable/src/index.ts is missing");
    process.exit(1);
}

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0]!.replace(/^tsx v?/, "").trim();
const capturedAt = new Date().toISOString();
const environment = {
    platform: "linux",
    arch: "x64",
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

type ArtifactDefinition = {
    path: string;
    sourceEndpoints: string[];
    normalizations: string[];
    scenarios: () => Promise<Scenario[]>;
};

const IDENTITY_NORMALIZATION =
    "generated conversation, task, submission and message IDs are compared only through identity relations or stable in-memory IDs";
const DURATION_NORMALIZATION = "elapsed durations are recorded as presence, not wall-clock values";
const TEMP_ROOT_NORMALIZATION = "temporary directory roots are omitted; the missing-file diagnostic replaces one with <root>";
const CODEMODE_NORMALIZATION = "code mode wall times are replaced by <seconds>; nested call durations are recorded as presence";
const EVENT_NORMALIZATION = "event frames may be delivered in batches; the recorded sequence is the flattened event order";

const definitions: ArtifactDefinition[] = [
    {
        path: "workflow.json",
        sourceEndpoints: [
            "packages/durable/src/harness/harness.ts:Harness.open",
            "packages/durable/src/harness/generation.ts:GenerationTask",
            "packages/durable/src/harness/tool.ts:ToolTask",
            "packages/durable/src/harness/registry.ts:Registry.install",
            "packages/durable/src/harness/agent.ts:resolveAgent",
            "packages/durable/test/harness-lifecycle.test.ts:conversation workflow",
        ],
        normalizations: [IDENTITY_NORMALIZATION, DURATION_NORMALIZATION],
        scenarios: allWorkflowScenarios,
    },
    {
        path: "conversations.json",
        sourceEndpoints: [
            "packages/durable/src/harness/harness.ts:HarnessImpl.root",
            "packages/durable/src/harness/harness.ts:ConversationImpl.fork",
            "packages/durable/src/harness/harness.ts:ConversationImpl.reset",
            "packages/durable/src/session/transaction.ts:Transaction.createConversation",
            "packages/durable/src/storage/sqlite/node.ts:openNodeSqliteStorage",
            "packages/durable/test/harness-conversations.test.ts:conversation lifecycle",
        ],
        normalizations: [IDENTITY_NORMALIZATION, "provider session identifiers are compared for stability, not printed"],
        scenarios: allConversationScenarios,
    },
    {
        path: "compaction.json",
        sourceEndpoints: [
            "packages/durable/src/harness/compaction.ts:CompactionTask",
            "packages/durable/src/harness/harness.ts:ConversationImpl.compact",
            "packages/durable/src/harness/generation.ts:#prepare",
            "packages/durable/test/harness-compaction.test.ts:compaction policy",
        ],
        normalizations: [IDENTITY_NORMALIZATION, "summary text is the scripted model output"],
        scenarios: allCompactionScenarios,
    },
    {
        path: "observability.json",
        sourceEndpoints: [
            "packages/durable/src/harness/harness.ts:Harness.inspect",
            "packages/durable/src/harness/scheduler.ts:TaskScheduler",
            "packages/durable/src/harness/task-graph.ts:TaskGraphView",
            "packages/durable/src/harness/view.ts:ConversationViews",
            "packages/durable/src/harness/events.ts:watchEvents",
            "packages/durable/src/harness/context.ts:readContext",
            "packages/durable/test/harness-inspect.test.ts:inspection",
            "packages/durable/test/harness-events.test.ts:event stream",
            "packages/durable/test/harness-view.test.ts:view frames",
            "packages/durable/test/harness-task-graph.test.ts:task graph",
        ],
        normalizations: [IDENTITY_NORMALIZATION, EVENT_NORMALIZATION],
        scenarios: allObservabilityScenarios,
    },
    {
        path: "environment.json",
        sourceEndpoints: [
            "packages/durable/src/env/node.ts:NodeExecutionEnv",
            "packages/durable/src/tools/index.ts:createCodingTools",
            "packages/durable/src/tools/bash.ts:createBashTool",
            "packages/durable/src/tools/read.ts:readImage",
            "packages/durable/src/tools/image.ts:detectSupportedImageMimeType",
            "packages/durable/src/images/node.ts:createNodePhotonImages",
        ],
        normalizations: [IDENTITY_NORMALIZATION, DURATION_NORMALIZATION, TEMP_ROOT_NORMALIZATION],
        scenarios: allEnvironmentScenarios,
    },
    {
        path: "codemode-toolsearch.json",
        sourceEndpoints: [
            "packages/durable/src/codemode/codemode.ts:createCodemodeTool",
            "packages/durable/src/codemode/codemode.ts:CodemodeStoreDoc",
            "packages/codemode/src/runtime/host.ts:CodemodeSandbox",
            "packages/durable/src/tool-search/tool-search.ts:createToolSearchTool",
            "packages/durable/src/harness/tool.ts:ToolTask",
            "packages/durable/test/codemode.test.ts:code mode",
            "packages/durable/test/tool-search.test.ts:deferred tools",
        ],
        normalizations: [CODEMODE_NORMALIZATION],
        scenarios: allCodemodeToolSearchScenarios,
    },
    {
        path: "generation-storage.json",
        sourceEndpoints: [
            "packages/durable/src/harness/generation.ts:GenerationTask",
            "packages/durable/src/harness/agent.ts:DEFAULT_RETRY_POLICY",
            "packages/durable/src/storage/jsonl/node.ts:openNodeJsonlStorage",
            "packages/durable/src/storage/jsonl/storage.ts:JsonlStorage",
        ],
        normalizations: [IDENTITY_NORMALIZATION, "the retry delay is configured to 1ms; no wall-clock wait is recorded"],
        scenarios: allGenerationStorageScenarios,
    },
];

const sourceFiles = [
    "capture-1056.mts",
    "pi.mts",
    "support.mts",
    "workflow.mts",
    "conversations.mts",
    "compaction.mts",
    "observability.mts",
    "environment.mts",
    "codemode-toolsearch.mts",
    "generation-storage.mts",
].map((name) => `fixtures/pi-v1.3.0/${FAMILY}/${name}`);

const pinnedSources = sourceFiles.map((path) => ({
    path,
    sha256: sha256(readFileSync(join(REPO, path))),
}));

const manifestArtifacts: any[] = [];
let scenarioCount = 0;
for (const definition of definitions) {
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
        source: scenario.source,
        inputs: scenario.inputs,
        expected: scenario.expected,
        normalizations: [...(scenario.normalizations ?? []), ...definition.normalizations],
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
    scenarioCount += scenarios.length;
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
        command:
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1056/capture-1056.mts",
        sources: pinnedSources,
    },
    artifacts: manifestArtifacts,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`captured ${manifestArtifacts.length} artifacts / ${scenarioCount} scenarios`);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
