/**
 * Frozen pi v1.3.0 observations for `audit-1055`: durable tasks, submissions, inbox admission,
 * ownership/abort, versioning and crash recovery.
 *
 * Every scenario drives public `Harness`/`Conversation`/`Submission` calls (plus the public `Storage`
 * seam where admission atomicity is the property) and returns one comparison unit for the manifest.
 * The killed-process scenarios spawn `crash-worker.mts` and SIGKILL it at a named boundary; the
 * worker and this file share task definitions through `audit-tasks.mts`.
 */

import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
    BACKGROUND_CONTEXT,
    ConversationBusy,
    fauxAssistantMessage,
    fauxText,
    defineExtension,
    defineTask,
    hook,
    Harness,
    InboxDoc,
    LiveDoc,
    MemoryStorage,
    type Scenario,
} from "./pi.mts";
import {
    abortCrashTask,
    counterTask,
    handoverV1,
    handoverV2,
    holdChild,
    holdParent,
    oneShotTask,
    openChat,
    openHarness,
    openSqlite,
    taskOfKind,
    transferTask,
    untilAborted,
    versionedTask,
} from "./audit-tasks.mts";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WORKER = path.join(HERE, "crash-worker.mts");

export function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

function errorRecord(error: unknown): Record<string, unknown> {
    return error instanceof Error
        ? { name: error.name, message: error.message }
        : { name: "NonError", message: String(error) };
}

async function sleep(ms: number): Promise<void> {
    await new Promise((resolve) => setTimeout(resolve, ms));
}

async function waitUntil(check: () => boolean | Promise<boolean>, label: string, timeoutMs = 5000): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!(await check())) {
        if (Date.now() > deadline) throw new Error(`Condition was not reached: ${label}`);
        await sleep(2);
    }
}

function textOf(entry: any): string | undefined {
    const message = entry.model?.[0];
    if (message?.role === "system") return undefined;
    if (typeof message?.content === "string") return message.content;
    const text = message?.content?.find((content: any) => content.type === "text");
    return text?.type === "text" ? text.text : undefined;
}

function transcript(entries: readonly any[]): string[] {
    return entries
        .filter((entry) => entry.kind !== "pi.system")
        .map((entry) => {
            const text = textOf(entry);
            return text === undefined ? entry.kind : `${entry.kind}:${text}`;
        });
}

async function entriesOf(conversation: any): Promise<any[]> {
    const page = await conversation.entries({}, 100, undefined, BACKGROUND_CONTEXT);
    return [...page.items].reverse();
}

function answer(text: string) {
    return fauxAssistantMessage([fauxText(text)]);
}

function heldStep(reached: () => void) {
    let release!: () => void;
    const gate = new Promise<void>((resolve) => {
        release = resolve;
    });
    const step = async (_request: unknown, options: any) => {
        reached();
        await Promise.race([
            gate,
            new Promise<never>((_, reject) =>
                options!.signal!.addEventListener("abort", () => reject(options!.signal!.reason), { once: true }),
            ),
        ]);
        return answer("first");
    };
    return { step, release };
}

class RecordingStorage extends MemoryStorage {
    readonly batches: any[][] = [];

    override async commit(writes: readonly any[], context: any): Promise<any> {
        this.batches.push(structuredClone(writes));
        return super.commit(writes, context);
    }
}

// ---------------------------------------------------------------------------------------------------
// Task lifecycle: phases, checkpoints, lifecycle times, memos, completing hold and hooks.
// ---------------------------------------------------------------------------------------------------

export async function counterLifecycle(): Promise<Scenario> {
    const seen: number[] = [];
    const runtimes = new Set<unknown>();
    const Counter = counterTask(seen, runtimes);
    const { harness } = await openHarness(new MemoryStorage(), [Counter], { now: () => 1000 });
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Counter, { to: 3 }, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    const result = receipt.state.outcome.status === "completed" ? receipt.state.outcome.result : undefined;
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a multi-phase task replaces checkpoint state and reports a typed terminal result",
        source: "packages/durable/src/harness/scheduler.ts:TaskScheduler,#runPhases",
        expected: {
            phasesSeen: seen,
            invocations: runtimes.size,
            result,
            record: { kind: receipt.kind, version: receipt.version, status: receipt.state.status },
        },
    };
}

export async function lifecycleTimesAcrossWait(): Promise<Scenario> {
    const log: string[] = [];
    let clock = 1000;
    let releaseChild!: () => void;
    const childGate = new Promise<void>((resolve) => {
        releaseChild = resolve;
    });
    let releaseClock!: () => void;
    const clockGate = new Promise<void>((resolve) => {
        releaseClock = resolve;
    });
    const Child = holdChild(log, childGate);
    const Clock = (() => {
        // A two-phase task: p1 parks in a durable wait on the child; p2 commits the terminal result.
        return defineTask({
            name: "audit.clock",
            version: 1,
            initial: () => ({ phase: "p1" }),
            phases: {
                p1: async (task: any, runtime: any, context: any) => {
                    log.push("clock-p1");
                    await runtime.commit(
                        () => ({
                            status: "waiting",
                            checkpoint: { phase: "p2" },
                            on: [task.input.child],
                            policy: "allSettled",
                        }),
                        context,
                    );
                },
                p2: async (_task: any, runtime: any, context: any) => {
                    log.push("clock-p2");
                    await clockGate;
                    await runtime.commit(
                        () => ({ status: "terminal", outcome: { status: "completed", result: "done" } }),
                        context,
                    );
                },
            },
            abort: async (_task: any, runtime: any, context: any) =>
                runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted" } }), context),
        });
    })();
    const { harness } = await openHarness(new MemoryStorage(), [Child, Clock], { now: () => clock });
    const root = await harness.root(BACKGROUND_CONTEXT);
    const childId = await root.commit(
        (tx: any) => tx.createTask(Child, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    const clockId = await root.commit(
        (tx: any) => tx.createTask(Clock, { child: childId }, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(() => log.includes("clock-p1"), "clock p1");
    await waitUntil(async () => (await harness.getTask(clockId, BACKGROUND_CONTEXT))?.state.status === "waiting", "clock waiting");
    const waitingStatus = (await harness.getTask(clockId, BACKGROUND_CONTEXT)).state.status;
    clock = 3000;
    releaseChild();
    await waitUntil(() => log.includes("clock-p2"), "clock p2");
    clock = 4000;
    releaseClock();
    const receipt = await harness.waitForTask(clockId, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "task lifecycle times come from the Harness clock and survive a durable wait",
        source: "packages/durable/src/harness/scheduler.ts:TaskScheduler,#stampLifecycleTimes",
        expected: {
            startedAt: receipt.startedAt,
            endedAt: receipt.endedAt,
            terminal: receipt.state.status,
            waitingStatus,
        },
    };
}

export async function memoAcrossCheckpoint(): Promise<Scenario> {
    let release!: () => void;
    const gate = new Promise<void>((resolve) => {
        release = resolve;
    });
    const Memo = defineTask({
        name: "audit.memo",
        version: 1,
        initial: () => ({ phase: "prepare" }),
        phases: {
            prepare: async (_task: any, runtime: any, context: any) => {
                await runtime.memo("requested", 7, context);
                await runtime.commit(
                    () => ({ status: "running", checkpoint: { phase: "apply" } }),
                    context,
                );
            },
            apply: async (_task: any, runtime: any, context: any) => {
                await gate;
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "done" } }),
                    context,
                );
            },
        },
        abort: async (_task: any, runtime: any, context: any) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted" } }), context),
    });
    const { harness } = await openHarness(new MemoryStorage(), [Memo]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Memo, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(async () => {
        const task = await harness.getTask(id, BACKGROUND_CONTEXT);
        return task?.state.status === "running" && task?.memos !== undefined;
    }, "memo committed");
    const running = await harness.getTask(id, BACKGROUND_CONTEXT);
    release();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a task memo survives checkpoint replacement and is absent from the terminal record",
        source: "packages/durable/src/harness/scheduler.ts:TaskRuntime.memo",
        expected: {
            runningMemos: running.memos,
            runningCheckpoint: running.state.checkpoint,
            terminalStatus: receipt.state.status,
            terminalHasMemos: Object.prototype.hasOwnProperty.call(receipt, "memos"),
        },
    };
}

