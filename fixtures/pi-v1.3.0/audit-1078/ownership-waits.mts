/**
 * Frozen pi v1.3.0 observations for `audit-1078`: the ownership and wait
 * boundaries the `audit-1055` `ownership-abort` artifact did not capture.
 *
 * `audit-1055` already records the failFast/allSettled joins, the bottom-up
 * abort cascade, the abort-handler ordering and the wait-cancel-versus-abort
 * distinction. This family adds the three observations the #1058 frontier
 * records as missing for the F5 slice:
 *
 * - the background ownership boundary: a conversation abort stops at a
 *   background task, while a direct abort of that task and a
 *   `background: true` conversation abort reach it;
 * - the ordinary idle wait: it resolves once the ordinary ownership scope is
 *   idle and ignores live background work;
 * - the ownership restrictions a transaction enforces on a child task and on
 *   a task-owned conversation.
 *
 * Every scenario drives public `Harness`/`Conversation`/`TaskRuntime` calls.
 * The helpers are adapted from `audit-1055`; nothing is imported across family
 * directories.
 */

import {
    BACKGROUND_CONTEXT,
    Harness,
    MemoryStorage,
    createRegistry,
    defineExtension,
    defineTask,
    type Scenario,
} from "./pi.mts";

/** Resolve when `predicate` holds, polling at 5 ms, or reject after 5 s. */
async function waitUntil(predicate: () => boolean, what: string): Promise<void> {
    const deadline = Date.now() + 5000;
    while (!predicate()) {
        if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
        await new Promise((resolve) => setTimeout(resolve, 5));
    }
}

function deferred(): { promise: Promise<void>; resolve: () => void } {
    let resolve!: () => void;
    return { promise: new Promise<void>((done) => { resolve = done; }), resolve: () => resolve() };
}

/** A promise that rejects when `signal` aborts, so a phase parks until it is signalled. */
function untilAborted(signal: AbortSignal): Promise<never> {
    return new Promise((_, reject) => {
        if (signal.aborted) reject(signal.reason);
        signal.addEventListener("abort", () => reject(signal.reason), { once: true });
    });
}

/**
 * One task definition whose run phase waits for `gate`, then completes with
 * `result`. The wait is raced against the invocation signal, so an abort mark
 * ends the run invocation and the scheduler can start the abort handler.
 */
function gatedTask(name: string, log: string[], gate: Promise<void>, result: string) {
    return defineTask({
        name,
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                log.push(`${name}:run`);
                const signalled = untilAborted(runtime.signal).then(
                    () => "signalled" as const,
                    () => "signalled" as const,
                );
                if ((await Promise.race([gate.then(() => "released" as const), signalled])) === "signalled") {
                    log.push(`${name}:signalled`);
                    return;
                }
                log.push(`${name}:released`);
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result } }),
                    context,
                );
            },
        },
        abort: async (_task: any, runtime: any, context: any) => {
            log.push(`${name}:abort`);
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason: "cascade" } }),
                context,
            );
        },
    });
}

/** Open a Harness over `storage` with one extension holding the given task definitions. */
async function openHarness(storage: any, tasks: readonly any[]): Promise<any> {
    const registry = createRegistry();
    registry.install(defineExtension({ name: "audit-1078", tasks }));
    return Harness.open(storage, { registry }, BACKGROUND_CONTEXT);
}

/**
 * A conversation abort reaches the ordinary ownership scope only: the
 * background task below the root survives it, a direct abort of that task
 * stops it, and a `{background: true}` conversation abort crosses the
 * boundary.
 */
