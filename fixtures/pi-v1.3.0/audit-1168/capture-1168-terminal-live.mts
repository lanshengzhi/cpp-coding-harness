/**
 * `audit-1168` capture: the frozen pi v1.3.0 public Storage task-write contract for
 * terminal↔live batches (issue #1168).
 *
 * Every scenario stays on the public `Storage` boundary of one frozen backend
 * (`MemoryStorage`, `JsonlStorage`, `SqliteStorage`): a raw `Storage.commit` batch of task
 * writes — the path the frozen Session transaction deliberately does not generate for a
 * terminal task, but the public Storage contract accepts. The capture records, per backend:
 *   - a new task ID settled terminal then live in one batch (last write wins),
 *   - a new task ID settled live then terminal in one batch,
 *   - terminal then live for an already-live task,
 *   - one live write reviving an already-terminal task,
 *   - a mixed batch whose second write is invalid, proving the rejection is atomic and
 *     consumes no sequence.
 * JSONL scenarios additionally record the store directory after each stage (sidecar
 * retention vs reclamation) and the state after close/reopen.
 *
 * A second artifact embeds the exact JSONL store bytes frozen pi produced for the two
 * discriminating shapes, so the C++ comparison runs offline against committed bytes.
 *
 * The script refuses to write evidence unless the private checkout is the exact baseline
 * revision and its tracked source is clean. It re-executes itself under that checkout's tsx
 * so the recorded `tsx` version belongs to the install that ran the capture.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1168-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1168-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1168/capture-1168-terminal-live.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1168";
const TICKET = "1168";
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

if (!process.env.AUDIT_1168_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1168_TSX: "1",
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
if (!existsSync(join(CHECKOUT, "packages/durable/src/storage/memory.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/durable/src/storage/memory.ts is missing");
    process.exit(1);
}

const { BACKGROUND_CONTEXT, MemoryStorage, openNodeJsonlStorage, openNodeSqliteStorage } = await import("./pi.mts");
const ctx = BACKGROUND_CONTEXT;

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

type Scenario = {
    readonly name: string;
    readonly inputs: Record<string, unknown>;
    readonly expected: Record<string, unknown>;
    readonly normalizations: readonly string[];
};

/** One task record in the shape the frozen `StorageWrite` "task" value carries. */
function taskRecord(status: "pending" | "terminal", checkpoint?: Record<string, unknown>): Record<string, unknown> {
    return {
        id: 2,
        conversationId: 1,
        kind: "work",
        version: 1,
        input: {},
        background: false,
        abortRequested: false,
        state:
            status === "terminal"
                ? { status: "terminal", outcome: { status: "completed" } }
                : { status, ...(checkpoint === undefined ? {} : { checkpoint }) },
    };
}

const terminalWrite = () => ({ type: "task", value: taskRecord("terminal") });
const liveWrite = (phase: string) => ({ type: "task", value: taskRecord("pending", { phase }) });
/** One invalid write: a document change for a document that does not exist. */
const invalidWrite = () => ({
    type: "document.change",
    id: 99,
    content: { kind: "base", version: 1, value: {} },
});

async function rejectionOf(promise: Promise<unknown>): Promise<{ rejected: boolean; message?: string }> {
    try {
        await promise;
        return { rejected: false };
    } catch (error) {
        const value = error as { message?: string };
        return { rejected: true, message: value.message ?? String(error) };
    }
}

type Backend = {
    readonly name: "memory" | "jsonl" | "sqlite";
    open(directory: string): Promise<any>;
    /** Backend-specific per-stage directory observations; undefined for memory/sqlite. */
    files?(directory: string): Record<string, unknown>;
};

const memoryBackend: Backend = {
    name: "memory",
    async open() {
        return new MemoryStorage();
    },
};

const jsonlBackend: Backend = {
    name: "jsonl",
    open(directory) {
        return openNodeJsonlStorage(directory, ctx);
    },
    files(directory) {
        return { files: readdirSync(directory).sort() };
    },
};

const sqliteBackend: Backend = {
    name: "sqlite",
    open(directory) {
        return openNodeSqliteStorage(join(directory, "store.sqlite"), ctx);
    },
};

async function taskState(storage: any): Promise<unknown> {
    const record = await storage.task(2, ctx);
    return record === undefined ? null : record;
}

async function close(storage: any): Promise<void> {
    await storage.close(ctx);
}

