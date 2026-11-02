/**
 * `audit-1173` capture: the durable public agent event stream (`watchEvents`) against a private
 * frozen pi v1.3.0 checkout.
 *
 * The script refuses to write evidence unless the checkout is the exact baseline revision and its
 * tracked source is clean. It re-executes itself under the checkout's tsx so the recorded runtime
 * belongs to the same install.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1173-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1173-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1173/capture-1173.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { allEventScenarios } from "./events.mts";
import type { Scenario } from "./pi.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1173";
const TICKET = "1173";
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

if (!process.env.AUDIT_1173_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1173_TSX: "1",
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

const IDENTITY_NORMALIZATION =
    "generated conversation, task, submission and message IDs are compared only through identity relations or stable in-memory IDs";
const EVENT_NORMALIZATION = "event frames may be delivered in batches; the recorded sequence is the flattened event order";
const TASK_NORMALIZATION = "a tool task's ID is compared through its presence and its relation to the slot it names, never printed";
const STOP_NORMALIZATION = "a watch end is compared through its reason";

const sourceEndpoints = [
    "packages/durable/src/harness/events.ts:watchEvents,AgentEvent",
    "packages/durable/src/session/observation.ts:CommittedWatch",
    "packages/durable/src/harness/view.ts:ConversationViews",
    "packages/durable/src/harness/harness.ts:ConversationImpl.id,ConversationImpl.abort",
    "packages/durable/src/harness/generation.ts:GenerationTask",
    "packages/durable/src/harness/tool.ts:ToolTask,ToolExecutionApi.output",
    "packages/durable/src/harness/live.ts:LiveDoc,addCompactionStatus",
    "packages/durable/test/harness-events.test.ts:event stream",
];

const normalizations = [IDENTITY_NORMALIZATION, EVENT_NORMALIZATION, TASK_NORMALIZATION, STOP_NORMALIZATION];

const scenarios = await allEventScenarios();
const names = new Set<string>();
for (const scenario of scenarios) {
    if (!scenario.name.trim() || !scenario.source.trim() || scenario.expected === undefined) {
        throw new Error("a scenario needs a name, source and expected value");
    }
    if (names.has(scenario.name)) throw new Error(`duplicate scenario ${scenario.name}`);
    names.add(scenario.name);
}
const records = scenarios.map((scenario: Scenario) => ({
    name: scenario.name,
    source: scenario.source,
    inputs: scenario.inputs,
    expected: scenario.expected,
    normalizations: [...(scenario.normalizations ?? []), ...normalizations],
}));
const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    sourceEndpoints,
    scenarios: records,
};
const content = `${JSON.stringify(artifact, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "event-stream.json"), content);

const sourceFiles = ["capture-1173.mts", "pi.mts", "support.mts", "events.mts"].map(
    (name) => `fixtures/pi-v1.3.0/${FAMILY}/${name}`,
);
const pinnedSources = sourceFiles.map((path) => ({ path, sha256: sha256(readFileSync(join(REPO, path))) }));
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
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1173/capture-1173.mts",
        sources: pinnedSources,
    },
    artifacts: [
        {
            path: "event-stream.json",
            family: FAMILY,
            bytes: Buffer.byteLength(content),
            sha256: sha256(content),
            sourceEndpoints,
            scenarios: records,
        },
    ],
};
writeFileSync(join(artifactDirectory, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(`captured ${records.length} scenarios`);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