export async function completingHold(): Promise<Scenario> {
    const log: string[] = [];
    let release!: () => void;
    const gate = new Promise<void>((resolve) => {
        release = resolve;
    });
    const Parent = holdParent(log);
    const Child = holdChild(log, gate);
    const { harness } = await openHarness(new MemoryStorage(), [Parent, Child]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Parent, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(() => log.includes("parent-commit-completed"), "parent completed commit");
    await sleep(20);
    const before = (await harness.inspect(BACKGROUND_CONTEXT)).tasks.map((task: any) => ({
        kind: task.record.kind,
        recordStatus: task.record.state.status,
        inspection: task.state.kind,
    }));
    release();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    const child = await taskOfKind(harness, "audit.hold-child");
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a completing owner holds its terminal outcome until ordinary owned work drains",
        source: "packages/durable/src/harness/scheduler.ts:#finalizeCompleting",
        expected: {
            before,
            parentOutcome: receipt.state.outcome,
            childOutcome: child.state.outcome,
        },
    };
}

export async function hooksOrderAndIsolation(): Promise<Scenario> {
    const log: string[] = [];
    const reports: string[] = [];
    const Hooked = defineTask({
        name: "audit.hooked",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                await runtime.hooks.each("onStep", (handler: any) => handler());
                log.push("task");
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "done" } }),
                    context,
                );
            },
        },
        abort: async (_task: any, runtime: any, context: any) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted" } }), context),
        hooks: { onStep() {} },
    });
    const { harness, registry } = await openHarness(new MemoryStorage(), [Hooked], {
        onReport: (error) => reports.push((error as Error).message),
    });
    registry.install(
        defineExtension({
            name: "hook-throw",
            hooks: [
                hook(Hooked, {
                    onStep: () => {
                        log.push("hook-1");
                        throw new Error("hook boom");
                    },
                }),
            ],
        }),
    );
    registry.install(
        defineExtension({
            name: "hook-observer",
            hooks: [hook(Hooked, { onStep: () => log.push("hook-2") })],
        }),
    );
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Hooked, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "task hooks run in extension order and an ordinary throw is reported while the task continues",
        source: "packages/durable/src/harness/scheduler.ts:TaskRuntime.hooks,#runPhases",
        expected: { log, reports, outcome: receipt.state.outcome },
    };
}

// ---------------------------------------------------------------------------------------------------
// Versioning, definition replacement, storage failure and inadmissible ownership.
// ---------------------------------------------------------------------------------------------------

export async function missingDefinition(): Promise<Scenario> {
    const Versioned = versionedTask(1, "v1");
    const { harness, registry } = await openHarness(new MemoryStorage(), [Versioned]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Versioned, { n: 1 }, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    registry.install(defineExtension({ name: "audit", tasks: [] }));
    harness.resume();
    await sleep(50);
    const blocked = (await harness.inspect(BACKGROUND_CONTEXT)).tasks[0];
    const record = await harness.getTask(id, BACKGROUND_CONTEXT);
    registry.install(defineExtension({ name: "audit", tasks: [Versioned] }));
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a task with no installed definition stays pending and runs when its definition returns",
        source: "packages/durable/src/harness/scheduler.ts:#blockedReason",
        expected: {
            inspection: blocked.state.kind,
            reason: blocked.state.reason,
            recordStatus: record.state.status,
            outcome: receipt.state.outcome,
        },
    };
}

export async function allSettledJoin(): Promise<Scenario> {
    const log: string[] = [];
    const Child = defineTask<{ name: string; fail?: boolean; wait?: boolean }, { phase: "run" }, string>({
        name: "audit.join-child",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (task: any, runtime: any, context: any) => {
                log.push(`child-run-${task.input.name}`);
                if (task.input.wait) await sleep(40);
                if (task.input.fail) {
                    await runtime.commit(
                        () => ({
                            status: "terminal",
                            outcome: { status: "failed", error: { name: "Error", message: "boom" } },
                        }),
                        context,
                    );
                } else {
                    await runtime.commit(
                        () => ({ status: "terminal", outcome: { status: "completed", result: task.input.name } }),
                        context,
                    );
                }
            },
        },
        abort: async (task: any, runtime: any, context: any) => {
            log.push(`child-abort-${task.input.name}`);
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "cascade" } }),
                context,
            );
        },
    });
    const Parent = defineTask<null, { phase: "spawn" } | { phase: "decide"; ids: any[] }, string>({
        name: "audit.join-parent",
        version: 1,
        initial: () => ({ phase: "spawn" }),
        phases: {
            spawn: async (task: any, runtime: any, context: any) => {
                log.push("parent-spawn");
                const spawn = async (input: { name: string; fail?: boolean; wait?: boolean }) => {
                    let id: any;
                    await runtime.commit(async (tx: any) => {
                        id = await tx.createTask(Child, input, {
                            ownership: { kind: "task", taskId: runtime.taskId },
                        });
                        return undefined;
                    }, context);
                    return id;
                };
                const ids = [await spawn({ name: "b", fail: true }), await spawn({ name: "c", wait: true })];
                await runtime.commit(
                    () => ({ status: "waiting", checkpoint: { phase: "decide", ids }, on: ids, policy: "allSettled" }),
                    context,
                );
                log.push("parent-waiting");
            },
            decide: async (task: any, runtime: any, context: any) => {
                log.push("parent-decide");
                const outcomes = await runtime.outcomes(task.state.checkpoint.ids, context);
                await runtime.commit(
                    () => ({
                        status: "terminal",
                        outcome: { status: "completed", result: outcomes.map((outcome: any) => outcome.status) },
                    }),
                    context,
                );
            },
        },
        abort: async (_task: any, runtime: any, context: any) => {
            log.push("parent-abort");
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "parent" } }),
                context,
            );
        },
    });
    const { harness } = await openHarness(new MemoryStorage(), [Parent, Child]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Parent, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    const records = await harness.commit((tx: any) => tx.scanTasks({}, 10), BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "an allSettled join lets surviving siblings finish and still reads ordered outcomes",
        source: "packages/durable/test/harness-ownership.test.ts:allSettled",
        expected: {
            log,
            parent: receipt.state.outcome,
            children: records.items
                .filter((task: any) => task.kind === "audit.join-child")
                .map((task: any) => ({
                    input: task.input.name,
                    outcome: task.state.outcome.status,
                    abortRequested: task.abortRequested,
                })),
        },
    };
}