/** These scenarios record raw observations; no value needs normalization. */
const normalizations: readonly string[] = [];

/** Run one backend's five-case matrix and return the scenarios in a stable order. */
async function backendScenarios(backend: Backend): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    // Case A: a brand-new task ID settled terminal then live in one batch. A
    // memory store has no reopen, so it records the next commit on the same
    // instance instead.
    {
        const directory = mkdtempSync(join(tmpdir(), `1168-${backend.name}-a-`));
        try {
            const storage = await backend.open(directory);
            const seq = await storage.commit([terminalWrite(), liveWrite("again")], ctx);
            const immediate = await taskState(storage);
            const stageFiles = backend.files?.(directory) ?? {};
            let reopen: unknown = null;
            let nextCommitSeq: number;
            if (backend.name === "memory") {
                nextCommitSeq = await storage.commit([liveWrite("next")], ctx);
                await close(storage);
            } else {
                await close(storage);
                const reopened = await backend.open(directory);
                reopen = await taskState(reopened);
                nextCommitSeq = await reopened.commit([liveWrite("next")], ctx);
                await close(reopened);
            }
            scenarios.push({
                name: `${backend.name} storage settles a new task terminal then live in one batch with the last write`,
                inputs: { backend: backend.name, writes: [terminalWrite(), liveWrite("again")] },
                expected: {
                    seq,
                    immediate,
                    ...stageFiles,
                    reopen,
                    nextCommitSeq,
                },
                normalizations,
            });
        } finally {
            rmSync(directory, { recursive: true, force: true });
        }
    }

    // Case B: a brand-new task ID settled live then terminal in one batch.
    {
        const directory = mkdtempSync(join(tmpdir(), `1168-${backend.name}-b-`));
        try {
            const storage = await backend.open(directory);
            const seq = await storage.commit([liveWrite("first"), terminalWrite()], ctx);
            const immediate = await taskState(storage);
            const stageFiles = backend.files?.(directory) ?? {};
            let reopen: unknown = null;
            let nextCommitSeq: number;
            let afterCloseFiles: Record<string, unknown> = {};
            if (backend.name === "memory") {
                nextCommitSeq = await storage.commit([liveWrite("after-terminal")], ctx);
                await close(storage);
            } else {
                await close(storage);
                afterCloseFiles = backend.files?.(directory) ?? {};
                const reopened = await backend.open(directory);
                reopen = await taskState(reopened);
                nextCommitSeq = await reopened.commit([liveWrite("after-terminal")], ctx);
                await close(reopened);
            }
            scenarios.push({
                name: `${backend.name} storage settles a new task live then terminal in one batch with the last write`,
                inputs: { backend: backend.name, writes: [liveWrite("first"), terminalWrite()] },
                expected: {
                    seq,
                    immediate,
                    ...stageFiles,
                    ...afterCloseFiles,
                    reopen,
                    nextCommitSeq,
                },
                normalizations,
            });
        } finally {
            rmSync(directory, { recursive: true, force: true });
        }
    }

    // Case C: an already-live task settled terminal then live in one batch.
    {
        const directory = mkdtempSync(join(tmpdir(), `1168-${backend.name}-c-`));
        try {
            const storage = await backend.open(directory);
            const firstSeq = await storage.commit([liveWrite("original")], ctx);
            const secondSeq = await storage.commit([terminalWrite(), liveWrite("again")], ctx);
            const immediate = await taskState(storage);
            const stageFiles = backend.files?.(directory) ?? {};
            let reopen: unknown = null;
            if (backend.name === "memory") {
                await close(storage);
            } else {
                await close(storage);
                const reopened = await backend.open(directory);
                reopen = await taskState(reopened);
                await close(reopened);
            }
            scenarios.push({
                name: `${backend.name} storage accepts terminal then live for an already-live task`,
                inputs: {
                    backend: backend.name,
                    firstCommit: [liveWrite("original")],
                    secondCommit: [terminalWrite(), liveWrite("again")],
                },
                expected: { firstSeq, secondSeq, immediate, ...stageFiles, reopen },
                normalizations,
            });
        } finally {
            rmSync(directory, { recursive: true, force: true });
        }
    }

    // Case D: one live write reviving an already-terminal task.
    {
        const directory = mkdtempSync(join(tmpdir(), `1168-${backend.name}-d-`));
        try {
            const storage = await backend.open(directory);
            const terminalSeq = await storage.commit([terminalWrite()], ctx);
            const filesAfterTerminal = backend.files?.(directory) ?? {};
            const reviveSeq = await storage.commit([liveWrite("revived")], ctx);
            const immediate = await taskState(storage);
            const stageFiles = backend.files?.(directory) ?? {};
            let reopen: unknown = null;
            if (backend.name === "memory") {
                await close(storage);
            } else {
                await close(storage);
                const reopened = await backend.open(directory);
                reopen = await taskState(reopened);
                await close(reopened);
            }
            scenarios.push({
                name: `${backend.name} storage revives an existing terminal task with one live write`,
                inputs: {
                    backend: backend.name,
                    terminalCommit: [terminalWrite()],
                    reviveCommit: [liveWrite("revived")],
                },
                expected: {
                    terminalSeq,
                    ...filesAfterTerminal,
                    reviveSeq,
                    immediate,
                    ...stageFiles,
                    reopen,
                },
                normalizations,
            });
        } finally {
            rmSync(directory, { recursive: true, force: true });
        }
    }

    // Case E: a mixed batch whose second write is invalid rejects atomically.
    {
        const directory = mkdtempSync(join(tmpdir(), `1168-${backend.name}-e-`));
        try {
            const storage = await backend.open(directory);
            const rejection = await rejectionOf(storage.commit([terminalWrite(), invalidWrite()], ctx));
            const after = await taskState(storage);
            const stageFiles = backend.files?.(directory) ?? {};
            const nextEmptyCommitSeq = await storage.commit([], ctx);
            await close(storage);
            scenarios.push({
                name: `${backend.name} storage rejects a mixed invalid batch atomically and consumes no sequence`,
                inputs: { backend: backend.name, writes: [terminalWrite(), invalidWrite()] },
                expected: {
                    rejected: rejection.rejected,
                    error: rejection.message ?? null,
                    taskAfter: after,
                    ...stageFiles,
                    nextEmptyCommitSeq,
                },
                normalizations,
            });
        } finally {
            rmSync(directory, { recursive: true, force: true });
        }
    }

    return scenarios;
}

