/**
 * Frozen pi v1.3.0 observations for `audit-1175`: the durable
 * `abandonOnRestart` flag, its first-reservation `restart` mark and the
 * reason-aware abort cascade.
 *
 * `audit-1055` already registers one abandoned-task reopen. This family adds the
 * observations that slice needs to separate the public field from its runtime
 * consumption: the open-only state, the reason an abort handler observes,
 * ordinary-owned-work propagation, background and unflagged boundaries, a
 * missing definition that waits instead of orphaning, and an explicit request
 * that replaces the reason down the tree.
 *
 * Every scenario drives public `Harness` calls and returns one comparison unit
 * for the manifest. The measured code is the frozen checkout named by
 * `PI_CHECKOUT`, not this repository.
 */

import {
    BACKGROUND_CONTEXT,
    createModels,
    createRegistry,
    defineExtension,
    defineTask,
    Harness,
    MemoryStorage,
    openNodeSqliteStorage,
    type Scenario,
} from "./pi.mts";

export function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

async function sleep(ms: number): Promise<void> {
    await new Promise((resolve) => setTimeout(resolve, ms));
}

async function eventually(check: () => boolean | Promise<boolean>, label: string, timeoutMs = 5000): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!(await check())) {
        if (Date.now() > deadline) throw new Error(`Condition was not reached: ${label}`);
        await sleep(2);
    }
}

interface HarnessHandle {
    harness: any;
    registry: any;
}

async function openHarness(storage: any, tasks: readonly any[] = []): Promise<HarnessHandle> {
    const registry = createRegistry();
    if (tasks.length > 0) registry.install(defineExtension({ name: "audit1175", tasks }));
    const harness = await Harness.open(storage, { models: createModels(), registry }, BACKGROUND_CONTEXT);
    return { harness, registry };
}

function openSqlite(path: string): Promise<any> {
    return openNodeSqliteStorage(path);
}

/**
 * One task kind whose run phase would complete with `ran`, and whose abort
 * handler records the reason it observed and ends `aborted` with that reason
 * (or `requested` when the record carries none).
 */
function roleTask(name: string, events: string[]) {
    return defineTask({
        name,
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                events.push(`${name}:run`);
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: "ran" } }),
                    context,
                );
            },
        },
        abort: async (task: any, runtime: any, context: any) => {
            const reason = task.abortReason ?? "requested";
            events.push(`${name}:abort:${reason}`);
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason } }),
                context,
            );
        },
    });
}

/** The stable projection of one task record the comparisons below use. */function project(record: any) {
    return {
        status: record?.state.status ?? null,
        abandonOnRestart: record?.abandonOnRestart === true,
        abortRequested: record?.abortRequested === true,
        abortReason: record?.abortReason ?? null,
        checkpointPhase: record?.state.checkpoint?.phase ?? null,
        outcomeStatus: record?.state.outcome?.status ?? null,
        outcomeReason: record?.state.outcome?.reason ?? null,
    };
}

/** A task whose run phase stays live until its abort signal, then cleans up. */
function dormantTask(name: string, events: string[]) {
    return defineTask({
        name,
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any) => {
                events.push(`${name}:run`);
                await new Promise<never>((_, reject) => {
                    if (runtime.signal.aborted) {
                        reject(runtime.signal.reason);
                        return;
                    }
                    runtime.signal.addEventListener("abort", () => reject(runtime.signal.reason), { once: true });
                });
            },
        },
        abort: async (task: any, runtime: any, context: any) => {
            const reason = task.abortReason ?? "requested";
            events.push(`${name}:abort:${reason}`);
            await runtime.commit(
                () => ({ status: "terminal", outcome: { status: "aborted", reason } }),
                context,
            );
        },
    });
}

function assertEqual(actual: unknown, expected: unknown, label: string): void {
    if (JSON.stringify(actual) !== JSON.stringify(expected)) {
        throw new Error(
            `${label} mismatch:\n  expected ${JSON.stringify(expected)}\n  actual   ${JSON.stringify(actual)}`,
        );
    }
}