export async function migrationBeforeReservation(): Promise<Scenario> {
    const log: string[] = [];
    const migrate = (input: any, checkpoint: any, fromVersion: number) => ({
        input: { n: input.n + fromVersion },
        checkpoint: { phase: "run", n: checkpoint.n + 10 },
    });
    const Versioned1 = versionedTask(1, "v1");
    const Versioned2 = versionedTask(2, "v2", migrate, log);
    const { harness, registry } = await openHarness(new MemoryStorage(), [Versioned1]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Versioned1, { n: 1 }, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    const before = (await harness.inspect(BACKGROUND_CONTEXT)).tasks.map((task: any) => task.state);
    registry.install(defineExtension({ name: "audit", tasks: [Versioned2] }));
    const afterInstall = (await harness.inspect(BACKGROUND_CONTEXT)).tasks.map((task: any) => task.state);
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "reservation migrates a stored definition before running the newer version",
        source: "packages/durable/src/harness/scheduler.ts:#reserve",
        expected: {
            before,
            afterInstall,
            log,
            version: receipt.version,
            input: receipt.input,
            outcome: receipt.state.outcome,
        },
    };
}

export async function versionTooOld(): Promise<Scenario> {
    const Versioned1 = versionedTask(1, "v1");
    const Versioned2 = versionedTask(2, "v2", (input: any, checkpoint: any) => ({ input, checkpoint }));
    const { harness, registry } = await openHarness(new MemoryStorage(), [Versioned2]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Versioned2, { n: 1 }, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    registry.install(defineExtension({ name: "audit", tasks: [Versioned1] }));
    harness.resume();
    await sleep(50);
    const blocked = (await harness.inspect(BACKGROUND_CONTEXT)).tasks[0];
    const record = await harness.getTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a task whose stored version is newer than the installed definition stays pending and blocked",
        source: "packages/durable/src/harness/scheduler.ts:#blockedReason",
        expected: {
            inspection: blocked.state.kind,
            reason: blocked.state.reason,
            recordStatus: record.state.status,
            checkpoint: record.state.checkpoint,
        },
    };
}

export async function migrationFailureAndRecovery(): Promise<Scenario> {
    const reports: string[] = [];
    const Versioned1 = versionedTask(1, "v1");
    const Broken = versionedTask(2, "broken", () => {
        throw new Error("cannot migrate");
    });
    const Good = versionedTask(2, "v2", (input: any, checkpoint: any) => ({ input, checkpoint }));
    const { harness, registry } = await openHarness(new MemoryStorage(), [Versioned1], {
        onReport: (error) => reports.push((error as Error).message),
    });
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Versioned1, { n: 1 }, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    registry.install(defineExtension({ name: "audit", tasks: [Broken] }));
    harness.resume();
    await sleep(50);
    const blocked = (await harness.inspect(BACKGROUND_CONTEXT)).tasks[0];
    registry.install(defineExtension({ name: "audit", tasks: [Good] }));
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a failed migration is reported and leaves the task pending until a usable definition is installed",
        source: "packages/durable/src/harness/scheduler.ts:#reserve",
        expected: {
            blocked: blocked.state.kind,
            reason: blocked.state.reason,
            reports,
            recoveredOutcome: receipt.state.outcome,
        },
    };
}

