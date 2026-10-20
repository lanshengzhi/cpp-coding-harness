/**
 * Frozen pi v1.3.0 Chord replicated-state observations for ticket #1062:
 * ordered document frames, replay/restore from a source cursor, bounded
 * pending delivery with newest-coalescing overflow, and observer isolation.
 *
 * The capture lives in the Pike repository but executes the private checkout
 * named by `PI_CHECKOUT`; `capture-1062.mts` guards its exact revision. Only
 * the Chord-level contract (`packages/chord/src/state.ts`) is observed here;
 * the durable Session watch (`packages/durable/src/session/observation.ts`)
 * remains a #1063 concern and its registered audit-1057 watch scenarios are
 * intentionally not promoted.
 */

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

export interface Scenario {
    readonly name: string;
    readonly source: string;
    readonly inputs: unknown;
    readonly expected: unknown;
    readonly normalizations?: readonly string[];
}

/* eslint-disable @typescript-eslint/no-explicit-any */
type Any = any;

const checkout = process.env.PI_CHECKOUT;
if (checkout === undefined || checkout === "") throw new Error("PI_CHECKOUT must name the frozen pi v1.3.0 checkout");

const statePath = path.join(checkout, "packages/chord/src/index.ts");
const contextPath = path.join(checkout, "packages/chord/src/context/index.ts");
if (!existsSync(statePath)) throw new Error(`missing frozen Chord state source: ${statePath}`);
const stateModule = (await import(pathToFileURL(statePath).href)) as Any;
const contextModule = (await import(pathToFileURL(contextPath).href)) as Any;

const replicatedState = stateModule.replicatedState as (source: Any, options?: Any) => Any;
const BACKGROUND_CONTEXT = contextModule.BACKGROUND_CONTEXT as Any;
const withCancel = contextModule.withCancel as (parent: Any) => { context: Any } & Any;

const sleep = (ms = 5): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

function deferred<T>(): { promise: Promise<T>; resolve: (value: T) => void; reject: (error: unknown) => void } {
    let resolve!: (value: T) => void;
    let reject!: (error: unknown) => void;
    const promise = new Promise<T>((res, rej) => {
        resolve = res;
        reject = rej;
    });
    return { promise, resolve, reject };
}

// Async listener settlements chain through microtasks; one macrotask tick
// after the awaited gate drains every pending continuation.
async function settled(): Promise<void> {
    await sleep(20);
}

function jsonClone<T>(value: T): T {
    return JSON.parse(JSON.stringify(value)) as T;
}

type SourceValue = Readonly<{ value: number }>;

/** The frozen test source from packages/chord/test/replicated-state.test.ts. */
class TestSource {
    readonly attachments = new Set<TestSourceAttachment>();
    onAttach: (() => void) | undefined;
    #value: SourceValue;
    #cursor: number;

    constructor(value: SourceValue, cursor = 0) {
        this.#value = value;
        this.#cursor = cursor;
    }

