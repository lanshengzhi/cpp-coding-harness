/**
 * `audit-1054` capture: durable Session transactions, documents, observation and the
 * memory/JSONL/SQLite storage contracts of frozen pi v1.3.0.
 *
 * The script refuses to write evidence unless the private checkout is the exact baseline revision
 * and its tracked source is clean. It re-executes itself under that checkout's tsx so the recorded
 * `tsx` version belongs to the install that ran the capture.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1054-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1054-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1054/capture-1054.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import type { Scenario } from "./stores.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1054";
const TICKET = "1054";
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

if (!process.env.AUDIT_1054_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1054_TSX: "1",
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

const {
    backendContractScenarios,
    interchangeScenarios,
    jsonlStoreScenarios,
    sessionTransactionScenarios,
    sqliteStoreScenarios,
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
        path: "session-transactions.json",
        sourceEndpoints: [
            "packages/durable/src/session/session.ts:createSession,SessionImpl.commit,SessionImpl.close",
            "packages/durable/src/session/transaction.ts:Transaction",
            "packages/durable/src/session/observation.ts:CommittedWatch,CommittedStateSource",
            "packages/durable/src/documents.ts:defineDoc,defineDocFamily,materializeDocument",
            "packages/durable/src/entries.ts:defineEntry",
            "packages/durable/test/session-documents.test.ts:Session document transactions",
            "packages/durable/test/session-watches.test.ts:Session document watches",
        ],
        scenarios: sessionTransactionScenarios,
    },
    {
        path: "backend-contract.json",
        sourceEndpoints: [
            "packages/durable/src/storage/memory.ts:MemoryStorage",
            "packages/durable/src/storage/jsonl/storage.ts:JsonlStorage",
            "packages/durable/src/storage/sqlite/storage.ts:SqliteStorage",
            "packages/durable/src/types.ts:Storage,Session,Cursor,Page",
            "packages/durable/src/testing/storage-conformance.ts:registerStorageConformance",
            "packages/durable/test/jsonl-storage.test.ts:JsonlStorage conformance across reopen",
            "packages/durable/test/sqlite-storage.test.ts:SqliteStorage conformance across reopen",
            "packages/durable/test/memory-storage.test.ts:MemoryStorage conformance",
        ],
        scenarios: backendContractScenarios,
    },
    {
        path: "jsonl-store.json",
        sourceEndpoints: [
            "packages/durable/src/storage/jsonl/storage.ts:JsonlStorage.open,JsonlStorage.commit,JsonlStorage.recover",
            "packages/durable/src/storage/jsonl/node.ts:openNodeJsonlStorage",
            "packages/durable/src/env/index.ts:FileSystem",
            "packages/durable/test/jsonl-storage.test.ts:Pico JsonlStorage publication and recovery",
        ],
        scenarios: jsonlStoreScenarios,
    },
    {
        path: "sqlite-store.json",
        sourceEndpoints: [
            "packages/durable/src/storage/sqlite/storage.ts:SqliteStorage.commit",
            "packages/durable/src/storage/sqlite/schema.ts:createSqliteSchema,validateSqliteSchemaPrefix",
            "packages/durable/src/storage/sqlite/migrations.ts:createSqliteMigrations,applySqliteMigrationsForSchema",
            "packages/durable/src/storage/sqlite/node.ts:openNodeSqliteStorage",
            "packages/durable/test/sqlite-migrations.test.ts:Durable SQLite migrations",
            "packages/durable/test/sqlite-storage.test.ts:SqliteStorage conformance",
        ],
        scenarios: sqliteStoreScenarios,
    },
    {
        path: "interchange.json",
        sourceEndpoints: [
            "packages/durable/src/session/session.ts:createSession",
            "packages/durable/src/documents.ts:defineDoc",
            "packages/durable/src/storage/jsonl/node.ts:openNodeJsonlStorage",
            "packages/durable/src/storage/sqlite/node.ts:openNodeSqliteStorage",
            "packages/durable/test/session-documents.test.ts:Session document transactions",
            "packages/durable/test/jsonl-storage.test.ts:Pico JsonlStorage publication and recovery",
            "packages/durable/test/sqlite-storage.test.ts:SqliteStorage conformance across reopen",
        ],
        scenarios: interchangeScenarios,
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

const generatorPath = "fixtures/pi-v1.3.0/audit-1054/capture-1054.mts";
const sourceFiles = [
    "fixtures/pi-v1.3.0/audit-1054/capture-1054.mts",
    "fixtures/pi-v1.3.0/audit-1054/scenarios.mts",
    "fixtures/pi-v1.3.0/audit-1054/stores.mts",
    "fixtures/pi-v1.3.0/audit-1054/pi.mts",
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
