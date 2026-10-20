/**
 * `audit-1063` capture: the frozen pi v1.3.0 durable Session properties that
 * ticket #1063's `cch_durable` owner slice must reproduce — the read-after-write
 * order rule, the close rejection, the sequence and ID identity, the failed
 * callback rollback, the reopen over memory storage, and the watch that replays
 * op-bearing committed frames from an older revision.
 *
 * The script refuses to write evidence unless the private checkout is the exact
 * baseline revision and its tracked source is clean. It re-executes itself under
 * that checkout's tsx so the recorded `tsx` version belongs to the install that
 * ran the capture.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1050-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1050-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1063/capture-1063.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import type { Scenario } from "./scenarios.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1063";
const TICKET = "1063";
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

if (!process.env.AUDIT_1063_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1063_TSX: "1",
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

const { durableSessionScenarios } = await import("./scenarios.mts");

const scenarios = await durableSessionScenarios();
if (scenarios.length === 0) throw new Error("the capture recorded no scenario");
const names = new Set<string>();
for (const scenario of scenarios) {
    if (names.has(scenario.name)) throw new Error(`duplicate scenario: ${scenario.name}`);
    names.add(scenario.name);
}

const sourceEndpoints = [
    "packages/durable/src/session/session.ts:createSession,SessionImpl.commit,SessionImpl.close,SessionImpl.assertUsable",
    "packages/durable/src/session/transaction.ts:Transaction,Transaction.#read,Transaction.#write,Transaction.settleFailure",
    "packages/durable/src/session/observation.ts:CommittedWatch",
    "packages/durable/src/storage/memory.ts:MemoryStorage,MemoryStorage.commit,MemoryStorage.prepareCommit,MemoryStorage.mintId",
    "packages/durable/src/types.ts:Storage,StorageWrite,CommitPublication,SessionEnd",
    "packages/durable/src/errors.ts:ReadAfterWrite,SessionFailed",
    "packages/durable/src/entries.ts:defineEntry",
    "packages/durable/src/documents.ts:defineDoc",
    "packages/durable/test/session-tables.test.ts:allows table reads only before the first table write",
    "packages/durable/test/session-watches.test.ts:can replay every delivered exact operation batch from the acquisition revision",
    "packages/durable/test/session-documents.test.ts:aborts every change when the callback fails",
    "packages/chord/src/state.ts:ReplicatedStateSource,ReplicatedStateSourceAttachment",
    "packages/chord/src/delta/index.ts:applyImmutable",
];

const bundle = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(bundle, { recursive: true });

const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision: EXPECTED_REVISION,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    sourceEndpoints,
    scenarios,
};
const artifactContent = `${JSON.stringify(artifact, null, 2)}\n`;
const artifactPath = "session-transactions.json";
writeFileSync(join(bundle, artifactPath), artifactContent, "utf8");
console.log(`${artifactPath}: ${scenarios.length} scenario(s), ${Buffer.byteLength(artifactContent)} bytes`);

const generatorPath = "fixtures/pi-v1.3.0/audit-1063/capture-1063.mts";
const sourceFiles = [
    "fixtures/pi-v1.3.0/audit-1063/capture-1063.mts",
    "fixtures/pi-v1.3.0/audit-1063/scenarios.mts",
    "fixtures/pi-v1.3.0/audit-1063/pi.mts",
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
    artifacts: [
        {
            path: artifactPath,
            family: FAMILY,
            bytes: Buffer.byteLength(artifactContent),
            sha256: sha256(artifactContent),
            sourceEndpoints,
            scenarios,
        },
    ],
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(bundle, "manifest.json"), manifestContent, "utf8");
console.log(`manifest.json: ${Buffer.byteLength(manifestContent)} bytes, sha256 ${sha256(manifestContent)}`);