const pausedFlagged = {
    status: "pending",
    abandonOnRestart: true,
    abortRequested: false,
    abortReason: null,
    checkpointPhase: "run",
    outcomeStatus: null,
    outcomeReason: null,
};

const settledRestart = {
    status: "terminal",
    abandonOnRestart: true,
    abortRequested: true,
    abortReason: "restart",
    checkpointPhase: null,
    outcomeStatus: "aborted",
    outcomeReason: "restart",
};

const restartMarkedWaiting = {
    ...pausedFlagged,
    abortRequested: true,
    abortReason: "restart",
};

/** Observe one reopened Harness, then resume it and settle one task. */
async function observeAndSettle(second: HarnessHandle, id: any, events: string[]) {
    const schedulingAfterOpen = (await second.harness.inspect(BACKGROUND_CONTEXT)).scheduling;
    const afterOpen = project(await second.harness.getTask(id, BACKGROUND_CONTEXT));
    await sleep(50);
    const afterOpenWithoutScheduling = project(await second.harness.getTask(id, BACKGROUND_CONTEXT));
    second.harness.resume();
    const settlement = project(await second.harness.waitForTask(id, BACKGROUND_CONTEXT));
    await second.harness.close(BACKGROUND_CONTEXT);
    return { schedulingAfterOpen, afterOpen, afterOpenWithoutScheduling, settlement, log: events };
}

// ── 1. Open-only state and the first later reservation pass ────────────────────────────────────

/**
 * A flagged task created by one Harness is unmarked while the next Harness
 * stays paused; its first enabled reservation pass marks it `restart`, so its
 * run phase never executes and its abort handler observes `restart`. The same
 * observation is taken over in-memory storage (a later Harness over the same
 * live records; MemoryStorage has no process-level reopen) and over a closed
 * and reopened SQLite store.
 */
export async function openOnlyVersusFirstReservation(): Promise<Scenario> {
    const memory = await (async () => {
        const events: string[] = [];
        const Flagged = roleTask("audit1175.flagged", events);
        const storage = new MemoryStorage();
        const first = await openHarness(storage, [Flagged]);
        const root = await first.harness.root(BACKGROUND_CONTEXT);
        const id = await root.commit(
            (tx: any) =>
                tx.createTask(Flagged, null, { ownership: { kind: "conversation" }, abandonOnRestart: true }),
            BACKGROUND_CONTEXT,
        );
        const created = project(await first.harness.getTask(id, BACKGROUND_CONTEXT));
        // A later Harness over the same records; the first one is left unclosed
        // because closing it would close the shared in-memory storage.
        const second = await openHarness(storage, [Flagged]);
        return { created, ...(await observeAndSettle(second, id, events)) };
    })();

    const sqlite = await (async () => {
        const events: string[] = [];
        const Flagged = roleTask("audit1175.flagged", events);
        const path = `/tmp/pi-1175-${process.pid}-${Date.now()}.sqlite`;
        const first = await openHarness(await openSqlite(path), [Flagged]);
        const root = await first.harness.root(BACKGROUND_CONTEXT);
        const id = await root.commit(
            (tx: any) =>
                tx.createTask(Flagged, null, { ownership: { kind: "conversation" }, abandonOnRestart: true }),
            BACKGROUND_CONTEXT,
        );
        const created = project(await first.harness.getTask(id, BACKGROUND_CONTEXT));
        await first.harness.close(BACKGROUND_CONTEXT);
        const second = await openHarness(await openSqlite(path), [Flagged]);
        return { created, ...(await observeAndSettle(second, id, events)) };
    })();

    const expected = {
        created: pausedFlagged,
        schedulingAfterOpen: "paused",
        afterOpen: pausedFlagged,
        afterOpenWithoutScheduling: pausedFlagged,
        settlement: settledRestart,
        log: ["audit1175.flagged:abort:restart"],
    };
    assertEqual(memory, expected, "memory open/resume");
    assertEqual(sqlite, expected, "sqlite reopen/resume");
    return {
        name: "a flagged task stays unmarked while the next Harness is paused and is marked restart by its first reservation pass",
        inputs: {
            storage: ["MemoryStorage identity (later Harness, first left unclosed)", "SQLite close/reopen"],
            openOnlyWaitMs: 50,
        },
        expected,
        normalizations: ["the temporary SQLite path and generated task id are omitted"],
        upstreamTest: {
            path: "packages/durable/test/harness-nested-tools-restart.test.ts",
            case: "marks nothing when the reopened Harness never starts scheduling",
        },
    };
}