export async function definitionHandover(): Promise<Scenario> {
    const log: string[] = [];
    let release!: () => void;
    const gate = new Promise<void>((resolve) => {
        release = resolve;
    });
    const V1 = handoverV1(gate, log);
    const V2 = handoverV2(log);
    const { harness, registry } = await openHarness(new MemoryStorage(), [V1]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(V1, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(() => log.includes("v1-p1"), "v1 p1");
    registry.install(defineExtension({ name: "audit", tasks: [V2] }));
    release();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a running task hands over to a newer definition at a phase boundary",
        source: "packages/durable/src/harness/scheduler.ts:#decide",
        expected: { log, version: receipt.version, outcome: receipt.state.outcome },
    };
}

export async function inadmissibleOwnership(): Promise<Scenario> {
    const Parent = holdParent([]);
    const Child = holdChild([]);
    const { harness } = await openHarness(new MemoryStorage(), [Parent, Child]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const parent = await root.commit(
        (tx: any) => tx.createTask(Parent, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    await harness.waitForTask(parent, BACKGROUND_CONTEXT);
    const other = await harness.createConversation({ ownership: { kind: "ownerless" } }, BACKGROUND_CONTEXT);
    const errors: Record<string, string> = {};
    const attempt = async (name: string, run: (tx: any) => Promise<unknown>) => {
        try {
            await root.commit(async (tx: any) => {
                await run(tx);
            }, BACKGROUND_CONTEXT);
            errors[name] = "accepted";
        } catch (error) {
            errors[name] = (error as Error).message;
        }
    };
    await attempt("child_of_terminal", (tx) =>
        tx.createTask(Child, null, { ownership: { kind: "task", taskId: parent } }),
    );
    await attempt("child_of_missing", (tx) =>
        tx.createTask(Child, null, { ownership: { kind: "task", taskId: 999_999 } }),
    );
    await attempt("background_child", (tx) =>
        tx.createTask(Child, null, { ownership: { kind: "task", taskId: parent }, background: true }),
    );
    await attempt("child_other_conversation", (tx) =>
        tx.createTask(Child, null, { ownership: { kind: "task", taskId: parent }, conversationId: other.id }),
    );
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "inadmissible child ownership is rejected by the transaction",
        source: "packages/durable/src/session/transaction.ts:Transaction.createTask",
        expected: errors,
    };
}

export async function storageFailureRecovery(): Promise<Scenario> {
    const log: string[] = [];
    const reports: string[] = [];
    const Once = oneShotTask("audit.storage-failure", "ok", log);
    const directory = mkdtempSync(path.join(tmpdir(), "pi-1055-storage-"));
    const database = path.join(directory, "session.sqlite");
    try {
        const real = await openSqlite(database);
        let fail = false;
        const failing = new Proxy(real, {
            get(target, property) {
                if (property === "commit") {
                    return async (writes: any, context: any) => {
                        if (fail) {
                            fail = false;
                            throw new Error("injected storage failure");
                        }
                        return target.commit(writes, context);
                    };
                }
                const value = Reflect.get(target, property, target);
                return typeof value === "function" ? value.bind(target) : value;
            },
        });
        const { harness } = await openHarness(failing as any, [Once], {
            onReport: (error) => reports.push((error as Error).message),
        });
        const root = await harness.root(BACKGROUND_CONTEXT);
        const id = await root.commit(
            (tx: any) => tx.createTask(Once, null, { ownership: { kind: "conversation" } }),
            BACKGROUND_CONTEXT,
        );
        fail = true;
        harness.resume();
        let failure: Record<string, unknown>;
        try {
            await harness.waitForTask(id, BACKGROUND_CONTEXT);
            failure = { name: "None", message: "wait unexpectedly succeeded" };
        } catch (error) {
            failure = errorRecord(error);
        }
        await harness.close(BACKGROUND_CONTEXT);
        const reopened = await openHarness(await openSqlite(database), [Once]);
        const state = await reopened.harness.getTask(id, BACKGROUND_CONTEXT);
        reopened.harness.resume();
        const receipt = await reopened.harness.waitForTask(id, BACKGROUND_CONTEXT);
        await reopened.harness.close(BACKGROUND_CONTEXT);
        return {
            name: "a storage failure at the terminal commit fails the Session and the task reruns after reopen",
            source: "packages/durable/src/session/session.ts:HarnessImpl.fail",
            expected: {
                failure,
                reopenedStatus: state.state.status,
                reopenedCheckpoint: state.state.checkpoint,
                finalOutcome: receipt.state.outcome,
                reports,
            },
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

// ---------------------------------------------------------------------------------------------------
// Ownership, joins and cancellation: failFast, abort ordering, and wait cancellation versus abort.
// ---------------------------------------------------------------------------------------------------

export async function failFastJoin(): Promise<Scenario> {
    const log: string[] = [];
    const Child = defineTask<{ name: string; fail?: boolean; wait?: boolean }, { phase: "run" }, string>({
        name: "audit.join-child",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (task: any, runtime: any, context: any) => {
                log.push(`child-run-${task.input.name}`);
                if (task.input.wait) await sleep(40);
                if (task.input.fail) {
                    await runtime.commit(
                        () => ({
                            status: "terminal",
                            outcome: { status: "failed", error: { name: "Error", message: "boom" } },
                        }),
                        context,
                    );
                } else {
                    await runtime.commit(
                        () => ({ status: "terminal", outcome: { status: "completed", result: task.input.name } }),
                        context,
                    );
                }
            },
        },
        abort: async (task: any, runtime: any, context: any) => {
            log.push(`child-abort-${task.input.name}`);
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "cascade" } }),
                context,
            );
        },
    });
    const Parent = defineTask<null, { phase: "spawn" } | { phase: "decide"; ids: any[] }, string>({
        name: "audit.join-parent",
        version: 1,
        initial: () => ({ phase: "spawn" }),
        phases: {
            spawn: async (task: any, runtime: any, context: any) => {
                log.push("parent-spawn");
                const spawn = async (input: { name: string; fail?: boolean; wait?: boolean }) => {
                    let id: any;
                    await runtime.commit(async (tx: any) => {
                        id = await tx.createTask(Child, input, {
                            ownership: { kind: "task", taskId: runtime.taskId },
                        });
                        return undefined;
                    }, context);
                    return id;
                };
                const ids = [
                    await spawn({ name: "a", fail: true }),
                    await spawn({ name: "b", wait: true }),
                    await spawn({ name: "c", wait: true }),
                ];
                await runtime.commit(
                    () => ({ status: "waiting", checkpoint: { phase: "decide", ids }, on: ids, policy: "failFast" }),
                    context,
                );
                log.push("parent-waiting");
            },
            decide: async (task: any, runtime: any, context: any) => {
                log.push("parent-decide");
                const outcomes = await runtime.outcomes(task.state.checkpoint.ids, context);
                await runtime.commit(
                    () => ({
                        status: "terminal",
                        outcome: { status: "completed", result: outcomes.map((outcome: any) => outcome.status) },
                    }),
                    context,
                );
            },
        },
        abort: async (_task: any, runtime: any, context: any) => {
            log.push("parent-abort");
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "parent" } }),
                context,
            );
        },
    });
    const { harness } = await openHarness(new MemoryStorage(), [Parent, Child]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Parent, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    const records = await harness.commit((tx: any) => tx.scanTasks({}, 10), BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a failFast join aborts live siblings and the parent reads ordered terminal outcomes",
        source: "packages/durable/test/harness-ownership.test.ts:failFast",
        expected: {
            log,
            parent: receipt.state.outcome,
            children: records.items
                .filter((task: any) => task.kind === "audit.join-child")
                .map((task: any) => ({
                    input: task.input.name,
                    outcome: task.state.outcome.status,
                    abortRequested: task.abortRequested,
                })),
        },
    };
}

export async function abortCascadeBottomUp(): Promise<Scenario> {
    const log: string[] = [];
    const Child = defineTask<null, { phase: "run" }, string>({
        name: "audit.cascade-child",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, _context: any) => {
                log.push("child-run");
                await untilAborted(runtime.signal);
                log.push("child-signalled");
            },
        },
        abort: async (_task: any, runtime: any, context: any) => {
            log.push("child-abort");
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "cascade" } }),
                context,
            );
        },
    });
    const Parent = defineTask<null, { phase: "run"; child?: any }, string>({
        name: "audit.cascade-parent",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                log.push("parent-run");
                let child: any;
                await runtime.commit(async (tx: any) => {
                    child = await tx.createTask(Child, null, {
                        ownership: { kind: "task", taskId: runtime.taskId },
                    });
                    return { status: "running", checkpoint: { phase: "run", child } };
                }, context);
                await untilAborted(runtime.signal);
                log.push("parent-signalled");
            },
        },
        abort: async (task: any, runtime: any, context: any) => {
            const outcomes = await runtime.outcomes([task.state.checkpoint.child], context);
            log.push(`parent-abort:${outcomes[0].status}`);
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "parent" } }),
                context,
            );
        },
    });
    const { harness } = await openHarness(new MemoryStorage(), [Parent, Child]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Parent, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(() => log.includes("child-run"), "child running");
    const marked = await harness.abortTask(id, BACKGROUND_CONTEXT);
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    const records = await harness.commit((tx: any) => tx.scanTasks({}, 10), BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "aborting a parent runs descendants' abort handlers before the parent and preserves order",
        source: "packages/durable/test/harness-ownership.test.ts:abort cascades bottom-up",
        expected: {
            marked,
            log,
            parent: receipt.state.outcome,
            children: records.items
                .filter((task: any) => task.kind === "audit.cascade-child")
                .map((task: any) => ({ outcome: task.state.outcome.status, abortRequested: task.abortRequested })),
        },
    };
}

