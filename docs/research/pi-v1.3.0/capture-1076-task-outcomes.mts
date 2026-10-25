import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const repository = join(dirname(scriptPath), "../../..");
const family = "audit-1076";
const baseline = "pi-v1.3.0";
const revision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const candidateCheckouts = [
    process.env.PI_CHECKOUT,
    "/home/lansy/Work/github/coding-agent/pi",
    "/tmp/cch-1055-pi-frozen",
    "/tmp/cch-1054-pi-frozen",
].filter(Boolean) as string[];
const checkout = candidateCheckouts.find((c) => existsSync(join(c, "node_modules/.bin/tsx")));
if (!checkout) throw new Error("Could not find frozen pi checkout with tsx");

const tsx = join(checkout, "node_modules/.bin/tsx");
if (!existsSync(tsx)) throw new Error(`frozen checkout has no tsx executable: ${tsx}`);

if (process.env.AUDIT_1076_TSX !== "1") {
    execFileSync(tsx, [scriptPath, ...process.argv.slice(2)], {
        cwd: repository,
        env: {
            ...process.env,
            AUDIT_1076_TSX: "1",
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
const registryJson = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { revision?: string; bundles?: Record<string, unknown> }>;
};
const registeredBaseline = registryJson.baselines[baseline];
if (registeredBaseline?.revision !== revision) throw new Error(`${baseline} does not resolve to the expected revision`);
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
    "packages/durable/src/tasks.ts:defineTask",
    "packages/durable/src/harness/harness.ts:Harness.open,resume,waitForTask,abortTask",
    "packages/durable/src/harness/scheduler.ts:TaskScheduler",
    "packages/durable/src/session/transaction.ts:Transaction.createTask,setTask",
    "packages/durable/src/storage/sqlite/node.ts:openNodeSqliteStorage",
];

const scenarios: unknown[] = [];

// Dynamically import modules from checkout
const { MemoryStorage, Harness, defineTask, createRegistry } = await import(join(checkout, "packages/durable/src/index.ts"));
const { openNodeSqliteStorage } = await import(join(checkout, "packages/durable/src/storage/sqlite/node.ts"));
const { BACKGROUND_CONTEXT } = await import(join(checkout, "packages/chord/src/context/index.ts"));

// Scenario 1: Multi-phase counter task replaces checkpoint state and reports typed completed result
{
    const seen: number[] = [];
    const runtimes = new Set<unknown>();
    const counterTask = defineTask({
        name: "audit1076.counter",
        version: 1,
        initial: () => ({ phase: "count", n: 0 }),
        phases: {
            count: async (task: any, runtime: any, context: any) => {
                seen.push(task.state.checkpoint.n);
                runtimes.add(runtime);
                await runtime.commit((_tx: any, current: any) => {
                    const n = current.state.checkpoint.n + 1;
                    return n === task.input.to
                        ? { status: "terminal", outcome: { status: "completed", result: n } }
                        : { status: "running", checkpoint: { phase: "count", n } };
                }, context);
            },
        },
        abort: async (_task: any, runtime: any, context: any) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted" } }), context),
    });

    const reg = createRegistry();
    reg.install({ name: "test", tasks: [counterTask] });
    const storage = new MemoryStorage();
    const harness = await Harness.open(storage, { registry: reg }, BACKGROUND_CONTEXT);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(counterTask, { to: 3 }, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);

    scenarios.push({
        name: "a multi-phase task advances checkpoints and commits a completed terminal receipt",
        inputs: { task: "audit1076.counter", to: 3 },
        expected: {
            phasesSeen: seen,
            invocations: runtimes.size,
            result: receipt.state.outcome.result,
            status: receipt.state.status,
            outcomeStatus: receipt.state.outcome.status,
            startedAtPresent: typeof receipt.startedAt === "number",
            endedAtPresent: typeof receipt.endedAt === "number",
            terminalHasMemos: receipt.memos !== undefined,
        },
        normalizations: [
            "timestamps startedAt and endedAt are normalized to presence assertions",
            "task IDs are omitted",
        ],
    });
}

// Scenario 2: Task memo survives checkpoint replacement and is cleared on terminal settlement
{
    let runningMemosSeen: unknown;
    const memoTask = defineTask({
        name: "audit1076.memo",
        version: 1,
        initial: () => ({ phase: "p1" }),
        phases: {
            p1: async (task: any, runtime: any, context: any) => {
                await runtime.memo("requested", task.input.amount, context);
                await runtime.commit(
                    () => ({ status: "running", checkpoint: { phase: "p2" } }),
                    context,
                );
            },
            p2: async (task: any, runtime: any, context: any) => {
                runningMemosSeen = task.memos;
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "done" } }),
                    context,
                );
            },
        },
        abort: async () => {},
    });

    const reg = createRegistry();
    reg.install({ name: "test", tasks: [memoTask] });
    const storage = new MemoryStorage();
    const harness = await Harness.open(storage, { registry: reg }, BACKGROUND_CONTEXT);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(memoTask, { amount: 7 }, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);

    scenarios.push({
        name: "a task memo survives checkpoint replacement and is cleared on terminal settlement",
        inputs: { task: "audit1076.memo", amount: 7 },
        expected: {
            runningMemos: runningMemosSeen,
            terminalStatus: receipt.state.status,
            terminalHasMemos: receipt.memos !== undefined,
            outcomeStatus: receipt.state.outcome.status,
        },
    });
}

