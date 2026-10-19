/**
 * Task definitions shared by the parent capture and the killed worker process.
 *
 * A killed process and its reopening process must agree on `name`, `version`, input, checkpoint and
 * result shapes; sharing the factory keeps that agreement mechanical. Each process supplies its own
 * phase bodies where the observation needs process-local state.
 */

import {
    BACKGROUND_CONTEXT,
    createModels,
    createRegistry,
    defineExtension,
    defineTask,
    fauxProvider,
    Harness,
    openNodeSqliteStorage,
    type Scenario,
} from "./pi.mts";

/** Reject with the invocation's abort reason once its signal fires. */
export function untilAborted(signal: AbortSignal): Promise<never> {
    return new Promise((_, reject) => {
        if (signal.aborted) reject(signal.reason);
        signal.addEventListener("abort", () => reject(signal.reason), { once: true });
    });
}

/** A one-phase counter task whose checkpoint advances one commit at a time. */
export function counterTask(seen: number[], runtimes: Set<unknown>) {
    return defineTask({
        name: "audit.counter",
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
}

type TransferInput = { readonly amount: number };
type TransferState = { phase: "prepare" } | { phase: "apply"; key: string };

/**
 * Intent/effect/outcome task. `effect` is idempotent by `key`: after a kill it may be called again,
 * and the second call must observe the first call's persisted application rather than add one.
 */
export function transferTask(
    effect: (key: string, amount: number) => Promise<number>,
    afterEffect?: (task: any) => Promise<void>,
) {
    return defineTask<TransferInput, TransferState, { receipt: number }>({
        name: "audit.transfer",
        version: 1,
        initial: () => ({ phase: "prepare" }),
        phases: {
            prepare: async (task, runtime, context) => {
                await runtime.memo("requested", task.input.amount, context);
                await runtime.commit(
                    () => ({ status: "running", checkpoint: { phase: "apply", key: `transfer-${task.id}` } }),
                    context,
                );
            },
            apply: async (task, runtime, context) => {
                const receipt = await effect(task.state.checkpoint.key, task.input.amount);
                if (afterEffect !== undefined) await afterEffect(task);
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: { receipt } } }),
                    context,
                );
            },
        },
        abort: async (_task, runtime, context) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted", reason: "requested" } }), context),
    });
}

/** A task that runs once and settles completed with `result`. */
export function oneShotTask(name: string, result: string, log: string[] = []) {
    return defineTask<null, { phase: "run" }, string>({
        name,
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task, runtime, context) => {
                log.push("run");
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result } }),
                    context,
                );
            },
        },
        abort: async (_task, runtime, context) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted", reason: "requested" } }), context),
    });
}

/**
 * A task whose run ends on its abort signal and whose abort handler is supplied by the caller. The
 * killed worker parks the handler before its terminal commit; the reopening process commits it.
 */
export function abortCrashTask(log: string[], abortHandler: (runtime: any, context: any) => Promise<void>) {
    return defineTask<null, { phase: "run" }, string>({
        name: "audit.abort-crash",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task, runtime, _context) => {
                log.push("run");
                await untilAborted(runtime.signal);
                log.push("run-signalled");
            },
        },
        abort: async (_task: any, runtime: any, context: any) => abortHandler(runtime, context),
    });
}

/** A versioned one-phase task; `migrate` may adapt input and checkpoint from an older version. */
export function versionedTask(version: number, result: string, migrate?: any, log: string[] = []) {
    return defineTask<{ n: number }, { phase: "run"; n: number }, string>({
        name: "audit.versioned",
        version,
        initial: (input: any) => ({ phase: "run", n: input.n }),
        phases: {
            run: async (task: any, runtime: any, context: any) => {
                log.push(`run-v${task.version}`);
                await runtime.commit(
                    () => ({
                        status: "terminal",
                        outcome: { status: "completed", result: `${result}:${task.state.checkpoint.n}` },
                    }),
                    context,
                );
            },
        },
        abort: async (_task: any, runtime: any, context: any) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted" } }), context),
        ...(migrate === undefined ? {} : { migrate }),
    });
}

type HandoverState = { phase: "p1" } | { phase: "p2" };