export async function backgroundBoundarySurvivesConversationAbort(): Promise<Scenario> {
    const log: string[] = [];
    const foregroundGate = deferred();
    const backgroundGate = deferred();
    const Foreground = gatedTask("audit.bg-foreground", log, foregroundGate.promise, "foreground");
    const Background = gatedTask("audit.bg-background", log, backgroundGate.promise, "background");
    const harness = await openHarness(new MemoryStorage(), [Foreground, Background]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    let foregroundId: any;
    let backgroundId: any;
    await root.commit(
        async (tx: any) => {
            foregroundId = await tx.createTask(Foreground, null, { ownership: { kind: "conversation" } });
            backgroundId = await tx.createTask(Background, null, {
                ownership: { kind: "conversation" },
                background: true,
            });
        },
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(
        () => log.includes("audit.bg-foreground:run") && log.includes("audit.bg-background:run"),
        "both tasks running",
    );

    await root.abort(BACKGROUND_CONTEXT);
    const foreground = await harness.getTask(foregroundId, BACKGROUND_CONTEXT);
    const backgroundAfterAbort = await harness.getTask(backgroundId, BACKGROUND_CONTEXT);
    const afterAbort = {
        log: [...log],
        foregroundStatus: foreground.state.status,
        foregroundOutcome: foreground.state.outcome,
        backgroundStatus: backgroundAfterAbort.state.status,
        backgroundAbortRequested: backgroundAfterAbort.abortRequested,
    };

    // A direct abort of the background task reaches it, ordinary or not.
    const direct = await harness.abortTask(backgroundId, BACKGROUND_CONTEXT);
    const directReceipt = await harness.waitForTask(backgroundId, BACKGROUND_CONTEXT);
    const afterDirect = {
        marked: direct,
        outcome: directReceipt.state.outcome,
        log: [...log],
    };

    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a conversation abort keeps live background work while a direct abort reaches it",
        source: "packages/durable/src/harness/scheduler.ts:abortConversation,#walkDown,#inScope",
        expected: { afterAbort, afterDirect },
    };
}

/**
 * A `{background: true}` conversation abort crosses background boundaries and
 * waits for the reached tasks, so the background task ends with the abort.
 */
export async function backgroundCrossingConversationAbort(): Promise<Scenario> {
    const log: string[] = [];
    const gate = deferred();
    const Background = gatedTask("audit.bg-cross", log, gate.promise, "background");
    const harness = await openHarness(new MemoryStorage(), [Background]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    let backgroundId: any;
    await root.commit(
        async (tx: any) => {
            backgroundId = await tx.createTask(Background, null, {
                ownership: { kind: "conversation" },
                background: true,
            });
        },
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(() => log.includes("audit.bg-cross:run"), "background task running");

    await root.abort(BACKGROUND_CONTEXT, { background: true });
    const record = await harness.getTask(backgroundId, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a background-crossing conversation abort reaches and waits for a live background task",
        source: "packages/durable/src/harness/scheduler.ts:abortConversation",
        expected: {
            status: record.state.status,
            outcome: record.state.outcome,
            abortRequested: record.abortRequested,
            log,
        },
    };
}

/**
 * `waitForIdle` resolves once the ordinary ownership scope has no live
 * non-background task: live background work does not hold it, and the
 * foreground task that does end releases it.
 */
export async function idleWaitIgnoresBackgroundWork(): Promise<Scenario> {
    const log: string[] = [];
    const foregroundGate = deferred();
    const backgroundGate = deferred();
    const Foreground = gatedTask("audit.idle-foreground", log, foregroundGate.promise, "foreground");
    const Background = gatedTask("audit.idle-background", log, backgroundGate.promise, "background");
    const OtherBackground = gatedTask("audit.idle-other", log, backgroundGate.promise, "other");
    const harness = await openHarness(new MemoryStorage(), [Foreground, Background, OtherBackground]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    let backgroundId: any;
    await root.commit(
        async (tx: any) => {
            await tx.createTask(Foreground, null, { ownership: { kind: "conversation" } });
            backgroundId = await tx.createTask(Background, null, {
                ownership: { kind: "conversation" },
                background: true,
            });
        },
        BACKGROUND_CONTEXT,
    );
    harness.resume();
    await waitUntil(
        () => log.includes("audit.idle-foreground:run") && log.includes("audit.idle-background:run"),
        "both tasks running",
    );

    // A background-only scope is already idle.
    const other = await harness.createConversation({ ownership: { kind: "ownerless" } }, BACKGROUND_CONTEXT);
    await other.commit(
        async (tx: any) => {
            await tx.createTask(OtherBackground, null, {
                ownership: { kind: "conversation" },
                background: true,
            });
        },
        BACKGROUND_CONTEXT,
    );
    await waitUntil(() => log.includes("audit.idle-other:run"), "the other conversation's background task running");
    const backgroundOnlyIdle = await Promise.race([
        other.waitForIdle(BACKGROUND_CONTEXT).then(() => "idle"),
        new Promise((resolve) => setTimeout(() => resolve("held"), 100)),
    ]);

    // The root scope holds on its live foreground task, then releases.
    let rootIdle = false;
    const rootWait = root.waitForIdle(BACKGROUND_CONTEXT).then(() => { rootIdle = true; });
    await new Promise((resolve) => setTimeout(resolve, 50));
    const heldBeforeRelease = !rootIdle;
    foregroundGate.resolve();
    await rootWait;
    const idleAfterForeground = {
        log: [...log],
        backgroundStillLive: (await harness.getTask(backgroundId, BACKGROUND_CONTEXT)).state.status,
    };

    // The background task finishing changes nothing about the ordinary scope:
    // it was idle before and after.
    backgroundGate.resolve();
    const backgroundReceipt = await harness.waitForTask(backgroundId, BACKGROUND_CONTEXT);
    const afterBackground = {
        outcome: backgroundReceipt.state.outcome,
        backgroundStillLive: (await harness.getTask(backgroundId, BACKGROUND_CONTEXT)).state.status,
    };
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "an idle wait resolves with live background work and holds on a live foreground task",
        source: "packages/durable/src/harness/scheduler.ts:waitForIdle,#idle,#walkDown",
        expected: { backgroundOnlyIdle, heldBeforeRelease, idleAfterForeground, afterBackground },
    };
}

/**
 * Ownership restrictions the transaction enforces: a child task of a terminal
 * owner, of a missing owner, a background child and a child in another
 * conversation are all rejected, and so is a conversation owned by a terminal
 * task.
 */
export async function ownershipRestrictionsRejected(): Promise<Scenario> {
    const log: string[] = [];
    const gate = deferred();
    const liveGate = deferred();
    const Parent = gatedTask("audit.own-parent", log, gate.promise, "parent");
    const Live = gatedTask("audit.own-live", log, liveGate.promise, "live");
    const Child = gatedTask("audit.own-child", log, Promise.resolve(), "child");
    const harness = await openHarness(new MemoryStorage(), [Parent, Live, Child]);
    const root = await harness.root(BACKGROUND_CONTEXT);
    const parent = await root.commit(
        (tx: any) => tx.createTask(Parent, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    gate.resolve();
    await harness.waitForTask(parent, BACKGROUND_CONTEXT);
    const other = await harness.createConversation({ ownership: { kind: "ownerless" } }, BACKGROUND_CONTEXT);

    const errors: Record<string, string> = {};
    const attempt = async (name: string, run: (tx: any) => Promise<unknown>) => {
        try {
            await root.commit(async (tx: any) => { await run(tx); }, BACKGROUND_CONTEXT);
            errors[name] = "accepted";
        } catch (error) {
            errors[name] = (error as Error).message;
        }
    };
    await attempt("child_of_terminal", (tx: any) =>
        tx.createTask(Child, null, { ownership: { kind: "task", taskId: parent } }));
    await attempt("child_of_missing", (tx: any) =>
        tx.createTask(Child, null, { ownership: { kind: "task", taskId: 999_999 } }));
    await attempt("background_child", (tx: any) =>
        tx.createTask(Child, null, {
            ownership: { kind: "task", taskId: parent },
            background: true,
        }));
    await attempt("child_other_conversation", (tx: any) =>
        tx.createTask(Child, null, {
            ownership: { kind: "task", taskId: parent },
            conversationId: other.id,
        }));
    await attempt("conversation_of_terminal", (tx: any) =>
        tx.createConversation({ ownership: { kind: "task", taskId: parent } }));

    // A live owner can take a child, and a task-owned conversation names its
    // owner.
    const liveParent = await root.commit(
        (tx: any) => tx.createTask(Live, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    let acceptedChild: any;
    let acceptedConversation: any;
    await root.commit(
        async (tx: any) => {
            acceptedChild = await tx.createTask(Child, null, {
                ownership: { kind: "task", taskId: liveParent },
            });
            acceptedConversation = await tx.createConversation({ ownership: { kind: "task", taskId: liveParent } });
        },
        BACKGROUND_CONTEXT,
    );
    const accepted = await harness.getTask(acceptedChild, BACKGROUND_CONTEXT);
    liveGate.resolve();
    await harness.waitForTask(liveParent, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a child task and a task-owned conversation are rejected for a terminal owner and accepted for a live one",
        source: "packages/durable/src/session/transaction.ts:Transaction.createTask,Transaction.createConversation",
        expected: {
            errors,
            acceptedChildOwner: accepted.owner,
            acceptedConversationOwner: acceptedConversation.owner?.taskId,
            acceptedConversationIdIsFresh: acceptedConversation.id !== root.id && acceptedConversation.id !== other.id,
        },
    };
}