// Scenario 3: Task commits a typed failed outcome with error detail and partial result
{
    const failTask = defineTask({
        name: "audit1076.fail",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                await runtime.commit(
                    () => ({
                        status: "terminal",
                        outcome: {
                            status: "failed",
                            error: { message: "transfer declined", detail: { code: 402 } },
                            result: { partial: 42 },
                        },
                    }),
                    context,
                );
            },
        },
        abort: async () => {},
    });

    const reg = createRegistry();
    reg.install({ name: "test", tasks: [failTask] });
    const storage = new MemoryStorage();
    const harness = await Harness.open(storage, { registry: reg }, BACKGROUND_CONTEXT);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(failTask, {}, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);

    scenarios.push({
        name: "a multi-phase task commits a typed failed outcome with error and partial result",
        inputs: { task: "audit1076.fail" },
        expected: {
            status: receipt.state.status,
            outcome: receipt.state.outcome,
        },
    });
}

// Scenario 4: abortTask marks running task and runs abort handler
{
    let abortRunSignalled = false;
    let abortHandlerRan = false;
    const abortTask = defineTask({
        name: "audit1076.abort",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, _context: any) => {
                await new Promise<void>((resolve) => {
                    if (runtime.signal.aborted) {
                        abortRunSignalled = true;
                        resolve();
                        return;
                    }
                    runtime.signal.addEventListener("abort", () => {
                        abortRunSignalled = true;
                        resolve();
                    });
                });
            },
        },
        abort: async (_task: any, runtime: any, context: any) => {
            abortHandlerRan = true;
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "requested" } }),
                context,
            );
        },
    });

    const reg = createRegistry();
    reg.install({ name: "test", tasks: [abortTask] });
    const storage = new MemoryStorage();
    const harness = await Harness.open(storage, { registry: reg }, BACKGROUND_CONTEXT);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(abortTask, {}, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    // Yield to let the task start running
    await new Promise((r) => setTimeout(r, 20));
    const abortResult = await harness.abortTask(id, BACKGROUND_CONTEXT);
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);

    scenarios.push({
        name: "abortTask marks a running task and runs its abort handler to commit an aborted outcome",
        inputs: { task: "audit1076.abort" },
        expected: {
            abortResult,
            abortRunSignalled,
            abortHandlerRan,
            status: receipt.state.status,
            outcome: receipt.state.outcome,
            abortRequested: receipt.abortRequested,
        },
    });
}

