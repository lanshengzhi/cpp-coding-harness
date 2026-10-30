/**
 * `audit-1093` scenarios: the deterministic-run OUTCOME-before-settle ordering
 * that the `audit-1053` `deterministic-replay` artifact did not record.
 *
 * Frozen contract under observation (`packages/codemode/src/types.ts`,
 * `CodemodeRun.finished`): "Whether the run has its outcome ... Set when that
 * is decided, before `result` settles; calls can no longer be answered. It
 * does not mean the VM has exited." The upstream unit tests
 * (`packages/codemode/test/deterministic.test.ts`, "ends with an error the
 * script cannot catch when the caller aborts" and "is finished once it has its
 * outcome") assert the transitions but not the ordering window; this capture
 * records the window as a public-caller observation.
 *
 * Each scenario runs its own sandbox and reports expected === observed.
 */

import { CodemodeSandbox, loadQuickJSWasm, type Scenario } from "./pi.mts";

const STARTED_AT = 1768471200000;
const WASM = await loadQuickJSWasm();

const ABORT_SCRIPT = 'try { await tools.wait(1); } catch { text("caught"); } text("after");';

/** Host-side view of one started run, with the hooks the ordering needs. */
interface RecordedRun {
    run: {
        result: Promise<any>;
        readonly finished: boolean;
        deliver(seq: number, now: number, id: number, outcome: unknown): unknown;
    };
    calls: unknown[];
}

function startRecorded(sandbox: any, code: string, options: Record<string, unknown>): RecordedRun {
    const calls: unknown[] = [];
    const run = sandbox.start(code, {
        startedAt: STARTED_AT,
        seed: 1,
        tools: [{ name: "wait" }],
        timeoutMs: 20_000,
        ...options,
        onCall: (call: unknown) => {
            calls.push(call);
        },
    });
    return { run, calls };
}

async function waitForCalls(calls: unknown[], count: number): Promise<void> {
    const deadline = Date.now() + 20_000;
    while (calls.length < count && Date.now() < deadline) {
        await new Promise((resolve) => setTimeout(resolve, 2));
    }
    if (calls.length < count) throw new Error(`timed out waiting for ${count} calls, got ${calls.length}`);
}

/** Maps a settled CodemodeResult to the compared projection (durations become presence flags). */
function resultProjection(result: any): Record<string, unknown> {
    return {
        ok: result.ok,
        error: result.error ? { kind: result.error.kind, message: result.error.message } : null,
        output: result.output,
        calls: result.calls.map((call: any) => ({
            name: call.name,
            status: call.status,
            durationPresent: typeof call.durationMs === "number",
        })),
    };
}

/** The caller aborts a run parked on a call: the outcome is visible strictly before the result settles. */
export async function outcomeVisibleBeforeSettleOnAbort(): Promise<Scenario> {
    const sandbox = new CodemodeSandbox({ wasm: WASM });
    try {
        const controller = new AbortController();
        // Settle-continuation witness: pi's finish() resolves the result
        // promise synchronously, so a .then callback records whether the
        // caller's continuation has run yet.
        let settleContinuationRan = false;
        const recorded = startRecorded(sandbox, ABORT_SCRIPT, { signal: controller.signal });
        void recorded.run.result.then(() => {
            settleContinuationRan = true;
        });
        await waitForCalls(recorded.calls, 1);

        const finishedWhileWaiting = recorded.run.finished;
        controller.abort(new Error("diverged"));
        const finishedWhenAbortReturns = recorded.run.finished;
        const settleContinuationWhenAbortReturns = settleContinuationRan;

        let deliverAfterFinishThrew = false;
        let deliverAfterFinishReturnedUndefined = false;
        try {
            deliverAfterFinishReturnedUndefined =
                recorded.run.deliver(1, STARTED_AT, 1, { ok: true, value: null }) === undefined;
        } catch {
            deliverAfterFinishThrew = true;
        }
        const finishedAfterLateAnswer = recorded.run.finished;

        const result = await recorded.run.result;
        const expected = {
            finishedWhileWaiting: false,
            finishedWhenAbortReturns: true,
            settleContinuationWhenAbortReturns: false,
            deliverAfterFinishThrew: false,
            deliverAfterFinishReturnedUndefined: true,
            finishedAfterLateAnswer: true,
            result: {
                ok: false,
                error: { kind: "aborted", message: "diverged" },
                output: [],
                calls: [{ name: "wait", status: "cancelled", durationPresent: true }],
            },
        };
        const observed = {
            finishedWhileWaiting,
            finishedWhenAbortReturns,
            settleContinuationWhenAbortReturns,
            deliverAfterFinishThrew,
            deliverAfterFinishReturnedUndefined,
            finishedAfterLateAnswer,
            result: resultProjection(result),
        };
        return {
            name: "the outcome is visible before the result settles when the caller aborts",
            source: "packages/codemode/src/types.ts:CodemodeRun.finished; packages/codemode/src/runtime/execution.ts:Execution.finish,CodemodeRun.finished; packages/codemode/test/deterministic.test.ts:ends with an error the script cannot catch when the caller aborts",
            inputs: {
                script: ABORT_SCRIPT,
                cancellation: "after start, while the call waits",
                abortMessage: "diverged",
                lateAnswer: { seq: 1, now: STARTED_AT, id: 1, outcome: { ok: true, value: null } },
            },
            expected,
            observed,
        };
    } finally {
        await sandbox.close();
    }
}

