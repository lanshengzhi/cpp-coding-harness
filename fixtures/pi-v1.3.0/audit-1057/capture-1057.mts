/**
 * `audit-1057` capture: the durable-required subset of `@earendil-works/chord` in frozen pi v1.3.0.
 *
 * The script refuses to write evidence unless the private checkout is the exact baseline revision
 * and its tracked source is clean. It re-executes itself under that checkout's tsx so the recorded
 * `tsx` version belongs to the install that ran the capture.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1057-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1057-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1057/capture-1057.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import type { Scenario } from "./scenarios.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1057";
const TICKET = "1057";
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

if (!process.env.AUDIT_1057_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1057_TSX: "1",
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
if (!existsSync(join(CHECKOUT, "packages/chord/src/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/chord/src/index.ts is missing");
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/durable/src/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/durable/src/index.ts is missing");
    process.exit(1);
}

const {
    contextScenarios,
    deltaScenarios,
    documentScenarios,
    jsonOutputScenarios,
    replicatedStateScenarios,
} = await import("./scenarios.mts");

const registryPath = join(REPO, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { revision?: string; bundles?: Record<string, unknown> }>;
};
const registered = registry.baselines[BASELINE];
if (registered?.revision !== EXPECTED_REVISION) {
    console.error(`baseline ${BASELINE} does not resolve to ${EXPECTED_REVISION}`);
    process.exit(1);
}
if (registered.bundles?.[FAMILY] !== undefined) {
    console.error(`${FAMILY} is already registered; a deliberate recapture needs an explicit registry replacement`);
    process.exit(1);
}

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const capturedAt = new Date().toISOString();
const environment = {
    platform: "linux",
    arch: "x64",
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};

const bundle = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(bundle, { recursive: true });

type ArtifactDefinition = {
    readonly path: string;
    readonly sourceEndpoints: readonly string[];
    readonly scenarios: () => Promise<Scenario[]>;
};

const artifacts: ArtifactDefinition[] = [
    {
        path: "document-transaction.json",
        sourceEndpoints: [
            "packages/durable/src/session/session.ts:createSession,SessionImpl.commit,SessionImpl.watchDoc,SessionImpl.documentState",
            "packages/durable/src/session/transaction.ts:Transaction.doc",
            "packages/durable/src/session/observation.ts:CommittedWatch,CommittedStateSource,RETIREMENT_OPERATIONS",
            "packages/durable/src/documents.ts:defineDoc",
            "packages/durable/test/session-documents.test.ts:Session document transactions",
            "packages/durable/test/session-watches.test.ts:Session document watches",
            "packages/durable/test/session-checkpoints-migrations.test.ts:Session document checkpoints; Session document migrations",
        ],
        scenarios: documentScenarios,
    },
    {
        path: "context-cancellation.json",
        sourceEndpoints: [
            "packages/chord/src/context/index.ts:BACKGROUND_CONTEXT,createContextKey,withContextValue,withAbortSignal,withoutAbortSignal,withCancel,awaitWithContext",
            "packages/durable/src/session/session.ts:SessionImpl.commit,SessionImpl.watchDoc,SessionImpl.close",
            "packages/durable/src/session/observation.ts:CommittedWatch",
            "packages/chord/test/context.test.ts:Context",
            "packages/chord/test/replicated-state-delivery.test.ts:Replicated state delivery",
            "packages/durable/test/session-watches.test.ts:Session document watches",
        ],
        scenarios: contextScenarios,
    },
    {
        path: "delta-revisions.json",
        sourceEndpoints: [
            "packages/chord/src/delta/index.ts:track,apply,applyImmutable,applyImmutableBatches,assertValidOp,isBase,isReplace,overlap",
            "packages/chord/src/delta/tracker.ts:Tracker,Change,Prepared",
            "packages/chord/test/delta.test.ts:Delta tracking and application",
            "packages/chord/test/delta-apply-immutable.test.ts:Immutable delta application",
        ],
        scenarios: deltaScenarios,
    },
    {
        path: "replicated-state.json",
        sourceEndpoints: [
            "packages/chord/src/state.ts:replicatedState,AttachedReplicatedState,ReplicatedStateSource,ReplicatedStateFrame",
            "packages/durable/src/session/observation.ts:CommittedStateSource,CommittedWatch",
            "packages/durable/src/session/session.ts:SessionImpl.watchDoc",
            "packages/chord/test/replicated-state.test.ts:Replicated state",
            "packages/chord/test/replicated-state-delivery.test.ts:Replicated state delivery",
            "packages/durable/test/session-watches.test.ts:Session document watches",
        ],
        scenarios: replicatedStateScenarios,
    },
    {
        path: "json-output-support.json",
        sourceEndpoints: [
            "packages/chord/src/json.ts:copyJson",
            "packages/chord/src/output.ts:utf8ByteLength,characterEnd,boundOutput,OutputBuffer,PROGRESS_BYTES_PER_SECOND",
            "packages/chord/test/json.test.ts:copyJson",
            "packages/chord/test/output.test.ts:OutputBuffer",
            "packages/chord/test/output-skip.test.ts:OutputBuffer skipped output",
            "packages/durable/src/harness/tool.ts:Progress,OutputBuffer",
        ],
        scenarios: jsonOutputScenarios,
    },
];

const artifactRecords: unknown[] = [];
for (const definition of artifacts) {
    const scenarios = await definition.scenarios();
    if (scenarios.length === 0) throw new Error(`${definition.path} captured no scenarios`);
    const names = new Set<string>();
    for (const scenario of scenarios) {
        if (names.has(scenario.name)) throw new Error(`${definition.path} has a duplicate scenario: ${scenario.name}`);
        names.add(scenario.name);
    }
    const artifact = {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: BASELINE,
        revision: EXPECTED_REVISION,
        ticket: TICKET,
        family: FAMILY,
        capturedAt,
        environment,
        sourceEndpoints: definition.sourceEndpoints,
        scenarios,
    };
    const content = `${JSON.stringify(artifact, null, 2)}\n`;
    writeFileSync(join(bundle, definition.path), content, "utf8");
    artifactRecords.push({
        path: definition.path,
        family: FAMILY,
        bytes: Buffer.byteLength(content),
        sha256: sha256(content),
        sourceEndpoints: definition.sourceEndpoints,
        scenarios,
    });
    console.log(`${definition.path}: ${scenarios.length} scenario(s), ${Buffer.byteLength(content)} bytes`);
}

const generatorPath = "fixtures/pi-v1.3.0/audit-1057/capture-1057.mts";
const sourceFiles = [
    "fixtures/pi-v1.3.0/audit-1057/capture-1057.mts",
    "fixtures/pi-v1.3.0/audit-1057/scenarios.mts",
    "fixtures/pi-v1.3.0/audit-1057/pi.mts",
];
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision: EXPECTED_REVISION,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    generator: {
        path: generatorPath,
        sha256: sha256(readFileSync(join(REPO, generatorPath))),
        command:
            `PI_CHECKOUT=${CHECKOUT} PI_BASELINE=${BASELINE} ` +
            `${join(CHECKOUT, "node_modules/.bin/tsx")} ${generatorPath}`,
        sources: sourceFiles.map((path) => ({ path, sha256: sha256(readFileSync(join(REPO, path))) })),
    },
    artifacts: artifactRecords,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(bundle, "manifest.json"), manifestContent, "utf8");
console.log(`manifest.json: ${Buffer.byteLength(manifestContent)} bytes, sha256 ${sha256(manifestContent)}`);