// Scenario 5: aborting a task with a missing definition commits orphaned outcome
{
    const dir = mkdtempSync(join(tmpdir(), "cch-audit1076-orphan-"));
    try {
        const dbPath = join(dir, "durable.sqlite");
        const placeholderTask = defineTask({
            name: "audit1076.orphan",
            version: 1,
            initial: () => ({ phase: "run" }),
            phases: { run: async () => {} },
            abort: async () => {},
        });
        const regWithTask = createRegistry();
        regWithTask.install({ name: "test", tasks: [placeholderTask] });

        let storage = await openNodeSqliteStorage(dbPath);
        let harness = await Harness.open(storage, { registry: regWithTask }, BACKGROUND_CONTEXT);
        const root = await harness.root(BACKGROUND_CONTEXT);
        const id = await root.commit(
            (tx: any) => tx.createTask(placeholderTask, {}, { ownership: { kind: "conversation" } }),
            BACKGROUND_CONTEXT,
        );
        await harness.close(BACKGROUND_CONTEXT);

        // Reopen without task registered
        storage = await openNodeSqliteStorage(dbPath);
        const emptyReg = createRegistry();
        harness = await Harness.open(storage, { registry: emptyReg }, BACKGROUND_CONTEXT);
        harness.resume();
        const abortResult = await harness.abortTask(id, BACKGROUND_CONTEXT);
        const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
        await harness.close(BACKGROUND_CONTEXT);

        scenarios.push({
            name: "aborting a task with a missing definition commits an orphaned outcome",
            inputs: { task: "audit1076.orphan" },
            expected: {
                abortResult,
                status: receipt.state.status,
                outcome: receipt.state.outcome,
            },
        });
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
}

// Scenario 6: A phase returning without durable progress faults the task
{
    const noProgressTask = defineTask({
        name: "audit1076.no-progress",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async () => {
                // does not commit durable progress
            },
        },
        abort: async () => {},
    });

    const reg = createRegistry();
    reg.install({ name: "test", tasks: [noProgressTask] });
    const storage = new MemoryStorage();
    const harness = await Harness.open(storage, { registry: reg }, BACKGROUND_CONTEXT);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(noProgressTask, {}, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);

    scenarios.push({
        name: "a phase returning without durable progress faults the task",
        inputs: { task: "audit1076.no-progress" },
        expected: {
            status: receipt.state.status,
            outcomeStatus: receipt.state.outcome.status,
            errorMessage: receipt.state.outcome.error.message,
        },
    });
}

// Scenario 7: Resuming an interrupted task continues from its durable checkpoint without rerunning completed phases
{
    const dir = mkdtempSync(join(tmpdir(), "cch-audit1076-resume-"));
    try {
        const dbPath = join(dir, "durable.sqlite");
        const log: string[] = [];

        let interruptFirst = true;
        const resumeTask = defineTask({
            name: "audit1076.resume",
            version: 1,
            initial: () => ({ phase: "p1" }),
            phases: {
                p1: async (_task: any, runtime: any, context: any) => {
                    log.push("p1");
                    await runtime.commit(
                        () => ({ status: "running", checkpoint: { phase: "p2", n: 1 } }),
                        context,
                    );
                },
                p2: async (task: any, runtime: any, context: any) => {
                    if (interruptFirst) {
                        interruptFirst = false;
                        await new Promise((_, reject) => {
                            if (runtime.signal.aborted) reject(runtime.signal.reason);
                            runtime.signal.addEventListener("abort", () => reject(runtime.signal.reason), { once: true });
                        });
                    }
                    log.push(`p2:${task.state.checkpoint.n}`);
                    await runtime.commit(
                        () => ({
                            status: "terminal",
                            outcome: { status: "completed", result: task.state.checkpoint.n + 10 },
                        }),
                        context,
                    );
                },
            },
            abort: async () => {},
        });

        const reg = createRegistry();
        reg.install({ name: "test", tasks: [resumeTask] });

        // Phase 1 execution
        let storage = await openNodeSqliteStorage(dbPath);
        let harness = await Harness.open(storage, { registry: reg }, BACKGROUND_CONTEXT);
        const root = await harness.root(BACKGROUND_CONTEXT);
        const id = await root.commit(
            (tx: any) => tx.createTask(resumeTask, {}, { ownership: { kind: "conversation" } }),
            BACKGROUND_CONTEXT,
        );

        // Run until p1 is committed
        harness.resume();
        await new Promise((r) => setTimeout(r, 20));
        await harness.close(BACKGROUND_CONTEXT);

        // Now reopen and run to completion
        const reopenedLog: string[] = [];
        const resumeTaskReopen = defineTask({
            name: "audit1076.resume",
            version: 1,
            initial: () => ({ phase: "p1" }),
            phases: {
                p1: async () => {
                    reopenedLog.push("p1-unexpected");
                },
                p2: async (task: any, runtime: any, context: any) => {
                    reopenedLog.push(`p2:${task.state.checkpoint.n}`);
                    await runtime.commit(
                        () => ({
                            status: "terminal",
                            outcome: { status: "completed", result: task.state.checkpoint.n + 10 },
                        }),
                        context,
                    );
                },
            },
            abort: async () => {},
        });
        const reopenedReg = createRegistry();
        reopenedReg.install({ name: "test", tasks: [resumeTaskReopen] });

        storage = await openNodeSqliteStorage(dbPath);
        harness = await Harness.open(storage, { registry: reopenedReg }, BACKGROUND_CONTEXT);
        await harness.openTasks(BACKGROUND_CONTEXT);
        harness.resume();
        const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
        await harness.close(BACKGROUND_CONTEXT);

        scenarios.push({
            name: "resuming an interrupted task continues from its durable checkpoint without rerunning completed phases",
            inputs: { task: "audit1076.resume" },
            expected: {
                reopenedPhasesRan: reopenedLog,
                status: receipt.state.status,
                result: receipt.state.outcome.result,
            },
        });
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
}

// Scenario 8: Terminal record and receipt commit in the same atomic storage batch
{
    class TrackingStorage extends MemoryStorage {
        readonly commits: any[] = [];
        override async commit(writes: readonly any[], context: any) {
            this.commits.push(structuredClone(writes));
            return super.commit(writes, context);
        }
    }

    const atomicTask = defineTask({
        name: "audit1076.atomic",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                await runtime.commit(
                    () => ({
                        status: "terminal",
                        outcome: { status: "completed", result: { receiptId: 999 } },
                    }),
                    context,
                );
            },
        },
        abort: async () => {},
    });

    const reg = createRegistry();
    reg.install({ name: "test", tasks: [atomicTask] });
    const storage = new TrackingStorage();
    const harness = await Harness.open(storage, { registry: reg }, BACKGROUND_CONTEXT);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(atomicTask, {}, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);

    // Find the terminal commit in storage
    const terminalBatches = storage.commits.filter((batch: any[]) =>
        batch.some((w: any) => w.type === "task" && w.value?.id === id && w.value?.state?.status === "terminal"),
    );

    scenarios.push({
        name: "terminal record and result receipt commit in the same atomic storage batch",
        inputs: { task: "audit1076.atomic" },
        expected: {
            terminalBatchCount: terminalBatches.length,
            batchHasRecordAndOutcome:
                terminalBatches.length === 1 &&
                terminalBatches[0].some(
                    (w: any) =>
                        w.type === "task" &&
                        w.value?.id === id &&
                        w.value?.state?.status === "terminal" &&
                        w.value?.state?.outcome?.status === "completed",
                ),
            terminalResult: receipt.state.outcome.result,
        },
    });
}