/** Read every JSONL file of a store directory in the audit-1054 interchange shape. */
function readJsonlStore(directory: string): unknown {
    const files = readdirSync(directory)
        .filter((name) => name.endsWith(".jsonl"))
        .sort()
        .map((name) => {
            const bytes = readFileSync(join(directory, name));
            const text = bytes.toString("utf8");
            return {
                name,
                bytes: bytes.length,
                sha256: sha256(bytes),
                text,
                lines: text
                    .split("\n")
                    .filter((line) => line !== "")
                    .map((line) => JSON.parse(line) as unknown),
            };
        });
    return { files };
}

/** Produce one JSONL store for the interchange artifact and return its files plus reopen facts. */
async function produceStore(
    ...commits: (() => Record<string, unknown>)[]
): Promise<{ files: unknown; reopenTask: unknown; nextCommitSeq: number }> {
    const directory = mkdtempSync(join(tmpdir(), "1168-interchange-"));
    try {
        let storage = await openNodeJsonlStorage(directory, ctx);
        for (const writes of commits) {
            await storage.commit(writes(), ctx);
        }
        await close(storage);
        const files = readJsonlStore(directory);
        storage = await openNodeJsonlStorage(directory, ctx);
        const reopenTask = await taskState(storage);
        const nextCommitSeq = await storage.commit([liveWrite("continued")], ctx);
        await close(storage);
        return { files, reopenTask, nextCommitSeq };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
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

const terminalLiveScenarios: Scenario[] = [];
for (const backend of [memoryBackend, jsonlBackend, sqliteBackend]) {
    terminalLiveScenarios.push(...(await backendScenarios(backend)));
}

// Guard the frozen meaning of the capture: the property under test is that the
// public Storage accepts terminal->live batches; a frozen pi that starts
// rejecting them invalidates the recorded expectations instead of silently
// pinning a different contract.
for (const scenario of terminalLiveScenarios) {
    if (scenario.name.includes("rejects a mixed invalid batch")) {
        if (scenario.expected.rejected !== true) {
            throw new Error(`${scenario.name}: frozen pi did not reject the invalid batch`);
        }
    } else if (scenario.expected.seq === undefined && scenario.expected.secondSeq === undefined &&
            scenario.expected.reviveSeq === undefined) {
        throw new Error(`${scenario.name}: captured no sequence`);
    }
}

const interchangeScenarios: Scenario[] = [];

{
    const produced = await produceStore(() => [terminalWrite(), liveWrite("again")]);
    interchangeScenarios.push({
        name: "frozen pi jsonl store with a terminal then live task batch reopens with the pending task",
        inputs: { backend: "jsonl", writes: [terminalWrite(), liveWrite("again")] },
        expected: {
            seq: 1,
            reopen: produced.reopenTask,
            nextCommitSeq: produced.nextCommitSeq,
            ...produced.files,
        },
        normalizations,
    });
}

{
    const produced = await produceStore(() => [liveWrite("first"), terminalWrite()]);
    interchangeScenarios.push({
        name: "frozen pi jsonl store with a live then terminal task batch reopens with the terminal task and a reclaimed sidecar",
        inputs: { backend: "jsonl", writes: [liveWrite("first"), terminalWrite()] },
        expected: {
            seq: 1,
            reopen: produced.reopenTask,
            nextCommitSeq: produced.nextCommitSeq,
            ...produced.files,
        },
        normalizations,
    });
}

{
    const produced = await produceStore(() => [terminalWrite()], () => [liveWrite("revived")]);
    interchangeScenarios.push({
        name: "frozen pi jsonl store with a revived terminal task reopens with the pending task",
        inputs: { backend: "jsonl", terminalCommit: [terminalWrite()], reviveCommit: [liveWrite("revived")] },
        expected: {
            terminalSeq: 1,
            reviveSeq: 2,
            reopen: produced.reopenTask,
            nextCommitSeq: produced.nextCommitSeq,
            ...produced.files,
        },
        normalizations,
    });
}

type ArtifactDefinition = {
    readonly path: string;
    readonly sourceEndpoints: readonly string[];
    readonly scenarios: Scenario[];
};

const artifacts: ArtifactDefinition[] = [
    {
        path: "terminal-live.json",
        sourceEndpoints: [
            "packages/durable/src/storage/memory.ts:MemoryStorage.prepareCommit,MemoryStorage.commit",
            "packages/durable/src/storage/jsonl/storage.ts:JsonlStorage.commit,JsonlStorage.recover,JsonlStorage.planReclamations",
            "packages/durable/src/storage/sqlite/storage.ts:SqliteStorage.commit",
            "packages/durable/src/session/transaction.ts:Transaction.#assemble",
            "packages/durable/test/memory-storage.test.ts:MemoryStorage conformance",
            "packages/durable/test/jsonl-storage.test.ts:Pico JsonlStorage publication and recovery",
            "packages/durable/test/sqlite-storage.test.ts:SqliteStorage conformance across reopen",
        ],
        scenarios: terminalLiveScenarios,
    },
    {
        path: "interchange.json",
        sourceEndpoints: [
            "packages/durable/src/storage/jsonl/storage.ts:JsonlStorage.open,JsonlStorage.commit,JsonlStorage.recover",
            "packages/durable/src/storage/jsonl/node.ts:openNodeJsonlStorage",
            "packages/durable/test/jsonl-storage.test.ts:Pico JsonlStorage publication and recovery",
        ],
        scenarios: interchangeScenarios,
    },
];

const artifactRecords: unknown[] = [];
for (const definition of artifacts) {
    if (definition.scenarios.length === 0) throw new Error(`${definition.path} captured no scenarios`);
    const names = new Set<string>();
    for (const scenario of definition.scenarios) {
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
        scenarios: definition.scenarios,
    };
    const content = `${JSON.stringify(artifact, null, 2)}\n`;
    writeFileSync(join(bundle, definition.path), content, "utf8");
    artifactRecords.push({
        path: definition.path,
        family: FAMILY,
        bytes: Buffer.byteLength(content),
        sha256: sha256(content),
        sourceEndpoints: definition.sourceEndpoints,
        scenarios: definition.scenarios,
    });
    console.log(`${definition.path}: ${definition.scenarios.length} scenario(s), ${Buffer.byteLength(content)} bytes`);
}

const generatorPath = "fixtures/pi-v1.3.0/audit-1168/capture-1168-terminal-live.mts";
const sourceFiles = [
    "fixtures/pi-v1.3.0/audit-1168/capture-1168-terminal-live.mts",
    "fixtures/pi-v1.3.0/audit-1168/pi.mts",
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