export async function abortOrderingWithHooks(): Promise<Scenario> {
    const log: string[] = [];
    const Abortable = defineTask<null, { phase: "run" }, string, { onStep(): void }>({
        name: "audit.abort-order",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                log.push("run-start");
                await runtime.hooks.each("onStep", (handler: any) => handler());
                try {
                    await untilAborted(runtime.signal);
                } catch {
                    log.push("run-signalled");
                }
                try {
                    await runtime.commit(
                        () => ({ status: "terminal", outcome: { status: "completed", result: "unexpected" } }),
                        context,
                    );
                } catch (error) {
                    log.push(`commit-rejected:${(error as Error).message}`);
                }
                log.push("run-end");
            },
        },
        abort: async (_task: any, runtime: any, context: any) => {
            log.push("abort-handler");
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "requested" } }),
                context,
            );
        },
        hooks: { onStep() {} },
    });
    const { harness, registry } = await openHarness(new MemoryStorage(), [Abortable]);
    registry.install(
        defineExtension({ name: "order-1", hooks: [hook(Abortable, { onStep: () => log.push("hook-1") })] }),
    );
    registry.install(
        defineExtension({ name: "order-2", hooks: [hook(Abortable, { onStep: () => log.push("hook-2") })] }),
    );
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Abortable, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(() => log.includes("hook-2"), "run reached its abort wait");
    const marked = await harness.abortTask(id, BACKGROUND_CONTEXT);
    const during = await harness.getTask(id, BACKGROUND_CONTEXT);
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "abortTask marks a running task and runs its abort handler after the run invocation joins",
        source: "packages/durable/src/harness/scheduler.ts:#runAbort",
        expected: {
            marked,
            duringMark: { status: during.state.status, abortRequested: during.abortRequested },
            log,
            outcome: receipt.state.outcome,
        },
    };
}

export async function waitCancellationIsNotAbort(): Promise<Scenario> {
    let release!: () => void;
    const gate = new Promise<void>((resolve) => {
        release = resolve;
    });
    const log: string[] = [];
    const Slow = defineTask({
        name: "audit.slow",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                log.push("run");
                await gate;
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "done" } }),
                    context,
                );
            },
        },
        abort: async (_task: any, runtime: any, context: any) => {
            log.push("abort");
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "requested" } }),
                context,
            );
        },
    });
    const { harness } = await openHarness(new MemoryStorage(), [Slow]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Slow, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(() => log.includes("run"), "slow task running");
    const controller = new AbortController();
    const waiting = harness.waitForTask(id, { ...BACKGROUND_CONTEXT, abortSignal: controller.signal });
    controller.abort(new Error("stop waiting"));
    let waitError: Record<string, unknown>;
    try {
        await waiting;
        waitError = { name: "None", message: "wait unexpectedly resolved" };
    } catch (error) {
        waitError = errorRecord(error);
    }
    const afterCancel = await harness.getTask(id, BACKGROUND_CONTEXT);
    release();
    const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "cancelling a wait cancels only the waiter; the persistent task keeps running and completes",
        source: "packages/durable/test/harness-ownership.test.ts:cancels only the caller's wait",
        expected: {
            waitError,
            afterCancel: { status: afterCancel.state.status, abortRequested: afterCancel.abortRequested },
            outcome: receipt.state.outcome,
            log,
        },
    };
}

// ---------------------------------------------------------------------------------------------------
// Submissions and inbox: admission, dedup, queue boundaries, queue modes and withdrawal.
// ---------------------------------------------------------------------------------------------------

export async function idleInputAnswered(): Promise<Scenario> {
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([answer("hello")]);
    const submission = await root.submit({ type: "input", content: "hi" }, BACKGROUND_CONTEXT);
    const settled = await submission.wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(root);
    const context = await root.context(BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "an idle input is placed and answered by a run",
        source: "packages/durable/src/harness/submissions.ts:admitSubmission",
        expected: {
            status: settled.status,
            type: settled.type,
            entryKind: entries[0]?.kind,
            transcript: transcript(entries),
            messages: context.messages.map((message: any) => message.role),
        },
    };
}

export async function busyFollowUpQueued(): Promise<Scenario> {
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const held = heldStep(() => reached());
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([held.step, answer("second")]);
    const first = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const followUp = await root.submit({ type: "input", content: "f1" }, BACKGROUND_CONTEXT);
    const before = {
        first: (await first.status(BACKGROUND_CONTEXT)).status,
        followUp: (await followUp.status(BACKGROUND_CONTEXT)).status,
        inbox: (await harness.snapshot(InboxDoc, root.id, BACKGROUND_CONTEXT)).items.map((item: any) => ({
            mode: item.mode,
            isFollowUp: item.id === followUp.id,
        })),
    };
    held.release();
    const settled = await followUp.wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(root);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a busy input queues as a follow-up and is placed at the final boundary",
        source: "packages/durable/src/harness/inbox.ts:applyBoundary",
        expected: {
            before,
            followUpStatus: settled.status,
            transcript: transcript(entries),
        },
    };
}

export async function idleWriteSettles(): Promise<Scenario> {
    const { harness, root } = await openChat(new MemoryStorage());
    const write = await root.submit(
        {
            type: "write",
            entry: { kind: "audit.note", data: { text: "passive" } },
        },
        BACKGROUND_CONTEXT,
    );
    const settled = await write.wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(root);
    const context = await root.context(BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "an idle passive write is appended and settled done without starting a run",
        source: "packages/durable/src/harness/submissions.ts:admitSubmission",
        expected: {
            status: settled.status,
            entryKind: entries[0]?.kind,
            entryData: entries[0]?.data,
            messages: context.messages.length,
        },
    };
}

export async function busySteerQueued(): Promise<Scenario> {
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const held = heldStep(() => reached());
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([held.step]);
    const first = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const steer = await root.submit({ type: "input", content: "s", whenBusy: "steer" }, BACKGROUND_CONTEXT);
    const before = {
        first: (await first.status(BACKGROUND_CONTEXT)).status,
        steer: (await steer.status(BACKGROUND_CONTEXT)).status,
        inbox: (await harness.snapshot(InboxDoc, root.id, BACKGROUND_CONTEXT)).items.map((item: any) => ({
            mode: item.mode,
            isSteer: item.id === steer.id,
        })),
    };
    await root.abort(BACKGROUND_CONTEXT);
    const after = await steer.status(BACKGROUND_CONTEXT);
    const live = await harness.snapshot(LiveDoc, root.id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a busy steer input queues in steer mode and a conversation abort withdraws it",
        source: "packages/durable/src/harness/inbox.ts:withdrawQueuedInputs,applyBoundary",
        expected: {
            before,
            after: { status: after.status, reason: after.reason ?? null },
            live,
        },
    };
}

export async function displayOnlyWriteWhileBusy(): Promise<Scenario> {
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const held = heldStep(() => reached());
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([held.step]);
    await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const write = await root.submit(
        { type: "write", entry: { kind: "audit.display", data: { label: "visible" } } },
        BACKGROUND_CONTEXT,
    );
    const settled = await write.status(BACKGROUND_CONTEXT);
    const inbox = (await harness.snapshot(InboxDoc, root.id, BACKGROUND_CONTEXT)).items;
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a display-only write is appended and settled while the conversation is busy",
        source: "packages/durable/src/harness/submissions.ts:isDisplayOnly",
        expected: { status: settled.status, inboxLength: inbox.length },
    };
}