const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline,
    revision,
    ticket: "1076",
    family,
    capturedAt,
    environment,
    sourceEndpoints,
    scenarios,
};

const artifactPath = "task-outcomes.json";
const artifactBytes = `${JSON.stringify(artifact, null, 2)}\n`;
const bundle = join(repository, "fixtures/pi-v1.3.0", family);
mkdirSync(bundle, { recursive: true });
writeFileSync(join(bundle, artifactPath), artifactBytes, "utf8");

const scriptRelativePath = "docs/research/pi-v1.3.0/capture-1076-task-outcomes.mts";
const command = `PI_CHECKOUT=${checkout} PI_BASELINE=pi-v1.3.0 NODE_OPTIONS='--conditions=source' TSX_TSCONFIG_PATH=${join(checkout, "tsconfig.json")} ${tsx} ${scriptRelativePath}`;
const scriptDigest = sha256(readFileSync(scriptPath));

const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline,
    revision,
    ticket: "1076",
    family,
    capturedAt,
    environment,
    generator: {
        path: scriptRelativePath,
        sha256: scriptDigest,
        command,
        sources: [{ path: scriptRelativePath, sha256: scriptDigest }],
    },
    artifacts: [
        {
            path: artifactPath,
            family,
            bytes: Buffer.byteLength(artifactBytes),
            sha256: sha256(artifactBytes),
            sourceEndpoints,
            scenarios: scenarios.map((s: any) => ({
                name: s.name,
                inputs: s.inputs,
                expected: s.expected,
                ...(s.normalizations ? { normalizations: s.normalizations } : {}),
            })),
        },
    ],
};

const manifestBytes = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(bundle, "manifest.json"), manifestBytes, "utf8");
console.log(`Captured ${scenarios.length} scenarios into ${join(bundle, artifactPath)}`);
