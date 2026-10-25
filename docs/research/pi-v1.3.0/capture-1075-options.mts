import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { DatabaseSync } from "node:sqlite";
import { openNodeSqliteDatabase, openNodeSqliteStorage } from "/tmp/cch-1054-pi-frozen/packages/durable/src/storage/sqlite/node.ts";
import { SqliteStorage } from "/tmp/cch-1054-pi-frozen/packages/durable/src/storage/sqlite/storage.ts";
import { createSession } from "/tmp/cch-1054-pi-frozen/packages/durable/src/session/session.ts";
import { defineDoc } from "/tmp/cch-1054-pi-frozen/packages/durable/src/documents.ts";
import { defineTask } from "/tmp/cch-1054-pi-frozen/packages/durable/src/tasks.ts";
import { BACKGROUND_CONTEXT } from "/tmp/cch-1054-pi-frozen/packages/chord/src/context/index.ts";
import { applySqliteMigrationsForSchema } from "/tmp/cch-1054-pi-frozen/packages/durable/src/storage/sqlite/migrations.ts";
import { createSqliteSchema } from "/tmp/cch-1054-pi-frozen/packages/durable/src/storage/sqlite/schema.ts";

const scriptPath = fileURLToPath(import.meta.url);
const repository = join(dirname(scriptPath), "../../..");
const family = "audit-1075";
const baseline = "pi-v1.3.0";
const revision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const checkout = process.env.PI_CHECKOUT ?? "/tmp/cch-1054-pi-frozen";
const tsx = join(checkout, "node_modules/.bin/tsx");
if (!existsSync(tsx)) throw new Error(`frozen checkout has no tsx executable: ${tsx}`);

if (process.env.AUDIT_1075_TSX !== "1") {
    execFileSync(tsx, [scriptPath, ...process.argv.slice(2)], {
        cwd: repository,
        env: {
            ...process.env,
            AUDIT_1075_TSX: "1",
            NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
            TSX_TSCONFIG_PATH: join(checkout, "tsconfig.json"),
        },
        stdio: "inherit",
    });
    process.exit(0);
}

const git = (args: string[]) => execFileSync("git", ["-C", checkout, ...args], { encoding: "utf8" }).trim();
if (git(["rev-parse", "HEAD"]) !== revision) throw new Error("frozen baseline revision mismatch");
if (git(["status", "--porcelain"]) !== "") throw new Error("frozen baseline checkout must be clean");
if (process.platform !== "linux" || process.arch !== "x64") throw new Error("capture requires Linux x64");
const processReport = process.report?.getReport() as { header?: { glibcVersionRuntime?: string } } | undefined;
if (!processReport?.header?.glibcVersionRuntime) throw new Error("capture requires glibc Linux");

const registryPath = join(repository, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { revision?: string; bundles?: Record<string, unknown> }>;
};
const registeredBaseline = registry.baselines[baseline];
if (registeredBaseline?.revision !== revision) throw new Error(`${baseline} does not resolve to the expected revision`);
if (registeredBaseline.bundles?.[family] !== undefined) throw new Error(`${family} is already registered`);
const sha256 = (value: Buffer | string) => createHash("sha256").update(value).digest("hex");
const run = (command: string, args: string[]) => execFileSync(command, args, { cwd: repository, encoding: "utf8" }).trim();
const tsxVersion = run(tsx, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const capturedAt = new Date().toISOString();
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: processReport.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};
const sourceEndpoints = [
    "packages/durable/src/storage/sqlite/node.ts:openNodeSqliteStorage,NodeSqliteDatabase",
    "packages/durable/src/storage/sqlite/storage.ts:SqliteStorage.scanEntries,applyDocumentActions",
    "packages/durable/src/storage/sqlite/migrations.ts:document_revisions",
    "packages/durable/src/storage/sqlite/database.ts:SqliteDatabase",
    "packages/durable/src/session/transaction.ts:Transaction.conversation,entry,task,scanConversations,scanEntries,scanTasks,latestHeadMarker,submissionByRequest",
    "packages/durable/test/sqlite-storage.test.ts:SqliteStorage filtered scans and options",
];
const scenarios: unknown[] = [];