    attach(): TestSourceAttachment {
        const attachment = new TestSourceAttachment(
            Object.freeze({ value: this.#value, cursor: this.#cursor }),
            () => this.attachments.delete(attachment),
        );
        this.attachments.add(attachment);
        this.onAttach?.();
        return attachment;
    }

    commit(value: SourceValue, ops: readonly unknown[], context: Any = BACKGROUND_CONTEXT, cursor = this.#cursor + 1): void {
        this.#value = value;
        this.#cursor = cursor;
        const frame = Object.freeze({ cursor, value, ops, context });
        for (const attachment of [...this.attachments]) attachment.publish(frame);
    }
}

class TestSourceAttachment {
    readonly snapshot: { readonly value: SourceValue; readonly cursor: number };
    readonly #onDispose: () => void;
    readonly #buffer: Any[] = [];
    #listener: ((frame: Any) => void) | undefined;
    #activated = false;
    #disposed = false;
    disposeCount = 0;

    constructor(snapshot: { readonly value: SourceValue; readonly cursor: number }, onDispose: () => void) {
        this.snapshot = snapshot;
        this.#onDispose = onDispose;
    }

    get activated(): boolean {
        return this.#activated;
    }

    activate(listener: (frame: Any) => void): void {
        if (this.#activated) throw new Error("attachment is already active");
        if (this.#disposed) throw new Error("attachment is disposed");
        this.#activated = true;
        this.#listener = listener;
        for (const frame of this.#buffer.splice(0)) listener(frame);
    }

    publish(frame: Any): void {
        if (this.#disposed) return;
        if (this.#listener === undefined) this.#buffer.push(frame);
        else this.#listener(frame);
    }

    dispose(): void {
        if (this.#disposed) return;
        this.#disposed = true;
        this.disposeCount += 1;
        this.#buffer.length = 0;
        this.#onDispose();
    }
}

/** One committed frame matching the frozen frame contract. */
function frame(value: number, cursor: number, context: Any = BACKGROUND_CONTEXT): Any {
    return Object.freeze({ cursor, value: Object.freeze({ value }), ops: Object.freeze([["s", ["value"], value]]), context });
}

/** Commit `value` at cursor `value` through the frozen test source contract. */
function commitFrame(source: TestSource, value: number, context: Any = BACKGROUND_CONTEXT): void {
    source.commit(Object.freeze({ value }), Object.freeze([["s", ["value"], value]]), context, value);
}

/** Ordered frames, replay/restore and attachment independence (synchronous). */
export async function orderedFrameScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        // packages/chord/test/replicated-state.test.ts: hydrates at sequence zero when attaching after existing commits
        const initial = Object.freeze({ value: 0 });
        const current = Object.freeze({ value: 1 });
        const source = new TestSource(initial, 40);
        source.commit(current, Object.freeze([["s", ["value"], 1]]));
        const state = replicatedState(source);
        const deliveries: Any[] = [];
        state.subscribe((value: Any, _context: Any, delivery: Any) =>
            deliveries.push({ kind: delivery.kind, sequence: delivery.sequence, value: jsonClone(value) }),
        );
        assert.equal(source.attachments.size, 1);
        assert.equal(source.attachments.values().next().value.activated, true);
        assert.deepEqual(deliveries, [{ kind: "hydrate", sequence: 0, value: { value: 1 } }]);
        assert.deepEqual(state.value, { value: 1 });
        scenarios.push({
            name: "an attached state restores the latest committed revision when subscribing after existing commits",
            source:
                "packages/chord/test/replicated-state.test.ts:hydrates at sequence zero when attaching after existing commits; " +
                "packages/chord/src/state.ts:replicatedState,AttachedReplicatedState,ReplicatedStatePublisher.subscribe",
            inputs: {
                snapshot: { value: 0, cursor: 40 },
                commitsBeforeAttach: [{ value: 1, cursor: 41 }],
            },
            expected: {
                attachmentActivated: true,
                deliveries,
                finalValue: jsonClone(state.value),
            },
        });
    }

    {
        // packages/chord/test/replicated-state.test.ts: captures before activation and drains queued commits in order
        const initial = Object.freeze({ value: 0 });
        const first = Object.freeze({ value: 1 });
        const second = Object.freeze({ value: 2 });
        const source = new TestSource(initial, 10);
        source.onAttach = () => {
            source.commit(first, Object.freeze([["s", ["value"], 1]]));
            source.commit(second, Object.freeze([["s", ["value"], 2]]));
        };
        const state = replicatedState(source);
        const deliveries: Any[] = [];
        state.subscribe((value: Any, _context: Any, delivery: Any) =>
            deliveries.push({ kind: delivery.kind, sequence: delivery.sequence, value: jsonClone(value) }),
        );
        assert.deepEqual(deliveries, [{ kind: "hydrate", sequence: 2, value: { value: 2 } }]);
        assert.deepEqual(state.value, { value: 2 });
        scenarios.push({
            name: "attachment captures the snapshot before activation and drains buffered frames in commit order",
            source:
                "packages/chord/test/replicated-state.test.ts:captures before activation and drains queued commits in order; " +
                "packages/chord/src/state.ts:ReplicatedStateSource.attach,ReplicatedStateSourceAttachment.activate,ReplicatedStatePublisher.publish",
            inputs: {
                snapshot: { value: 0, cursor: 10 },
                commitsDuringAttach: [
                    { value: 1, cursor: 11 },
                    { value: 2, cursor: 12 },
                ],
            },
            expected: {
                deliveries,
                finalValue: jsonClone(state.value),
            },
            normalizations: [
                "A late subscriber converges to the latest committed revision: only one hydrate at the publisher sequence reached after the buffered drain; intermediate frames are observable through a subscriber installed before the commits.",
            ],
        });
    }

    {
        // packages/chord/test/replicated-state.test.ts: publishes exact source values without applying or re-diffing
        const source = new TestSource(Object.freeze({ value: 0 }));
        const state = replicatedState(source);
        const next = Object.freeze({ value: 1 });
        // Deliberately not the operation that produces `next`: Chord publishes the source value as is.
        const ops = Object.freeze([["s", ["value"], 7]]);
        let publishedValue: SourceValue | undefined;
        let publishedDelivery: unknown = undefined;
        state.subscribe((value: Any, _context: Any, delivery: Any) => {
            if (delivery.kind === "update") {
                publishedValue = value;
                publishedDelivery = delivery;
            }
        });
        source.commit(next, ops);
        assert.deepEqual(publishedValue, { value: 1 });
        assert.deepEqual(state.value, { value: 1 });
        scenarios.push({
            name: "exact source values are published without applying or re-diffing the frame operations",
            source:
                "packages/chord/test/replicated-state.test.ts:publishes exact source values without applying or re-diffing; " +
                "packages/chord/src/state.ts:ReplicatedStateSourceFrame,ReplicatedStatePublisher.publish",
            inputs: {
                snapshot: { value: 0, cursor: 0 },
                committed: { value: 1, cursor: 1, ops: [["s", ["value"], 7]] },
            },
            expected: {
                publishedValue: jsonClone(publishedValue),
                publishedDelivery,
                finalValue: jsonClone(state.value),
            },
            normalizations: [
                "The frame ops intentionally disagree with the committed value: an implementation that re-applied or re-diffed them would publish 7 instead of 1.",
            ],
        });
    }

    {
        // packages/chord/src/state.ts AttachedReplicatedStateImpl.#receive: every later frame must be cursor + 1.
        const source = new TestSource(Object.freeze({ value: 1 }), 0);
        const errors: Error[] = [];
        const state = replicatedState(source, { onError: (error: Error) => errors.push(error) });
        const deliveries: number[] = [];
        state.subscribe((_value: Any, _context: Any, delivery: Any) => deliveries.push(delivery.sequence));
        source.commit(Object.freeze({ value: 2 }), Object.freeze([["s", ["value"], 2]]), BACKGROUND_CONTEXT, 1);
        // A repeated (backward) cursor is the same out-of-order contract failure as a skipped one.
        source.commit(Object.freeze({ value: 3 }), Object.freeze([["s", ["value"], 3]]), BACKGROUND_CONTEXT, 1);
        assert.deepEqual(errors.map((error) => error.message), [
            "Replicated state source cursor has a gap: expected 2, received 1",
        ]);
        assert.deepEqual(deliveries, [0, 1]);
        assert.deepEqual(state.value, { value: 2 });
        assert.equal(source.attachments.size, 0);
        source.commit(Object.freeze({ value: 4 }), Object.freeze([["s", ["value"], 4]]), BACKGROUND_CONTEXT, 2);
        assert.deepEqual(state.value, { value: 2 });
        scenarios.push({
            name: "a repeated cursor is reported once as a gap and keeps the last good value",
            source:
                "packages/chord/src/state.ts:AttachedReplicatedStateImpl.#receive,AttachedReplicatedStateImpl.#fail; " +
                "packages/chord/test/replicated-state.test.ts:reports cursor gaps, disposes the broken attachment, and ignores later frames",
            inputs: {
                snapshot: { value: 1, cursor: 0 },
                commits: [
                    { value: 2, cursor: 1 },
                    { value: 3, cursor: 1 },
                    { value: 4, cursor: 2 },
                ],
            },
            expected: {
                reportedErrors: errors.map((error) => ({ name: error.name, message: error.message })),
                deliveredSequences: deliveries,
                lastGoodValue: jsonClone(state.value),
                attachmentCountAfterGap: source.attachments.size,
                lastGoodValueAfterLaterFrame: jsonClone(state.value),
            },
        });
    }

    {
        // packages/chord/test/replicated-state.test.ts: buffers reentrant frames and skips updates covered by a late hydration
        const source = new TestSource(Object.freeze({ value: 0 }));
        const state = replicatedState(source);
        const received: number[] = [];
        const late: Any[] = [];
        let nested = false;
        state.subscribe((_value: Any, _context: Any, delivery: Any) => {
            if (delivery.kind !== "update" || delivery.sequence !== 1 || nested) return;
            nested = true;
            source.commit(Object.freeze({ value: 2 }), Object.freeze([["s", ["value"], 2]]));
            state.subscribe((value: Any, _context: Any, next: Any) => late.push({ ...next, value: value.value }));
        });
        state.subscribe((value: Any, _context: Any, delivery: Any) => {
            if (delivery.kind === "update") received.push(value.value);
        });
        source.commit(Object.freeze({ value: 1 }), Object.freeze([["s", ["value"], 1]]));
        assert.deepEqual(received, [1, 2]);
        assert.deepEqual(late, [{ kind: "hydrate", sequence: 2, value: 2 }]);
        scenarios.push({
            name: "reentrant commits buffer in order and a late subscriber hydrates past them at the latest sequence",
            source:
                "packages/chord/test/replicated-state.test.ts:buffers reentrant frames and skips updates covered by a late hydration; " +
                "packages/chord/src/state.ts:ReplicatedStatePublisher.publish,ReplicatedStatePublisher.subscribe,StateSubscriber.push",
            inputs: {
                snapshot: { value: 0, cursor: 0 },
                frames: [
                    { value: 1, cursor: 1, reentrantCommit: { value: 2, cursor: 2 } },
                    { value: 2, cursor: 2 },
                ],
            },
            expected: {
                earlySubscriberUpdates: received,
                lateSubscriberDeliveries: late,
                finalValue: jsonClone(state.value),
            },
        });
    }

    {
        // packages/chord/test/replicated-state-delivery.test.ts: serializes reentrant hydration and update callbacks
        const source = new TestSource(Object.freeze({ value: 0 }));
        const state = replicatedState(source);
        const events: string[] = [];
        state.subscribe((value: Any) => {
            events.push(`start:${value.value}`);
            if (value.value < 2) commitFrame(source, value.value + 1);
            events.push(`end:${value.value}`);
        });
        assert.deepEqual(events, ["start:0", "end:0", "start:1", "end:1", "start:2", "end:2"]);
        scenarios.push({
            name: "one subscription serializes reentrant hydration and update callbacks in publication order",
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:serializes reentrant hydration and update callbacks; " +
                "packages/chord/src/state.ts:StateSubscriber.drain,ReplicatedStatePublisher.publish",
            inputs: {
                snapshot: { value: 0, cursor: 0 },
                reentrantChainUntil: 2,
            },
            expected: { events },
        });
    }

    {
        // packages/chord/test/replicated-state.test.ts: keeps attachments independent and disposes each idempotently
        const source = new TestSource(Object.freeze({ value: 0 }));
        const first = replicatedState(source);
        const second = replicatedState(source);
        assert.equal(source.attachments.size, 2);
        source.commit(Object.freeze({ value: 1 }), Object.freeze([["s", ["value"], 1]]));
        assert.deepEqual(first.value, { value: 1 });
        assert.deepEqual(second.value, { value: 1 });
        first.dispose();
        first.dispose();
        assert.equal(source.attachments.size, 1);
        source.commit(Object.freeze({ value: 2 }), Object.freeze([["s", ["value"], 2]]));
        assert.deepEqual(first.value, { value: 1 });
        assert.deepEqual(second.value, { value: 2 });
        second.dispose();
        assert.equal(source.attachments.size, 0);
        assert.deepEqual(first.value, { value: 1 });
        scenarios.push({
            name: "independent attachments converge separately and dispose idempotently without affecting each other",
            source:
                "packages/chord/test/replicated-state.test.ts:keeps attachments independent and disposes each idempotently; " +
                "packages/chord/src/state.ts:AttachedReplicatedState.dispose,ReplicatedStateSourceAttachment.dispose",
            inputs: {
                snapshot: { value: 0, cursor: 0 },
                commits: [{ value: 1, cursor: 1 }, { value: 2, cursor: 2 }],
                disposeFirstTwiceBefore: 2,
            },
            expected: {
                attachmentCountAfterAttach: 2,
                attachmentCountAfterFirstDispose: 1,
                attachmentCountAfterSecondDispose: 0,
                firstValueAfterDisposeAndLaterCommit: jsonClone(first.value),
                secondValueAfterLaterCommit: { value: 2 },
            },
        });
    }

    return scenarios;
}

/** Bounded pending delivery, overflow coalescing, failure isolation and unsubscribe. */
export async function observerDeliveryScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    async function boundsPending(count: number): Promise<Scenario> {
        const source = new TestSource(Object.freeze({ value: 0 }));
        const state = replicatedState(source);
        const gate = deferred<void>();
        const received: number[] = [];
        state.subscribe((value: Any) => {
            received.push(value.value);
            if (value.value === 0) return gate.promise;
        });
        for (let value = 1; value <= count; value += 1) commitFrame(source, value);
        gate.resolve();
        await gate.promise;
        await settled(`bounded drain of ${count}`);
        const first = Math.floor((count - 1) / 100) * 100 + 1;
        assert.deepEqual(received, [0, ...Array.from({ length: count - first + 1 }, (_unused, index) => first + index)]);
        return {
            name: `at most 100 deliveries pend behind one held listener and overflow keeps the newest (${count} commits)`,
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:bounds pending deliveries; " +
                "packages/chord/src/state.ts:StateSubscriber.push",
            inputs: { snapshot: { value: 0, cursor: 0 }, heldHydration: true, commits: count, pendingBound: 100 },
            expected: { received },
            normalizations: [
                "Received values after the held listener settles: the hydration value, then only the newest coalesced range; intermediate deliveries are dropped at the 100-pending bound, never reorder",
            ],
        };
    }

    scenarios.push(await boundsPending(100));
    scenarios.push(await boundsPending(102));
    scenarios.push(await boundsPending(202));

    {
        // packages/chord/test/replicated-state-delivery.test.ts: awaits hydration and each update independently of other subscribers
        const source = new TestSource(Object.freeze({ value: 0 }));
        const state = replicatedState(source);
        const hydration = deferred<void>();
        const update = deferred<void>();
        const events: string[] = [];
        const fast: number[] = [];
        state.subscribe(async (value: Any) => {
            events.push(`start:${value.value}`);
            if (value.value === 0) await hydration.promise;
            if (value.value === 1) await update.promise;
            events.push(`end:${value.value}`);
        });
        state.subscribe((value: Any) => fast.push(value.value));
        commitFrame(source, 1);
        commitFrame(source, 2);
        const beforeRelease = { events: [...events], fast: [...fast], value: jsonClone(state.value) };
        assert.deepEqual(beforeRelease, { events: ["start:0"], fast: [0, 1, 2], value: { value: 2 } });
        hydration.resolve();
        await settled();
        const afterHydrationRelease = [...events];
        assert.deepEqual(afterHydrationRelease, ["start:0", "end:0", "start:1"]);
        update.resolve();
        await settled();
        assert.deepEqual(events, ["start:0", "end:0", "start:1", "end:1", "start:2", "end:2"]);
        scenarios.push({
            name: "a held listener serializes its own updates while another subscription converges independently",
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:awaits hydration and each update independently of other subscribers; " +
                "packages/chord/src/state.ts:StateSubscriber.drain,ReplicatedStatePublisher.publish",
            inputs: {
                snapshot: { value: 0, cursor: 0 },
                heldDelivery: { hydration: "value 0", update: "value 1" },
                commits: [{ value: 1, cursor: 1 }, { value: 2, cursor: 2 }],
            },
            expected: {
                heldListenerEvents: events,
                beforeHydrationRelease: beforeRelease,
                afterHydrationRelease,
                otherSubscriberReceived: fast,
            },
            normalizations: [
                "The held listener's post-await body runs only after each release; the other subscription converges synchronously and keeps its own publication order.",
            ],
        });
    }

    {
        // packages/chord/test/replicated-state-delivery.test.ts: excludes a running update from overflow and retains exact value/context/delivery
        const source = new TestSource(Object.freeze({ value: 0 }));
        const state = replicatedState(source);
        const gate = deferred<void>();
        const cancelled = withCancel(BACKGROUND_CONTEXT);
        const received: Any[] = [];
        state.subscribe((value: Any, context: Any, delivery: Any) => {
            received.push({ value: jsonClone(value), signalPresent: context.abortSignal !== undefined, signalAborted: context.abortSignal?.aborted ?? null, delivery: jsonClone(delivery) });
            if (value.value === 1) return gate.promise;
        });
        for (let value = 1; value < 102; value += 1) commitFrame(source, value);
        commitFrame(source, 102, cancelled.context);
        const adopted = jsonClone(state.value);
        commitFrame(source, 103);
        assert.deepEqual(
            received.map((entry: Any) => entry.value.value),
            [0, 1],
        );
        gate.resolve();
        await gate.promise;
        await settled();
        assert.deepEqual(
            received.map((entry: Any) => entry.value.value),
            [0, 1, 102, 103],
        );
        assert.deepEqual(received[2].delivery, { kind: "update", sequence: 102 });
        assert.deepEqual(adopted, { value: 102 });
        scenarios.push({
            name: "overflow keeps only the newest pending delivery with its exact value context and delivery",
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:excludes a running update from overflow and retains exact value/context/delivery; " +
                "packages/chord/src/state.ts:StateSubscriber.push,ReplicatedStateDelivery",
            inputs: {
                snapshot: { value: 0, cursor: 0 },
                heldValue: 1,
                commits: 101,
                newestPending: { value: 102, cursor: 102, context: "withCancel(BACKGROUND_CONTEXT)" },
                trailingCommit: { value: 103, cursor: 103 },
            },
            expected: {
                receivedValues: received.map((entry: Any) => entry.value.value),
                newestDelivery: received[2],
                adoptedValueAtOverflow: adopted,
            },
            normalizations: [
                "`context` identity is pinned by the recorded abort-signal presence: the newest pending delivery carries the exact context object published with frame 102 (a withCancel layer over the background root).",
            ],
        });
    }

    {
        // packages/chord/test/replicated-state-delivery.test.ts: observes hydration rejection, synchronous throw, and update rejection while continuing delivery
        const source = new TestSource(Object.freeze({ value: 0 }));
        const errors: Error[] = [];
        const state = replicatedState(source, { onError: (error: Error) => errors.push(error) });
        const gate = deferred<void>();
        const received: number[] = [];
        const fast: number[] = [];
        state.subscribe((value: Any) => {
            received.push(value.value);
            if (value.value === 0) return gate.promise;
            if (value.value === 1) throw new Error("sync update");
            if (value.value === 2) return Promise.reject(new Error("async update"));
        });
        state.subscribe((value: Any) => fast.push(value.value));
        for (let value = 1; value <= 3; value += 1) commitFrame(source, value);
        gate.reject(new Error("async hydration"));
        await settled();
        assert.deepEqual(received, [0, 1, 2, 3]);
        assert.deepEqual(
            errors.map((error) => error.message),
            ["async hydration", "sync update", "async update"],
        );
        assert.deepEqual(fast, [0, 1, 2, 3]);
        scenarios.push({
            name: "a failing listener is reported through onError in isolation and keeps receiving later deliveries",
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:observes hydration rejection, synchronous throw, and update rejection while continuing delivery; " +
                "packages/chord/test/replicated-state.test.ts:reports listener failures without throwing them into the source; " +
                "packages/chord/src/state.ts:StateSubscriber.drain,AttachedReplicatedStateImpl.#receive",
            inputs: {
                snapshot: { value: 0, cursor: 0 },
                listenerFailures: { 0: "reject:async hydration", 1: "throw:sync update", 2: "reject:async update" },
                commits: [{ value: 1, cursor: 1 }, { value: 2, cursor: 2 }, { value: 3, cursor: 3 }],
            },
            expected: {
                failingListenerReceived: received,
                otherSubscriberReceived: fast,
                reportedErrors: errors.map((error) => ({ name: error.name, message: error.message })),
            },
        });
    }

    {
        // packages/chord/test/replicated-state-delivery.test.ts: isolates a synchronous hydration failure without removing the subscription
        const errors: Error[] = [];
        const receive: Any[] = [];
        const source = {
            attach: () => ({
                snapshot: { value: Object.freeze({ value: 0 }), cursor: 0 },
                activate: (listener: Any) => {
                    receive.push(listener);
                },
                dispose() {},
            }),
        };
        const state = replicatedState(source, { onError: (error: Error) => errors.push(error) });
        const received: number[] = [];
        const failure = new Error("sync hydration");
        state.subscribe((value: Any) => {
            received.push(value.value);
            if (value.value === 0) throw failure;
        });
        receive[0](frame(1, 1));
        assert.deepEqual(errors, [failure]);
        assert.deepEqual(received, [0, 1]);
        scenarios.push({
            name: "a synchronous hydration failure is reported without removing the subscription",
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:isolates a synchronous hydration failure without removing the subscription; " +
                "packages/chord/src/state.ts:ReplicatedStatePublisher.subscribe,StateSubscriber.drain",
            inputs: { snapshot: { value: 0, cursor: 0 }, hydrationFailure: "throw:sync hydration" },
            expected: {
                received,
                reportedErrors: errors.map((error) => ({ name: error.name, message: error.message })),
            },
        });
    }

    {
        // packages/chord/test/replicated-state-delivery.test.ts: unsubscribe drops queued callbacks without joining or aborting the running callback
        const source = new TestSource(Object.freeze({ value: 0 }));
        const state = replicatedState(source);
        const gate = deferred<void>();
        const cancelled = withCancel(BACKGROUND_CONTEXT);
        const received: number[] = [];
        let completed = false;
        const stop = state.subscribe(async (value: Any) => {
            received.push(value.value);
            if (value.value === 1) {
                await gate.promise;
                completed = true;
            }
        });
        await Promise.resolve();
        commitFrame(source, 1, cancelled.context);
        commitFrame(source, 2, cancelled.context);
        stop();
        commitFrame(source, 3, cancelled.context);
        assert.equal(cancelled.context.abortSignal?.aborted, false);
        assert.equal(completed, false);
        gate.resolve();
        await settled();
        assert.equal(completed, true);
        assert.deepEqual(received, [0, 1]);
        scenarios.push({
            name: "unsubscribe discards queued deliveries without aborting or joining the running callback",
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:unsubscribe drops queued callbacks without joining or aborting the running callback; " +
                "packages/chord/src/state.ts:StateSubscriber.close,StateSubscriber.clear",
            inputs: {
                snapshot: { value: 0, cursor: 0 },
                heldValue: 1,
                queuedDuringHold: [{ value: 2, cursor: 2 }],
                commitsAfterUnsubscribe: [{ value: 3, cursor: 3 }],
            },
            expected: {
                received,
                completedAfterRelease: completed,
                aborted: false,
            },
        });
    }

    {
        // packages/chord/test/replicated-state-delivery.test.ts: still observes a callback rejection after unsubscribe
        const source = new TestSource(Object.freeze({ value: 0 }));
        const errors: Error[] = [];
        const state = replicatedState(source, { onError: (error: Error) => errors.push(error) });
        const gate = deferred<void>();
        const received: number[] = [];
        const stop = state.subscribe((value: Any) => {
            received.push(value.value);
            return gate.promise;
        });
        commitFrame(source, 1);
        stop();
        const failure = new Error("stopped callback");
        gate.reject(failure);
        await settled();
        assert.deepEqual(errors, [failure]);
        assert.deepEqual(received, [0]);
        scenarios.push({
            name: "a callback rejection that settles after unsubscribe is still reported",
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:still observes a callback rejection after unsubscribe; " +
                "packages/chord/src/state.ts:StateSubscriber.#report",
            inputs: { snapshot: { value: 0, cursor: 0 }, lateRejection: "stopped callback" },
            expected: {
                received,
                reportedErrors: errors.map((error) => ({ name: error.name, message: error.message })),
            },
        });
    }

    {
        // packages/chord/test/replicated-state-delivery.test.ts: treats two subscriptions of the same callback independently
        const source = new TestSource(Object.freeze({ value: 0 }));
        const state = replicatedState(source);
        const gate = deferred<void>();
        const received: number[] = [];
        const listener = (value: Any) => {
            received.push(value.value);
            if (value.value === 0) return gate.promise;
        };
        const stopFirst = state.subscribe(listener);
        const stopSecond = state.subscribe(listener);
        commitFrame(source, 1);
        stopFirst();
        stopFirst();
        gate.resolve();
        await gate.promise;
        await settled();
        assert.deepEqual(received, [0, 0, 1]);
        stopSecond();
        scenarios.push({
            name: "two subscriptions of the same listener are delivered independently",
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:treats two subscriptions of the same callback independently; " +
                "packages/chord/src/state.ts:ReplicatedStatePublisher.subscribe,StateSubscriber",
            inputs: { snapshot: { value: 0, cursor: 0 }, sameListenerSubscribedTwice: true, firstUnsubscribedTwice: true },
            expected: { received },
        });
    }

    return scenarios;
}