// ── 2. Restart reason propagates to ordinary owned work ───────────────────────────────────────

/**
 * The `restart` mark cascades bottom-up to the abandoned task's ordinary owned
 * child with the same reason, so neither run phase executes. A background task
 * in a conversation the abandoned task owns and an unflagged sibling keep
 * running and never receive a mark.
 */
export async function restartCascadeBoundaries(): Promise<Scenario> {
    const events: string[] = [];
    const Parent = roleTask("audit1175.parent", events);
    const Child = roleTask("audit1175.child", events);
    const Background = roleTask("audit1175.background", events);
    const Sibling = roleTask("audit1175.sibling", events);
    const path = `/tmp/pi-1175-cascade-${process.pid}-${Date.now()}.sqlite`;

    const first = await openHarness(await openSqlite(path), [Parent, Child, Background, Sibling]);
    const root = await first.harness.root(BACKGROUND_CONTEXT);
    const parent = await root.commit(
        (tx: any) => tx.createTask(Parent, null, { ownership: { kind: "conversation" }, abandonOnRestart: true }),
        BACKGROUND_CONTEXT,
    );
    const child = await root.commit(
        (tx: any) => tx.createTask(Child, null, { ownership: { kind: "task", taskId: parent } }),
        BACKGROUND_CONTEXT,
    );
    const backgroundConversation = await first.harness.createConversation(
        { ownership: { kind: "task", taskId: parent } },
        BACKGROUND_CONTEXT,
    );
    const background = await backgroundConversation.commit(
        (tx: any) =>
            tx.createTask(Background, null, {
                conversationId: backgroundConversation.id,
                ownership: { kind: "conversation" },
                background: true,
            }),
        BACKGROUND_CONTEXT,
    );
    const sibling = await root.commit(
        (tx: any) => tx.createTask(Sibling, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    await first.harness.close(BACKGROUND_CONTEXT);

    const second = await openHarness(await openSqlite(path), [Parent, Child, Background, Sibling]);
    const afterOpen = {
        parent: project(await second.harness.getTask(parent, BACKGROUND_CONTEXT)),
        child: project(await second.harness.getTask(child, BACKGROUND_CONTEXT)),
        background: project(await second.harness.getTask(background, BACKGROUND_CONTEXT)),
        sibling: project(await second.harness.getTask(sibling, BACKGROUND_CONTEXT)),
    };
    second.harness.resume();
    const receipts = await Promise.all([
        second.harness.waitForTask(parent, BACKGROUND_CONTEXT),
        second.harness.waitForTask(child, BACKGROUND_CONTEXT),
        second.harness.waitForTask(background, BACKGROUND_CONTEXT),
        second.harness.waitForTask(sibling, BACKGROUND_CONTEXT),
    ]);
    await second.harness.close(BACKGROUND_CONTEXT);

    const unsettled = {
        status: "pending",
        abandonOnRestart: false,
        abortRequested: false,
        abortReason: null,
        checkpointPhase: "run",
        outcomeStatus: null,
        outcomeReason: null,
    };
    const expected = {
        afterOpen: {
            parent: pausedFlagged,
            child: unsettled,
            background: unsettled,
            sibling: unsettled,
        },
        settlement: {
            parent: settledRestart,
            child: { ...settledRestart, abandonOnRestart: false },
            background: {
                status: "terminal",
                abandonOnRestart: false,
                abortRequested: false,
                abortReason: null,
                checkpointPhase: null,
                outcomeStatus: "completed",
                outcomeReason: null,
            },
            sibling: {
                status: "terminal",
                abandonOnRestart: false,
                abortRequested: false,
                abortReason: null,
                checkpointPhase: null,
                outcomeStatus: "completed",
                outcomeReason: null,
            },
        },
        abortLog: events.filter((entry) => entry.includes(":abort:")),
        runLog: events.filter((entry) => entry.includes(":run")).sort(),
    };
    assertEqual(
        {
            afterOpen,
            settlement: {
                parent: project(receipts[0]),
                child: project(receipts[1]),
                background: project(receipts[2]),
                sibling: project(receipts[3]),
            },
            abortLog: events.filter((entry) => entry.includes(":abort:")),
            runLog: events.filter((entry) => entry.includes(":run")).sort(),
        },
        expected,
        "restart cascade",
    );
    return {
        name: "a restart mark cascades to ordinary owned work with the same reason and stops at background and unflagged work",
        inputs: { storage: "SQLite close/reopen", parent: "flagged", child: "ordinary owned", background: "below a task-owned conversation" },
        expected,
        normalizations: ["the temporary SQLite path and generated task ids are omitted"],
        upstreamTest: {
            path: "packages/durable/test/harness-nested-tools-restart.test.ts",
            case: "leaves background work under an abandoned nested call running, and settles without it",
        },
    };
}

// ── 3. Missing definition waits, explicit abort orphans, definition returns ──────────────────

/**
 * A restart-marked task whose definition is missing stays pending and blocked,
 * not orphaned. An explicit abort request replaces the reason and orphans it,
 * while a sibling restart-marked task keeps waiting and runs its abort handler
 * with reason `restart` once its definition is installed.
 */
export async function missingDefinitionVersusExplicitAbort(): Promise<Scenario> {
    const events: string[] = [];
    const Missing = roleTask("audit1175.missing", events);
    const Returns = roleTask("audit1175.returns", events);
    const path = `/tmp/pi-1175-missing-${process.pid}-${Date.now()}.sqlite`;

    const first = await openHarness(await openSqlite(path), [Missing, Returns]);
    const root = await first.harness.root(BACKGROUND_CONTEXT);
    const missing = await root.commit(
        (tx: any) => tx.createTask(Missing, null, { ownership: { kind: "conversation" }, abandonOnRestart: true }),
        BACKGROUND_CONTEXT,
    );
    const returns = await root.commit(
        (tx: any) => tx.createTask(Returns, null, { ownership: { kind: "conversation" }, abandonOnRestart: true }),
        BACKGROUND_CONTEXT,
    );
    await first.harness.close(BACKGROUND_CONTEXT);

    // Reopen without either definition: both are restart-marked and wait.
    const second = await openHarness(await openSqlite(path), []);
    second.harness.resume();
    await eventually(async () => {
        const left = await second.harness.getTask(missing, BACKGROUND_CONTEXT);
        const right = await second.harness.getTask(returns, BACKGROUND_CONTEXT);
        return left?.abortReason === "restart" && right?.abortReason === "restart";
    }, "both missing-definition tasks are restart-marked");
    const blocked = (await second.harness.inspect(BACKGROUND_CONTEXT)).tasks.find(
        (task: any) => task.record.id === missing,
    );
    const waitingBeforeAbort = {
        missing: project(await second.harness.getTask(missing, BACKGROUND_CONTEXT)),
        returns: project(await second.harness.getTask(returns, BACKGROUND_CONTEXT)),
        missingInspection: blocked?.state ?? null,
    };

    const abortResult = await second.harness.abortTask(missing, BACKGROUND_CONTEXT);
    const missingReceipt = project(await second.harness.waitForTask(missing, BACKGROUND_CONTEXT));

    second.registry.install(defineExtension({ name: "audit1175.returns", tasks: [Returns] }));
    const returnsReceipt = project(await second.harness.waitForTask(returns, BACKGROUND_CONTEXT));
    await second.harness.close(BACKGROUND_CONTEXT);

    const orphaned = {
        status: "terminal",
        abandonOnRestart: true,
        abortRequested: true,
        abortReason: "restart",
        checkpointPhase: null,
        outcomeStatus: "orphaned",
        outcomeReason: "missing_task",
    };
    const expected = {
        waitingBeforeAbort: {
            missing: restartMarkedWaiting,
            returns: restartMarkedWaiting,
            missingInspection: { kind: "blocked", reason: "missing_task" },
        },
        abortResult: "marked",
        missingReceipt: orphaned,
        returnsReceipt: settledRestart,
        log: ["audit1175.returns:abort:restart"],
    };
    assertEqual(
        {
            waitingBeforeAbort,
            abortResult,
            missingReceipt,
            returnsReceipt,
            log: events,
        },
        expected,
        "missing-definition recovery",
    );
    return {
        name: "a restart-marked task with a missing definition waits; an explicit abort orphans it; a restored definition runs its abort with reason restart",
        inputs: { storage: "SQLite close/reopen", definitions: "absent at reopen, installed for one task later" },
        expected,
        normalizations: ["the temporary SQLite path and generated task ids are omitted"],
        upstreamTest: {
            path: "packages/durable/test/harness-nested-tools-restart.test.ts",
            case: "keeps an abandoned task whose definition is missing waiting until it is installed, then runs its abort",
        },
    };
}

// ── 4. An explicit request replaces restart marks down the tree ───────────────────────────────

/**
 * Only the flagged middle task is abandonment-marked at open; its unflagged
 * owner and its missing-definition leaf are not. The restart cascade marks the
 * leaf with reason `restart`, so it waits. A request on the unflagged top task
 * replaces the reasons two levels down; the leaf is then orphaned and the two
 * reachable tasks run their abort handlers with no reason.
 */
export async function explicitAbortReplacesRestartDownTheTree(): Promise<Scenario> {
    const events: string[] = [];
    const Top = dormantTask("audit1175.top", events);
    const Middle = roleTask("audit1175.middle", events);
    const Leaf = roleTask("audit1175.leaf", events);
    const path = `/tmp/pi-1175-upgrade-${process.pid}-${Date.now()}.sqlite`;

    const first = await openHarness(await openSqlite(path), [Top, Middle, Leaf]);
    const root = await first.harness.root(BACKGROUND_CONTEXT);
    const top = await root.commit(
        (tx: any) => tx.createTask(Top, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    const middle = await root.commit(
        (tx: any) =>
            tx.createTask(Middle, null, { ownership: { kind: "task", taskId: top }, abandonOnRestart: true }),
        BACKGROUND_CONTEXT,
    );
    const leaf = await root.commit(
        (tx: any) => tx.createTask(Leaf, null, { ownership: { kind: "task", taskId: middle } }),
        BACKGROUND_CONTEXT,
    );
    await first.harness.close(BACKGROUND_CONTEXT);

    // The leaf definition is missing, so it waits under the restart cascade.
    const second = await openHarness(await openSqlite(path), [Top, Middle]);
    second.harness.resume();
    await eventually(async () => {
        const record = await second.harness.getTask(leaf, BACKGROUND_CONTEXT);
        return record?.abortReason === "restart" && record?.state.status !== "terminal";
    }, "the leaf waits under the restart cascade");
    await eventually(() => events.includes("audit1175.top:run"), "the unflagged owner is running");
    const beforeRequest = {
        top: project(await second.harness.getTask(top, BACKGROUND_CONTEXT)),
        middle: project(await second.harness.getTask(middle, BACKGROUND_CONTEXT)),
        leaf: project(await second.harness.getTask(leaf, BACKGROUND_CONTEXT)),
    };

    const abortResult = await second.harness.abortTask(top, BACKGROUND_CONTEXT);
    const receipts = await Promise.all([
        second.harness.waitForTask(leaf, BACKGROUND_CONTEXT),
        second.harness.waitForTask(middle, BACKGROUND_CONTEXT),
        second.harness.waitForTask(top, BACKGROUND_CONTEXT),
    ]);
    await second.harness.close(BACKGROUND_CONTEXT);

    const expected = {
        beforeRequest: {
            top: {
                status: "running",
                abandonOnRestart: false,
                abortRequested: false,
                abortReason: null,
                checkpointPhase: "run",
                outcomeStatus: null,
                outcomeReason: null,
            },
            middle: restartMarkedWaiting,
            leaf: { ...restartMarkedWaiting, abandonOnRestart: false },
        },
        abortResult: "marked",
        settlement: {
            leaf: {
                status: "terminal",
                abandonOnRestart: false,
                abortRequested: true,
                abortReason: null,
                checkpointPhase: null,
                outcomeStatus: "orphaned",
                outcomeReason: "missing_task",
            },
            middle: {
                status: "terminal",
                abandonOnRestart: true,
                abortRequested: true,
                abortReason: null,
                checkpointPhase: null,
                outcomeStatus: "aborted",
                outcomeReason: "requested",
            },
            top: {
                status: "terminal",
                abandonOnRestart: false,
                abortRequested: true,
                abortReason: null,
                checkpointPhase: null,
                outcomeStatus: "aborted",
                outcomeReason: "requested",
            },
        },
        log: ["audit1175.top:run", "audit1175.middle:abort:requested", "audit1175.top:abort:requested"],
    };
    assertEqual(
        {
            beforeRequest,
            abortResult,
            settlement: {
                leaf: project(receipts[0]),
                middle: project(receipts[1]),
                top: project(receipts[2]),
            },
            log: events,
        },
        expected,
        "request replaces restart",
    );
    return {
        name: "an explicit abort above a restart-marked subtree replaces the reason and orphans a blocked leaf",
        inputs: {
            storage: "SQLite close/reopen",
            top: "unflagged conversation-owned",
            middle: "flagged owned by top",
            leaf: "unflagged owned by middle, definition missing",
        },
        expected,
        normalizations: ["the temporary SQLite path and generated task ids are omitted"],
        upstreamTest: {
            path: "packages/durable/test/harness-cancellation-barrier.test.ts",
            case: "upgrades restart marks down the tree when an abort is requested above them",
        },
    };
}

// ── 5. A flagged task created after open is not abandoned ─────────────────────────────────────

/**
 * Only live flagged tasks the next Harness finds at open are abandoned. A task
 * created with the flag after that Harness opened belongs to the running
 * process and runs its phase normally, even though its record carries the flag.
 * This separates "the first reservation pass marks the open-time set" from a
 * scanner that abandons every flagged record it sees.
 */
export async function flaggedTaskCreatedAfterOpenRuns(): Promise<Scenario> {
    const events: string[] = [];
    const Flagged = roleTask("audit1175.late", events);
    const harness = await openHarness(new MemoryStorage(), [Flagged]);
    const root = await harness.harness.root(BACKGROUND_CONTEXT);
    const id = await root.commit(
        (tx: any) => tx.createTask(Flagged, null, { ownership: { kind: "conversation" }, abandonOnRestart: true }),
        BACKGROUND_CONTEXT,
    );
    const created = project(await harness.harness.getTask(id, BACKGROUND_CONTEXT));
    harness.harness.resume();
    const settlement = project(await harness.harness.waitForTask(id, BACKGROUND_CONTEXT));
    await harness.harness.close(BACKGROUND_CONTEXT);
    const expected = {
        created: pausedFlagged,
        settlement: {
            status: "terminal",
            abandonOnRestart: true,
            abortRequested: false,
            abortReason: null,
            checkpointPhase: null,
            outcomeStatus: "completed",
            outcomeReason: null,
        },
        log: ["audit1175.late:run"],
    };
    assertEqual({ created, settlement, log: events }, expected, "created after open");
    return {
        name: "a flagged task created after the Harness opened runs instead of being abandoned",
        inputs: { storage: "MemoryStorage", createdAfterOpen: true },
        expected,
        normalizations: ["the generated task id is omitted"],
        upstreamTest: {
            path: "packages/durable/src/harness/scheduler.ts",
            case: "TaskScheduler.open's #abandoned set is populated only from records read at open",
        },
    };
}