export async function requestIdDeduplication(): Promise<Scenario> {
    const storage = new RecordingStorage();
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const held = heldStep(() => reached());
    const { harness, root, faux } = await openChat(storage);
    faux.setResponses([held.step]);
    const first = await root.submit({ type: "input", content: "a", requestId: "req-9" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const batchesBefore = storage.batches.length;
    const duplicate = await root.submit(
        { type: "input", content: "different", requestId: "req-9" },
        BACKGROUND_CONTEXT,
    );
    const duplicateBatches = storage.batches.length - batchesBefore;
    let wrongType: Record<string, unknown>;
    try {
        await root.submit({ type: "write", entry: { kind: "note" }, requestId: "req-9" }, BACKGROUND_CONTEXT);
        wrongType = { name: "None", message: "accepted" };
    } catch (error) {
        wrongType = errorRecord(error);
    }
    const originalStatus = (await first.status(BACKGROUND_CONTEXT)).status;
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a duplicate request ID returns the existing submission without a storage write",
        source: "packages/durable/src/harness/submissions.ts:admitSubmission",
        expected: { sameId: duplicate.id === first.id, duplicateBatches, originalStatus, wrongType },
    };
}

export async function busyReject(): Promise<Scenario> {
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const held = heldStep(() => reached());
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([held.step]);
    await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const before = await harness.inspect(BACKGROUND_CONTEXT);
    let error: Record<string, unknown>;
    try {
        await root.submit(
            { type: "input", content: "rejected", whenBusy: "reject" },
            BACKGROUND_CONTEXT,
        );
        error = { name: "None", message: "accepted" };
    } catch (caught) {
        error = errorRecord(caught);
        error.isConversationBusy = caught instanceof ConversationBusy;
    }
    const after = await harness.inspect(BACKGROUND_CONTEXT);
    const inbox = (await harness.snapshot(InboxDoc, root.id, BACKGROUND_CONTEXT)).items;
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a busy input with whenBusy reject throws ConversationBusy and writes no submission",
        source: "packages/durable/src/harness/submissions.ts:admitSubmission",
        expected: {
            error,
            submissionsBefore: before.submissions.length,
            submissionsAfter: after.submissions.length,
            inboxLength: inbox.length,
        },
    };
}

export async function atomicAdmissionBatch(): Promise<Scenario> {
    const storage = new RecordingStorage();
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const held = heldStep(() => reached());
    const { harness, root, faux } = await openChat(storage);
    faux.setResponses([held.step]);
    await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const before = storage.batches.length;
    await root.submit({ type: "input", content: "queued" }, BACKGROUND_CONTEXT);
    const batch = storage.batches[before]!;
    const submission = batch.find((write) => write.type === "submission");
    const document = batch.find((write) => write.type === "document.change");
    const appended = document?.content?.ops?.find((op: any) => op[1]?.[0] === "items");
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "admission commits the queued submission and its inbox change in one storage batch",
        source: "packages/durable/src/session/session.ts:HarnessImpl.#runCommit",
        expected: {
            batchTypes: batch.map((write) => write.type),
            submissionStatus: submission?.value?.status,
            submissionType: submission?.value?.type,
            documentKind: document?.content?.kind,
            documentVersion: document?.content?.version,
            inboxPath: Array.isArray(appended?.[1]) ? appended[1][0] : null,
        },
    };
}

export async function queueModes(): Promise<Scenario> {
    const runMode = async (followUpMode: "all" | "one-at-a-time") => {
        let reached!: () => void;
        const reachedPromise = new Promise<void>((resolve) => {
            reached = resolve;
        });
        const held = heldStep(() => reached());
        const { harness, root, faux } = await openChat(new MemoryStorage(), { settings: { followUpMode } });
        faux.setResponses([held.step, answer("a"), answer("b")]);
        const first = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
        await reachedPromise;
        const one = await root.submit({ type: "input", content: "f1" }, BACKGROUND_CONTEXT);
        const two = await root.submit({ type: "input", content: "f2" }, BACKGROUND_CONTEXT);
        held.release();
        await first.wait(BACKGROUND_CONTEXT);
        await one.wait(BACKGROUND_CONTEXT);
        const twoAfterFirst = (await two.status(BACKGROUND_CONTEXT)).status;
        await two.wait(BACKGROUND_CONTEXT);
        const entries = await entriesOf(root);
        await harness.close(BACKGROUND_CONTEXT);
        return { twoAfterFirst, transcript: transcript(entries) };
    };
    return {
        name: "queue modes decide whether all follow-ups or one are placed at the final boundary",
        source: "packages/durable/src/harness/inbox.ts:applyBoundary",
        expected: {
            all: await runMode("all"),
            oneAtATime: await runMode("one-at-a-time"),
        },
    };
}

export async function abortResults(): Promise<Scenario> {
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const held = heldStep(() => reached());
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([held.step]);
    const placed = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const queued = await root.submit({ type: "input", content: "b" }, BACKGROUND_CONTEXT);
    const other = await harness.createConversation({ ownership: { kind: "ownerless" } }, BACKGROUND_CONTEXT);
    const results: Record<string, unknown> = {
        placed: await placed.abort(BACKGROUND_CONTEXT),
        wrongConversation: await harness.abortSubmission(queued.id, BACKGROUND_CONTEXT, other.id),
        queued: await harness.abortSubmission(queued.id, BACKGROUND_CONTEXT),
        again: await harness.abortSubmission(queued.id, BACKGROUND_CONTEXT),
        unknown: await harness.abortSubmission(999_999, BACKGROUND_CONTEXT),
    };
    const queuedHandle = await harness.submission(queued.id, BACKGROUND_CONTEXT);
    const queuedStatus = await queuedHandle.status(BACKGROUND_CONTEXT);
    const inbox = (await harness.snapshot(InboxDoc, root.id, BACKGROUND_CONTEXT)).items;
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "abort reports queued, placed and settled outcomes and removes the queued item",
        source: "packages/durable/src/harness/submissions.ts:Submissions.abort",
        expected: {
            results,
            queuedRecord: { status: queuedStatus.status, reason: queuedStatus.reason ?? null },
            inboxLength: inbox.length,
        },
    };
}