const directory = mkdtempSync(join(tmpdir(), "cch-audit1075-options-"));
try {
    const path = join(directory, "storage.sqlite");
    const initialization = await openNodeSqliteDatabase(path, { busyTimeoutMs: 30, walAutoCheckpointPages: 7 });
    const schema = createSqliteSchema();
    await applySqliteMigrationsForSchema(initialization, schema);
    await initialization.run("UPDATE durable_metadata SET next_id = '100', next_seq = 50 WHERE singleton = 1");
    await initialization.close();
    const database = await openNodeSqliteDatabase(path, { busyTimeoutMs: 30, walAutoCheckpointPages: 7 });
    const connectionSettings = {
        busyTimeoutMs: await database.get("PRAGMA busy_timeout"),
        walAutoCheckpointPages: await database.get("PRAGMA wal_autocheckpoint"),
        journalMode: await database.get("PRAGMA journal_mode"),
    };
    const storage = await SqliteStorage.open(database);
    const session = createSession(storage);
    const context = BACKGROUND_CONTEXT;
    const note = defineDoc({
        kind: "audit1075.versioned",
        version: 3,
        scope: "conversation",
        history: "rewindable",
        fork: "asOf",
        initial: () => ({ revision: 1 }),
    });
    const conversation = await session.commit((tx: any) => tx.createConversation({ ownership: { kind: "ownerless" } }), context);
    const conversationId = conversation.id;
    let selectedEntryId: number | undefined;
    await session.commit(async (tx: any) => {
        const selected = await tx.appendEntry(conversationId, {
            kind: "audit1075.filtered",
            selected: true,
            head: "self",
        });
        selectedEntryId = selected.id;
        const draft = await tx.doc(note, conversationId);
        draft.revision = 2;
    }, context);
    await session.commit(async (tx: any) => {
        await tx.appendEntry(conversationId, { kind: "audit1075.other" });
        const draft = await tx.doc(note, conversationId);
        draft.revision = 3;
    }, context);
    const currentOnly = defineDoc({
        kind: "audit1075.current-only",
        version: 1,
        scope: "conversation",
        history: "latest",
        initial: () => ({ revision: 1 }),
    });
    const currentOnlyId = await storage.mintId();
    const currentOnlyRecord = {
        id: currentOnlyId,
        kind: currentOnly.definition.kind,
        family: false,
        key: undefined,
        scope: { kind: "conversation", conversationId },
        createdAt: 0,
    };
    await storage.commit([{
        type: "document.create",
        record: currentOnlyRecord,
        content: { kind: "base", version: 1, value: { revision: 1 } },
    }], context);
    await storage.commit([{
        type: "document.change",
        id: currentOnlyId,
        content: { kind: "base", version: 1, value: { revision: 2 } },
    }], context);
    const Probe = defineTask<null, { phase: "ready" }, null>({
        name: "audit1075.read-probe",
        version: 1,
        initial: () => ({ phase: "ready" }),
        phases: { ready: async () => {} },
        abort: async () => {},
    });
    const taskId = await session.commit(
        (tx: any) => tx.createTask(Probe, null, { ownership: { kind: "conversation" }, conversationId }),
        context,
    );
    const requestId = "audit1075.request-1";
    await session.commit(
        (tx: any) => tx.createSubmission({ conversationId, requestId, type: "input", status: "queued" }),
        context,
    );
    const transactionReads = await session.commit(async (tx: any) => ({
        conversation: (await tx.conversation(conversationId)) ?? null,
        missingConversation: (await tx.conversation(999999)) ?? null,
        entry: (await tx.entry(selectedEntryId)) ?? null,
        missingEntry: (await tx.entry(999999)) ?? null,
        task: (await tx.task(taskId)) ?? null,
        missingTask: (await tx.task(999999)) ?? null,
        conversations: await tx.scanConversations({}, 10),
        entries: await tx.scanEntries({ conversationId, minEntryId: selectedEntryId, maxEntryId: selectedEntryId }, 10),
        tasks: await tx.scanTasks({
            conversationId,
            kind: "audit1075.read-probe",
            status: "pending",
            abortRequested: false,
            background: false,
        }, 10),
        latestHeadMarker: (await tx.latestHeadMarker(conversationId)) ?? null,
        submissionByRequest: (await tx.submissionByRequest(conversationId, requestId)) ?? null,
        missingSubmissionByRequest: (await tx.submissionByRequest(conversationId, "audit1075.missing")) ?? null,
    }), context);
    await session.close(context);

    const inspection = new DatabaseSync(path);
    const revisions = inspection.prepare("SELECT seq, kind, version, content FROM document_revisions ORDER BY seq").all();
    inspection.close();

    const contended = await openNodeSqliteStorage(path, { busyTimeoutMs: 30, walAutoCheckpointPages: 7 });
    const blocker = new DatabaseSync(path);
    blocker.exec("BEGIN IMMEDIATE");
    const startedAt = performance.now();
    let busyMessage = "";
    try {
        await contended.commit([{ type: "conversation", value: { id: 50 } }], undefined);
    } catch (error) {
        busyMessage = error instanceof Error ? error.message : String(error);
    }
    const elapsedMs = performance.now() - startedAt;
    await contended.close(undefined);
    blocker.exec("ROLLBACK");
    blocker.close();

    scenarios.push({
        name: "SQLite applies entry ID bounds and stores document revisions with configured busy and WAL options",
        inputs: { backend: "SqliteStorage", busyTimeoutMs: 30, walAutoCheckpointPages: 7 },
        expected: { transactionReads, connectionSettings, revisions, busyMessage, elapsedMs },
        normalizations: ["elapsedMs is expected to be at least the configured 30 ms busy timeout and below 2,000 ms"],
    });
} finally {
    rmSync(directory, { recursive: true, force: true });
}

const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline,
    revision,
    ticket: "1075",
    family,
    capturedAt,
    environment,
    sourceEndpoints,
    scenarios,
};
const artifactPath = "sqlite-transaction-options.json";
const artifactBytes = `${JSON.stringify(artifact, null, 2)}\n`;
const bundle = join(repository, "fixtures/pi-v1.3.0", family);
mkdirSync(bundle, { recursive: true });
writeFileSync(join(bundle, artifactPath), artifactBytes, "utf8");
const scriptRelativePath = "docs/research/pi-v1.3.0/capture-1075-options.mts";
const command = `PI_CHECKOUT=${checkout} PI_BASELINE=pi-v1.3.0 NODE_OPTIONS='--conditions=source' TSX_TSCONFIG_PATH=${join(checkout, "tsconfig.json")} ${tsx} ${scriptRelativePath}`;
const scriptDigest = sha256(readFileSync(scriptPath));
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline,
    revision,
    ticket: "1075",
    family,
    capturedAt,
    environment,
    generator: {
        path: scriptRelativePath,
        sha256: scriptDigest,
        command,
        sources: [{ path: scriptRelativePath, sha256: scriptDigest }],
    },
    artifacts: [{
        path: artifactPath,
        family,
        bytes: Buffer.byteLength(artifactBytes),
        sha256: sha256(artifactBytes),
        sourceEndpoints,
        scenarios,
    }],
};
writeFileSync(join(bundle, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
process.stdout.write(artifactBytes);