/** Version 1 of a task that parks in `p1` after committing the `p2` checkpoint. */
export function handoverV1(gate: Promise<void>, log: string[]) {
    return defineTask<null, HandoverState, string>({
        name: "audit.handover",
        version: 1,
        initial: () => ({ phase: "p1" }),
        phases: {
            p1: async (_task, runtime, context) => {
                log.push("v1-p1");
                await runtime.commit(() => ({ status: "running", checkpoint: { phase: "p2" } }), context);
                await gate;
                log.push("v1-p1-release");
            },
            p2: async (_task, runtime, context) => {
                log.push("v1-p2-unexpected");
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "v1" } }),
                    context,
                );
            },
        },
        abort: async (_task, runtime, context) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted" } }), context),
    });
}

/** Version 2 of the handover task; `p1` must never run after migration. */
export function handoverV2(log: string[]) {
    return defineTask<null, HandoverState, string>({
        name: "audit.handover",
        version: 2,
        initial: () => ({ phase: "p1" }),
        phases: {
            p1: async () => {
                log.push("v2-p1-unexpected");
                throw new Error("version 2 phase p1 must not run");
            },
            p2: async (_task, runtime, context) => {
                log.push("v2-p2");
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "v2" } }),
                    context,
                );
            },
        },
        abort: async (_task, runtime, context) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted" } }), context),
        migrate: (input: any, checkpoint: any, fromVersion: number) => {
            log.push(`v2-migrate-from-${fromVersion}`);
            return { input, checkpoint };
        },
    });
}

export function holdParent(log: string[]) {
    return defineTask<null, { phase: "run" }, string>({
        name: "audit.hold-parent",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task, runtime, context) => {
                await runtime.commit(async (tx: any) => {
                    await tx.createTask(holdChild(log), null, { ownership: { kind: "task", taskId: runtime.taskId } });
                    return undefined;
                }, context);
                log.push("parent-commit-completed");
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "parent" } }),
                    context,
                );
            },
        },
        abort: async (_task, runtime, context) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted" } }), context),
    });
}

export function holdChild(log: string[], gate?: Promise<void>) {
    return defineTask<null, { phase: "run" }, string>({
        name: "audit.hold-child",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task, runtime, context) => {
                log.push("child-run");
                if (gate !== undefined) await gate;
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "child" } }),
                    context,
                );
                log.push("child-done");
            },
        },
        abort: async (_task, runtime, context) =>
            runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted" } }), context),
    });
}

/** Open a Harness over one registry and its own faux model table. */
export async function openHarness(
    storage: any,
    tasks: readonly any[],
    options: { readonly now?: () => number; readonly onReport?: (error: unknown) => void; readonly settings?: any } = {},
): Promise<{ harness: any; registry: any; models: any }> {
    const registry = createRegistry();
    if (tasks.length > 0) registry.install(defineExtension({ name: "audit", tasks }));
    const models = createModels();
    const harness = await Harness.open(
        storage,
        {
            models,
            registry,
            ...(options.now === undefined ? {} : { now: options.now }),
            ...(options.onReport === undefined ? {} : { onReport: options.onReport }),
            ...(options.settings === undefined ? {} : { settings: options.settings }),
        },
        BACKGROUND_CONTEXT,
    );
    return { harness, registry, models };
}

/** Open a harness whose root conversation uses a faux provider table. */
export async function openChat(storage: any, options: { readonly settings?: any } = {}): Promise<any> {
    const faux = fauxProvider();
    const registry = createRegistry();
    const models = createModels();
    models.setProvider(faux.provider);
    const harness = await Harness.open(
        storage,
        { models, registry, ...(options.settings === undefined ? {} : { settings: options.settings }) },
        BACKGROUND_CONTEXT,
    );
    const root = await harness.root(BACKGROUND_CONTEXT, {
        agent: { model: { provider: "faux", modelId: "faux-1" } },
    });
    return { harness, root, faux, registry, models };
}

/** Open SQLite storage through the frozen Node adapter. */
export function openSqlite(path: string): Promise<any> {
    return openNodeSqliteStorage(path);
}

/** The one task of `kind`, or undefined; used after a reopen where the ID is not known in memory. */
export async function taskOfKind(harness: any, kind: string): Promise<any> {
    const page = await harness.commit((tx: any) => tx.scanTasks({}, 100), BACKGROUND_CONTEXT);
    return page.items.find((task: any) => task.kind === kind);
}

export type { Scenario };