export async function conversationAbortEffects(): Promise<Scenario> {
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const held = heldStep(() => reached());
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([held.step]);
    const placed = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const queued = await root.submit({ type: "input", content: "b" }, BACKGROUND_CONTEXT);
    const write = await root.submit(
        { type: "write", entry: { kind: "note", model: [{ role: "user", content: "n", timestamp: 0 }] } },
        BACKGROUND_CONTEXT,
    );
    const before = await Promise.all([
        placed.status(BACKGROUND_CONTEXT),
        queued.status(BACKGROUND_CONTEXT),
        write.status(BACKGROUND_CONTEXT),
    ]);
    await root.abort(BACKGROUND_CONTEXT);
    const handles = await Promise.all([
        harness.submission(placed.id, BACKGROUND_CONTEXT),
        harness.submission(queued.id, BACKGROUND_CONTEXT),
        harness.submission(write.id, BACKGROUND_CONTEXT),
    ]);
    const after = await Promise.all(handles.map((handle) => handle.status(BACKGROUND_CONTEXT)));
    const inbox = (await harness.snapshot(InboxDoc, root.id, BACKGROUND_CONTEXT)).items;
    const live = await harness.snapshot(LiveDoc, root.id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "conversation abort withdraws queued inputs, keeps queued writes and settles the placed input unanswered",
        source: "packages/durable/src/harness/inbox.ts:withdrawQueuedInputs",
        expected: {
            before: before.map((record: any) => record.status),
            after: after.map((record: any) =>
                record === undefined ? null : { status: record.status, reason: record.reason ?? null },
            ),
            inbox: inbox.map((item: any) => ({ mode: item.mode, isWrite: item.id === write.id })),
            live,
        },
    };
}

export async function submissionWaitCancellation(): Promise<Scenario> {
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const held = heldStep(() => reached());
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([held.step]);
    const submission = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const controller = new AbortController();
    const waiting = submission.wait({ ...BACKGROUND_CONTEXT, abortSignal: controller.signal });
    controller.abort(new Error("stop waiting"));
    let error: Record<string, unknown>;
    try {
        await waiting;
        error = { name: "None", message: "wait unexpectedly resolved" };
    } catch (caught) {
        error = errorRecord(caught);
    }
    const afterCancel = await submission.status(BACKGROUND_CONTEXT);
    held.release();
    const settled = await submission.wait(BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "cancelling a submission wait leaves the submission placed until it settles",
        source: "packages/durable/test/harness-submissions.test.ts:cancels only a wait",
        expected: {
            error,
            afterCancel: afterCancel.status,
            settled: settled.status,
        },
    };
}

// ---------------------------------------------------------------------------------------------------
// Process crash and reopen: killed worker, SQLite continuation, request-ID dedup, abort marks and restart.
// ---------------------------------------------------------------------------------------------------

async function crashWorker(mode: string): Promise<{ ready: any; signal: string | null; database: string; directory: string }> {
    const directory = mkdtempSync(path.join(tmpdir(), `pi-1055-${mode}-`));
    const database = path.join(directory, "session.sqlite");
    const checkout = process.env.PI_CHECKOUT;
    assert(checkout !== undefined && checkout !== "", "PI_CHECKOUT is required");
    // `node --import tsx` keeps the worker as the direct child: a SIGKILL then kills the measured
    // process itself. Spawning the `.bin/tsx` wrapper instead leaves a grandchild alive.
    const child = spawn(process.execPath, ["--import", "tsx", WORKER, mode, database], {
        cwd: checkout,
        env: { ...process.env, AUDIT_1055_CHILD: "1" },
        stdio: ["ignore", "pipe", "inherit"],
    });
    const ready = await new Promise<any>((resolve, reject) => {
        let output = "";
        const timer = setTimeout(() => reject(new Error(`worker ${mode} did not reach its boundary`)), 30_000);
        child.stdout.on("data", (chunk) => {
            output += chunk;
            if (!output.includes("READY\n")) return;
            clearTimeout(timer);
            try {
                resolve(JSON.parse(readFileSync(`${database}.ready`, "utf8")));
            } catch (error) {
                reject(error);
            }
        });
        child.on("exit", (code) => {
            clearTimeout(timer);
            reject(new Error(`worker ${mode} exited before its boundary: ${String(code)}`));
        });
    });
    child.kill("SIGKILL");
    const signal = await new Promise<string | null>((resolve) => child.on("exit", (_code, value) => resolve(value)));
    assert(signal === "SIGKILL", `worker ${mode} was not killed at its boundary (signal ${String(signal)})`);
    return { ready, signal, database, directory };
}

export async function crashedGenerationSubmission(): Promise<Scenario> {
    const worker = await crashWorker("generation");
    try {
        const reopened = await openChat(await openSqlite(worker.database));
        reopened.faux.setResponses([answer("after reopen")]);
        const harness = reopened.harness;
        const root = reopened.root;
        const submitted = await harness.submission(worker.ready.submissionId, BACKGROUND_CONTEXT);
        const afterOpen = await harness.inspect(BACKGROUND_CONTEXT);
        const submissionAfterOpen = await submitted.status(BACKGROUND_CONTEXT);
        harness.resume();
        const settled = await submitted.wait(BACKGROUND_CONTEXT);
        const entriesAfterSettle = await entriesOf(root);
        const duplicate = await root.submit(
            { type: "input", content: "different", requestId: "crash-1" },
            BACKGROUND_CONTEXT,
        );
        await harness.close(BACKGROUND_CONTEXT);

        const third = await openChat(await openSqlite(worker.database));
        const reacquired = await third.harness.submission(worker.ready.submissionId, BACKGROUND_CONTEXT);
        const settledAfterThird = await reacquired.wait(BACKGROUND_CONTEXT);
        const entriesAfterThird = await entriesOf(third.root);
        await third.harness.close(BACKGROUND_CONTEXT);
        return {
            name: "a killed process leaves an admitted input and its generation task recoverable without a duplicate submission",
            source: "packages/durable/test/harness-submissions.test.ts:reacquires a submission after reopen",
            expected: {
                killSignal: worker.signal,
                schedulingAfterOpen: afterOpen.scheduling,
                taskAfterOpen: afterOpen.tasks.map((task: any) => ({
                    kind: task.record.kind,
                    recordStatus: task.record.state.status,
                    inspection: task.state.kind,
                    checkpointPhase: task.record.state.checkpoint.phase,
                    abortRequested: task.record.abortRequested,
                })),
                submissionAfterOpen: { status: submissionAfterOpen.status, requestId: submissionAfterOpen.requestId },
                settled: { status: settled.status, hasAnswer: settled.type === "input" && settled.answer !== undefined },
                duplicateSameId: duplicate.id === worker.ready.submissionId,
                transcriptAfterSettle: transcript(entriesAfterSettle),
                settledAfterThird: settledAfterThird.status,
                answerCountAfterThird: entriesAfterThird.filter((entry) => entry.kind === "pi.assistant").length,
            },
        };
    } finally {
        rmSync(worker.directory, { recursive: true, force: true });
    }
}