/** A pre-aborted run: the outcome is decided in the Execution constructor, so it is visible when `start` returns. */
export async function preAbortedRunIsFinishedAtStart(): Promise<Scenario> {
    const sandbox = new CodemodeSandbox({ wasm: WASM });
    try {
        const controller = new AbortController();
        controller.abort(new Error("diverged"));
        let settleContinuationRan = false;
        const recorded = startRecorded(sandbox, "await tools.wait(1);", { signal: controller.signal });
        void recorded.run.result.then(() => {
            settleContinuationRan = true;
        });
        // No wait: the run never reaches the VM.
        const finishedAtStart = recorded.run.finished;
        const settleContinuationAtStart = settleContinuationRan;

        let deliverAfterFinishThrew = false;
        let deliverAfterFinishReturnedUndefined = false;
        try {
            deliverAfterFinishReturnedUndefined =
                recorded.run.deliver(1, STARTED_AT, 1, { ok: true, value: null }) === undefined;
        } catch {
            deliverAfterFinishThrew = true;
        }
        const finishedAfterLateAnswer = recorded.run.finished;

        const result = await recorded.run.result;
        const expected = {
            finishedAtStart: true,
            settleContinuationAtStart: false,
            deliverAfterFinishThrew: false,
            deliverAfterFinishReturnedUndefined: true,
            finishedAfterLateAnswer: true,
            result: {
                ok: false,
                error: { kind: "aborted", message: "diverged" },
                output: [],
                calls: [],
            },
        };
        const observed = {
            finishedAtStart,
            settleContinuationAtStart,
            deliverAfterFinishThrew,
            deliverAfterFinishReturnedUndefined,
            finishedAfterLateAnswer,
            result: resultProjection(result),
        };
        return {
            name: "a pre-aborted run is finished when start returns",
            source: "packages/codemode/src/types.ts:CodemodeRun.finished; packages/codemode/src/runtime/execution.ts:Execution constructor abort signal handling,Execution.finish; packages/codemode/test/deterministic.test.ts:is finished once it has its outcome",
            inputs: {
                script: "await tools.wait(1);",
                cancellation: "the AbortSignal is already aborted when start runs",
                abortMessage: "diverged",
                lateAnswer: { seq: 1, now: STARTED_AT, id: 1, outcome: { ok: true, value: null } },
            },
            expected,
            observed,
        };
    } finally {
        await sandbox.close();
    }
}

/** The normal path: finished is false while a call waits and true by the time the result continuation runs. */
export async function finishedTransitionsOnTheNormalPath(): Promise<Scenario> {
    const sandbox = new CodemodeSandbox({ wasm: WASM });
    try {
        let finishedAtSettleContinuation: boolean | null = null;
        const recorded = startRecorded(sandbox, "await tools.wait(1); return 2;", {});
        void recorded.run.result.then(() => {
            finishedAtSettleContinuation = recorded.run.finished;
        });
        await waitForCalls(recorded.calls, 1);
        const finishedWhileWaiting = recorded.run.finished;

        recorded.run.deliver(1, STARTED_AT, 1, { ok: true, value: 2 });
        const result = await recorded.run.result;

        let deliverAfterFinishThrew = false;
        let deliverAfterFinishReturnedUndefined = false;
        try {
            deliverAfterFinishReturnedUndefined =
                recorded.run.deliver(2, STARTED_AT, 1, { ok: true, value: null }) === undefined;
        } catch {
            deliverAfterFinishThrew = true;
        }
        const finishedAfterLateAnswer = recorded.run.finished;

        const expected = {
            finishedWhileWaiting: false,
            finishedAtSettleContinuation: true,
            deliverAfterFinishThrew: false,
            deliverAfterFinishReturnedUndefined: true,
            finishedAfterLateAnswer: true,
            result: {
                ok: true,
                error: null,
                output: [],
                calls: [{ name: "wait", status: "ok", durationPresent: true }],
            },
        };
        const observed = {
            finishedWhileWaiting,
            finishedAtSettleContinuation,
            deliverAfterFinishThrew,
            deliverAfterFinishReturnedUndefined,
            finishedAfterLateAnswer,
            result: resultProjection(result),
        };
        return {
            name: "finished reads false while a call waits and true when the result continuation runs",
            source: "packages/codemode/src/types.ts:CodemodeRun.finished; packages/codemode/src/runtime/execution.ts:Execution.finish; packages/codemode/test/deterministic.test.ts:is finished once it has its outcome",
            inputs: {
                script: "await tools.wait(1); return 2;",
                answer: { seq: 1, now: STARTED_AT, id: 1, outcome: { ok: true, value: 2 } },
                lateAnswer: { seq: 2, now: STARTED_AT, id: 1, outcome: { ok: true, value: null } },
            },
            expected,
            observed,
        };
    } finally {
        await sandbox.close();
    }
}