export async function crashedTransferTask(): Promise<Scenario> {
    const worker = await crashWorker("effect");
    try {
        const effectPath = `${worker.database}.effect.json`;
        const effect = (key: string, amount: number) => {
            const state = JSON.parse(readFileSync(effectPath, "utf8"));
            state.calls++;
            if (!(key in state.applied)) state.applied[key] = amount * 10;
            writeFileSync(effectPath, JSON.stringify(state));
            return state.applied[key];
        };
        const Transfer = transferTask(async (key, amount) => effect(key, amount));
        const reopened = await openHarness(await openSqlite(worker.database), [Transfer]);
        const task = await taskOfKind(reopened.harness, "audit.transfer");
        const afterOpen = await reopened.harness.getTask(task.id, BACKGROUND_CONTEXT);
        const effectAfterCrash = JSON.parse(readFileSync(effectPath, "utf8"));
        reopened.harness.resume();
        const receipt = await reopened.harness.waitForTask(task.id, BACKGROUND_CONTEXT);
        const effectAfterResume = JSON.parse(readFileSync(effectPath, "utf8"));
        await reopened.harness.close(BACKGROUND_CONTEXT);

        const third = await openHarness(await openSqlite(worker.database), [Transfer]);
        const terminal = await third.harness.getTask(task.id, BACKGROUND_CONTEXT);
        third.harness.resume();
        await sleep(50);
        const effectAfterThird = JSON.parse(readFileSync(effectPath, "utf8"));
        await third.harness.close(BACKGROUND_CONTEXT);
        return {
            name: "a killed process resumes an intent/effect/outcome task from its checkpoint and applies the effect once",
            source: "packages/durable/test/harness-tasks-recovery.test.ts:intent/effect/outcome",
            expected: {
                killSignal: worker.signal,
                ready: { key: worker.ready.key, receipt: worker.ready.receipt },
                afterOpen: {
                    status: afterOpen.state.status,
                    checkpoint: afterOpen.state.checkpoint,
                    memos: afterOpen.memos,
                    startedAtPresent: typeof afterOpen.startedAt === "number",
                },
                effectAfterCrash: { calls: effectAfterCrash.calls, applied: Object.keys(effectAfterCrash.applied).length },
                outcome: receipt.state.outcome,
                effectAfterResume: { calls: effectAfterResume.calls, applied: Object.keys(effectAfterResume.applied).length },
                terminalAfterThird: terminal.state.status,
                effectAfterThird: { calls: effectAfterThird.calls, applied: Object.keys(effectAfterThird.applied).length },
            },
        };
    } finally {
        rmSync(worker.directory, { recursive: true, force: true });
    }
}

export async function crashedAbortMark(): Promise<Scenario> {
    const worker = await crashWorker("abort");
    try {
        const log: string[] = [];
        const Abortable = abortCrashTask(log, async (runtime: any, context: any) => {
            log.push("abort");
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "recovered" } }),
                context,
            );
        });
        const reopened = await openHarness(await openSqlite(worker.database), [Abortable]);
        const task = await taskOfKind(reopened.harness, "audit.abort-crash");
        const afterOpen = await reopened.harness.getTask(task.id, BACKGROUND_CONTEXT);
        reopened.harness.resume();
        const receipt = await reopened.harness.waitForTask(task.id, BACKGROUND_CONTEXT);
        await reopened.harness.close(BACKGROUND_CONTEXT);
        return {
            name: "an abort mark survives a killed process and only the abort handler runs after reopen",
            source: "packages/durable/test/harness-tasks-recovery.test.ts:abort work after close",
            expected: {
                killSignal: worker.signal,
                workerLog: worker.ready.log,
                afterOpen: {
                    status: afterOpen.state.status,
                    abortRequested: afterOpen.abortRequested,
                    checkpoint: afterOpen.state.checkpoint,
                },
                reopenedLog: log,
                outcome: receipt.state.outcome,
            },
        };
    } finally {
        rmSync(worker.directory, { recursive: true, force: true });
    }
}

export async function crashedTerminalTask(): Promise<Scenario> {
    const worker = await crashWorker("terminal");
    try {
        const log: string[] = [];
        const Once = oneShotTask("audit.terminal", "once", log);
        const reopened = await openHarness(await openSqlite(worker.database), [Once]);
        const task = await taskOfKind(reopened.harness, "audit.terminal");
        const submission = await reopened.harness.submission(worker.ready.submissionId, BACKGROUND_CONTEXT);
        const submissionStatus = await submission.status(BACKGROUND_CONTEXT);
        reopened.harness.resume();
        const reopenedRoot = await reopened.harness.root(BACKGROUND_CONTEXT);
        const duplicate = await reopenedRoot.submit(
            { type: "write", entry: { kind: "audit.terminal.other", data: { done: false } }, requestId: "terminal-1" },
            BACKGROUND_CONTEXT,
        );
        await sleep(50);
        await reopened.harness.close(BACKGROUND_CONTEXT);
        return {
            name: "a killed process after a terminal commit does not rerun the task or duplicate the submission",
            source: "packages/durable/test/harness-tasks-recovery.test.ts:terminal receipt survives reopen",
            expected: {
                killSignal: worker.signal,
                workerLog: worker.ready.log,
                taskStatus: task.state.status,
                outcome: task.state.outcome,
                submissionStatus: submissionStatus.status,
                rerunLog: log,
                duplicateSameId: duplicate.id === worker.ready.submissionId,
            },
        };
    } finally {
        rmSync(worker.directory, { recursive: true, force: true });
    }
}

export async function abandonedOnRestart(): Promise<Scenario> {
    const log: string[] = [];
    const Abandonable = defineTask({
        name: "audit.abandon",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                log.push("run");
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "unexpected" } }),
                    context,
                );
            },
        },
        abort: async (task: any, runtime: any, context: any) => {
            log.push(`abort:${task.abortReason ?? "none"}`);
            await runtime.commit(
                () => ({
                    status: "terminal",
                    outcome: { status: "aborted", reason: task.abortReason ?? "requested" },
                }),
                context,
            );
        },
    });
    const directory = mkdtempSync(path.join(tmpdir(), "pi-1055-abandon-"));
    const database = path.join(directory, "session.sqlite");
    try {
        const first = await openHarness(await openSqlite(database), [Abandonable]);
        const root = await first.harness.root(BACKGROUND_CONTEXT);
        const id = await root.commit(
            (tx: any) =>
                tx.createTask(Abandonable, null, {
                    ownership: { kind: "conversation" },
                    abandonOnRestart: true,
                }),
            BACKGROUND_CONTEXT,
        );
        await first.harness.close(BACKGROUND_CONTEXT);

        const reopened = await openHarness(await openSqlite(database), [Abandonable]);
        const afterOpen = await reopened.harness.getTask(id, BACKGROUND_CONTEXT);
        reopened.harness.resume();
        const receipt = await reopened.harness.waitForTask(id, BACKGROUND_CONTEXT);
        await reopened.harness.close(BACKGROUND_CONTEXT);
        return {
            name: "a task abandoned on restart is aborted with reason restart after reopen",
            source: "packages/durable/src/harness/scheduler.ts:#markAbandonedOnRestart",
            expected: {
                afterOpen: { status: afterOpen.state.status, checkpoint: afterOpen.state.checkpoint },
                abortReason: receipt.abortReason,
                outcome: receipt.state.outcome,
                log,
            },
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}
