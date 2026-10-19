/**
 * Public-path observations for the durable-required subset of `@earendil-works/chord` in pi v1.3.0.
 *
 * Every value under `expected` is read from the frozen implementation, never inferred from the
 * TypeScript types. Assertions pin the discriminating facts; the JSON record is the exact value a
 * later C++ test must match after the declared normalizations.
 */

import assert from "node:assert/strict";
import {
    BACKGROUND_CONTEXT,
    MemoryStorage,
    OutputBuffer,
    PROGRESS_BYTES_PER_SECOND,
    apply,
    applyImmutable,
    applyImmutableBatches,
    assertValidOp,
    awaitWithContext,
    boundOutput,
    characterEnd,
    copyJson,
    createContextKey,
    createSession,
    defineDoc,
    isBase,
    isReplace,
    overlap,
    replicatedState,
    track,
    utf8ByteLength,
    withAbortSignal,
    withCancel,
    withContextValue,
    withoutAbortSignal,
    type Scenario,
} from "./pi.mts";

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

async function within<T>(promise: Promise<T>, label: string, ms = 2_000): Promise<T> {
    return Promise.race([
        promise,
        sleep(ms).then(() => {
            throw new Error(`timed out waiting for ${label}`);
        }),
    ]);
}

function jsonClone<T>(value: T): T {
    return JSON.parse(JSON.stringify(value)) as T;
}

function signalState(context: any): { present: boolean; aborted: boolean; reason: string | null } {
    const signal = context.abortSignal;
    if (signal === undefined) return { present: false, aborted: false, reason: null };
    return { present: true, aborted: signal.aborted, reason: signal.aborted ? String(signal.reason) : null };
}

function errorShape(error: unknown): { name: string; message: string; code?: unknown } {
    const value = error as { name?: unknown; message?: unknown; code?: unknown };
    const shape: { name: string; message: string; code?: unknown } = {
        name: String(value?.name ?? typeof error),
        message: String(value?.message ?? error),
    };
    if (value?.code !== undefined) shape.code = value.code;
    return shape;
}

class CountingStorage extends MemoryStorage {
    commits = 0;
    async commit(writes: readonly any[], context: any): Promise<any> {
        this.commits += 1;
        return super.commit(writes, context);
    }
}

class GatedCloseStorage extends MemoryStorage {
    readonly gate = deferred<void>();
    readonly entered = deferred<void>();
    closeStarted = false;
    closeCompleted = false;
    enterClose(): Promise<void> {
        return this.entered.promise;
    }
    async close(context: any): Promise<void> {
        this.closeStarted = true;
        this.entered.resolve();
        await this.gate.promise;
        this.closeCompleted = true;
        return super.close(context);
    }
}

class GatedFindStorage extends MemoryStorage {
    gateArmed = false;
    readonly enter = deferred<void>();
    readonly gate = deferred<void>();
    async findDocument(address: any, at: any, context: any): Promise<any> {
        if (this.gateArmed) {
            this.enter.resolve();
            await this.gate.promise;
        }
        return super.findDocument(address, at, context);
    }
}

function attachedSource(snapshot: { value: any; cursor: number }): {
    source: { attach(): any };
    push(frame: any): void;
    disposed(): number;
} {
    let listener: ((frame: any) => void) | undefined;
    let disposed = 0;
    const buffered: any[] = [];
    const attachment = {
        snapshot,
        activate(next: (frame: any) => void): void {
            listener = next;
            for (const frame of buffered.splice(0)) next(frame);
        },
        dispose(): void {
            disposed += 1;
        },
    };
    return {
        source: { attach: () => attachment },
        push(frame: any): void {
            if (listener !== undefined) listener(frame);
            else buffered.push(frame);
        },
        disposed: () => disposed,
    };
}

/** Document transactions: exact commit sequence, watcher frames, rollback, revision mismatch, storage failure. */
export async function documentScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    scenarios.push(await exactCommittedFrames());
    scenarios.push(await callbackRollback());
    scenarios.push(await newerDefinitionReplacement());
    scenarios.push(await structuralNoOpPublication());
    scenarios.push(await documentStateDelivery());
    scenarios.push(await immutableSnapshots());
    scenarios.push(await retirement());
    scenarios.push(await storageFailure());

    return scenarios;

    async function exactCommittedFrames(): Promise<Scenario> {
        const token = defineDoc({
            kind: "1057.frames",
            scope: "session",
            version: 1,
            initial: () => ({ value: 0, items: ["a"], nested: { count: 0 } }),
        });
        const storage = new CountingStorage();
        const session = createSession(storage);
        let watch: any;
        try {
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 1;
            }, BACKGROUND_CONTEXT);
            watch = await session.watchDoc(token, BACKGROUND_CONTEXT);
            assert.ok(watch);
            const frames: any[] = [];
            const publications: any[] = [];
            session.subscribeCommits((publication: any) => {
                const change = publication.changes.find((entry: any) => entry.type === "document");
                publications.push({
                    seq: Number(publication.seq),
                    version: change?.version ?? null,
                    ops: change === undefined ? [] : jsonClone(change.ops),
                    value: change === undefined ? null : jsonClone(change.value),
                });
            });
            watch.start((value: any, ops: any, context: any) => {
                frames.push({
                    value: jsonClone(value),
                    ops: jsonClone(ops),
                    abortSignal: context.abortSignal ?? null,
                });
            });
            const acquisitionValue = jsonClone(watch.value);
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 2;
            }, BACKGROUND_CONTEXT);
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.items.push("c");
                doc.nested.count = 1;
            }, BACKGROUND_CONTEXT);
            await sleep();
            const finalSnapshot = jsonClone(await session.snapshot(token, BACKGROUND_CONTEXT));
            let replayed: any = acquisitionValue;
            for (const frame of frames) replayed = applyImmutable(replayed, frame.ops);
            assert.equal(frames.length, 2);
            assert.ok(frames.every((frame) => frame.ops.length > 0));
            assert.equal(JSON.stringify(replayed), JSON.stringify(finalSnapshot));
            assert.deepEqual(publications.map((entry: any) => entry.seq), [2, 3]);
            watch.stop();
            return {
                name: "exact committed frames replay from the acquisition revision",
                source:
                    "packages/durable/test/session-watches.test.ts:keeps the acquisition revision until start and delivers exact committed frames; " +
                    "packages/chord/test/delta.test.ts:applies mutable and immutable operations",
                inputs: {
                    document: "1057.frames",
                    initial: { value: 0, items: ["a"], nested: { count: 0 } },
                    watchedAfterCommit: 1,
                    commits: [
                        { mutation: "value = 2" },
                        { mutation: 'items.push("c"); nested.count = 1' },
                    ],
                },
                expected: {
                    acquisitionValue,
                    commitSeqs: publications.map((entry: any) => entry.seq),
                    publications,
                    frames,
                    nonEmptyFrameOps: frames.every((frame) => frame.ops.length > 0),
                    replayMatchesFinal: true,
                    finalSnapshot,
                },
                normalizations: [
                    "`ops` are the exact emitted tuples for this run; a C++ test compares frame order, replay of the recorded tuples onto the acquisition value, and the final value. Chord does not document operation spelling as canonical, so a C++ producer may emit an equivalent batch.",
                ],
            };
        } finally {
            watch.stop();
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }

    async function callbackRollback(): Promise<Scenario> {
        const token = defineDoc({
            kind: "1057.rollback",
            scope: "session",
            version: 1,
            initial: () => ({ value: 1, nested: { count: 0 } }),
        });
        const storage = new CountingStorage();
        const session = createSession(storage);
        await session.commit(async (tx: any) => {
            const doc = await tx.doc(token);
            doc.value = 1;
        }, BACKGROUND_CONTEXT);
        const watch = await session.watchDoc(token, BACKGROUND_CONTEXT);
        assert.ok(watch);
        const frames: any[] = [];
        const publications: any[] = [];
        session.subscribeCommits((publication: any) => publications.push(Number(publication.seq)));
        watch.start((value: any, ops: any) => frames.push({ value: jsonClone(value), ops: jsonClone(ops) }));
        const before = jsonClone(watch.value);
        try {
            let rollbackError: any;
            try {
                await session.commit(async (tx: any) => {
                    const doc = await tx.doc(token);
                    doc.value = 99;
                    doc.nested.count = 5;
                    throw new Error("rollback refused");
                }, BACKGROUND_CONTEXT);
            } catch (error) {
                rollbackError = errorShape(error);
            }
            await sleep();
            const afterRollback = {
                watchValue: jsonClone(watch.value),
                snapshot: jsonClone(await session.snapshot(token, BACKGROUND_CONTEXT)),
                publications: [...publications],
                frames: [...frames],
                storageCommits: storage.commits,
                rollbackError,
            };
            assert.deepEqual(afterRollback.watchValue, before);
            assert.deepEqual(afterRollback.snapshot, before);
            assert.deepEqual(afterRollback.publications, []);
            assert.equal(afterRollback.frames.length, 0);
            assert.equal(afterRollback.storageCommits, 1);
            assert.equal(afterRollback.rollbackError?.message, "rollback refused");

            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 2;
            }, BACKGROUND_CONTEXT);
            await sleep();
            assert.deepEqual(publications, [2]);
            assert.equal(frames.length, 1);
            return {
                name: "a callback rollback publishes nothing and keeps the committed revision",
                source:
                    "packages/durable/test/session-documents.test.ts:Session document transactions; " +
                    "packages/chord/test/delta.test.ts:aborts and revokes without changing the committed value",
                inputs: {
                    document: "1057.rollback",
                    committedOne: 1,
                    failedMutation: "value = 99; nested.count = 5; throw new Error('rollback refused')",
                    laterMutation: "value = 2",
                },
                expected: {
                    beforeRollback: before,
                    afterRollback,
                    afterLaterCommit: {
                        publications: [...publications],
                        frameValues: frames.map((frame) => frame.value),
                        snapshot: jsonClone(await session.snapshot(token, BACKGROUND_CONTEXT)),
                        storageCommits: storage.commits,
                    },
                },
            };
        } finally {
            watch.stop();
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }

    async function newerDefinitionReplacement(): Promise<Scenario> {
        const tokenV1 = defineDoc({
            kind: "1057.versioned",
            scope: "session",
            version: 1,
            initial: () => ({ value: 1 }),
        });
        const tokenV2 = defineDoc({
            kind: "1057.versioned",
            scope: "session",
            version: 2,
            initial: () => ({ value: 2, extra: true }),
            migrate: () => ({ value: 2, extra: true }),
        });
        const storage = new MemoryStorage();
        const session = createSession(storage);
        try {
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(tokenV1);
                doc.value = 1;
            }, BACKGROUND_CONTEXT);
            const watch = await session.watchDoc(tokenV1, BACKGROUND_CONTEXT);
            assert.ok(watch);
            const frames: any[] = [];
            const publications: any[] = [];
            session.subscribeCommits((publication: any) => {
                const change = publication.changes.find((entry: any) => entry.type === "document");
                publications.push({
                    seq: Number(publication.seq),
                    version: change?.version ?? null,
                    ops: change === undefined ? [] : jsonClone(change.ops),
                });
            });
            watch.start((value: any, ops: any) => frames.push({
                value: jsonClone(value),
                ops: jsonClone(ops),
            }));
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(tokenV2);
                doc.value = 2;
            }, BACKGROUND_CONTEXT);
            await sleep();
            const snapshotV2 = jsonClone(await session.snapshot(tokenV2, BACKGROUND_CONTEXT));
            let readError: any;
            try {
                await session.snapshot(tokenV1, BACKGROUND_CONTEXT);
            } catch (error) {
                readError = errorShape(error);
            }
            assert.equal(frames.length, 1);
            assert.deepEqual(frames[0].ops[0][0], "r");
            assert.equal(publications[0].version, 2);
            assert.equal(readError?.name, "Error");
            assert.match(readError?.message ?? "", /has newer version 2 than 1/);
            return {
                name: "a watcher acquired at an older definition receives a root replacement on a newer-definition commit",
                source:
                    "packages/durable/test/session-documents.test.ts:Session document transactions; " +
                    "packages/durable/test/session-checkpoints-migrations.test.ts:Session document checkpoints; Session document migrations; " +
                    "packages/chord/test/delta.test.ts:supports root replacement, root splice, and permutations",
                inputs: {
                    document: "1057.versioned",
                    watchedVersion: 1,
                    commitVersion: 2,
                    committedValue: { value: 2, extra: true },
                },
                expected: {
                    publication: publications[0],
                    frame: frames[0],
                    frameIsRootReplacement: frames[0].ops[0][0] === "r",
                    snapshotV2,
                    olderVersionRead: readError,
                },
                normalizations: [
                    "`ops` are exact for this run; the C++ test may compare only that the frame starts with a root replacement and that applying it yields the recorded value.",
                ],
            };
        } finally {
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }

    async function structuralNoOpPublication(): Promise<Scenario> {
        const token = defineDoc({
            kind: "1057.noop",
            scope: "session",
            version: 1,
            initial: () => ({ items: ["a"] }),
        });
        const session = createSession(new MemoryStorage());
        await session.commit(async (tx: any) => {
            const doc = await tx.doc(token);
            doc.value = 0;
        }, BACKGROUND_CONTEXT);
        const watch = await session.watchDoc(token, BACKGROUND_CONTEXT);
        assert.ok(watch);
        const frames: any[] = [];
        const publications: any[] = [];
        session.subscribeCommits((publication: any) => publications.push(Number(publication.seq)));
        watch.start((value: any, ops: any, context: any) => frames.push({
            value: jsonClone(value),
            ops: jsonClone(ops),
            abortSignal: context.abortSignal ?? null,
        }));
        try {
            const before = jsonClone(watch.value);
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.items.shift();
                doc.items.unshift("a");
            }, BACKGROUND_CONTEXT);
            await sleep();
            const after = jsonClone(watch.value);
            assert.deepEqual(after, before);
            assert.equal(frames.length, 1);
            assert.ok(frames[0].ops.length > 0);
            assert.deepEqual(publications, [2]);
            assert.equal(frames[0].abortSignal, null);
            return {
                name: "a structurally rewritten but value-identical commit still publishes a nonempty exact batch",
                source:
                    "packages/chord/test/delta.test.ts:normalizes a deep no-op to exact previous identity; " +
                    "packages/durable/test/session-watches.test.ts:Session document watches",
                inputs: { document: "1057.noop", mutation: 'items.shift(); items.unshift("a")' },
                expected: {
                    before,
                    after,
                    operations: frames[0].ops,
                    operationsNonempty: frames[0].ops.length > 0,
                    publications,
                    deliveryAbortSignal: frames[0].abortSignal,
                },
                normalizations: [
                    "`operations` are exact for this run; a C++ test may compare `operationsNonempty`, publication order, and the unchanged value.",
                ],
            };
        } finally {
            watch.stop();
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }

    async function documentStateDelivery(): Promise<Scenario> {
        const token = defineDoc({
            kind: "1057.state",
            scope: "session",
            version: 1,
            initial: () => ({ value: 0 }),
        });
        const session = createSession(new MemoryStorage());
        await session.commit(async (tx: any) => {
            const doc = await tx.doc(token);
            doc.value = 0;
        }, BACKGROUND_CONTEXT);
        const key = createContextKey<string>("audit-1057-state");
        const state = await session.documentState(token, BACKGROUND_CONTEXT);
        assert.ok(state);
        const deliveries: any[] = [];
        state.subscribe((value: any, context: any, delivery: any) => {
            deliveries.push({
                kind: delivery.kind,
                sequence: delivery.sequence,
                value: jsonClone(value),
                abortSignal: context.abortSignal ?? null,
                contextValue: context.value(key) ?? null,
            });
        });
        try {
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 3;
            }, withContextValue(key, "commit", BACKGROUND_CONTEXT));
            await sleep();
            assert.deepEqual(deliveries.map((delivery) => [delivery.kind, delivery.sequence]), [
                ["hydrate", 0],
                ["update", 1],
            ]);
            assert.equal(deliveries[0].value.value, 0);
            assert.equal(deliveries[1].value.value, 3);
            assert.equal(deliveries[1].contextValue, "commit");
            assert.equal(deliveries[1].abortSignal, null);
            state.dispose();
            return {
                name: "document state hydrates from the source snapshot and then delivers the committed update with caller values",
                source:
                    "packages/durable/test/session-watches.test.ts:preserves commit Context values without inheriting producer cancellation; " +
                    "packages/durable/src/session/session.ts:SessionImpl.documentState; " +
                    "packages/chord/test/state.test.ts:publishes snapshots then exact ops; derivations follow",
                inputs: { document: "1057.state", commitValue: 3, contextValue: "commit" },
                expected: { deliveries },
            };
        } finally {
            state.dispose();
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }


    async function immutableSnapshots(): Promise<Scenario> {
        const token = defineDoc({
            kind: "1057.snapshots",
            scope: "session",
            version: 1,
            initial: () => ({ value: 0, nested: { count: 0 } }),
        });
        const session = createSession(new MemoryStorage());
        try {
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 1;
            }, BACKGROUND_CONTEXT);
            const first = await session.snapshot(token, BACKGROUND_CONTEXT);
            assert.ok(first);
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 2;
                doc.nested.count = 1;
            }, BACKGROUND_CONTEXT);
            const second = await session.snapshot(token, BACKGROUND_CONTEXT);
            assert.ok(second);
            const firstAfter = jsonClone(first);
            const secondAfter = jsonClone(second);
            assert.deepEqual(firstAfter, { value: 1, nested: { count: 0 } });
            assert.deepEqual(secondAfter, { value: 2, nested: { count: 1 } });
            return {
                name: "committed snapshots keep earlier revisions immutable while a later commit advances the document",
                source:
                    "packages/durable/test/session-documents.test.ts:returns shared immutable snapshots and keeps prior revisions stable; " +
                    "packages/durable/test/session-watches.test.ts:keeps earlier immutable revisions stable",
                inputs: { document: "1057.snapshots", firstValue: 1, secondValue: 2 },
                expected: {
                    first: firstAfter,
                    second: secondAfter,
                    firstAfterSecondCommit: jsonClone(first),
                    firstIsSecond: first === second,
                },
            };
        } finally {
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }

    async function retirement(): Promise<Scenario> {
        const token = defineDoc({
            kind: "1057.retirement",
            scope: "session",
            version: 1,
            initial: () => ({ value: 1 }),
        });
        const session = createSession(new MemoryStorage());
        let watch: any;
        try {
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 1;
            }, BACKGROUND_CONTEXT);
            watch = await session.watchDoc(token, BACKGROUND_CONTEXT);
            assert.ok(watch);
            const frames: any[] = [];
            const publications: any[] = [];
            session.subscribeCommits((publication: any) => {
                const change = publication.changes.find((entry: any) => entry.type === "document");
                publications.push({
                    seq: Number(publication.seq),
                    version: change?.version ?? null,
                    ops: change === undefined ? [] : jsonClone(change.ops),
                });
            });
            watch.start((value: any, ops: any) => frames.push({ value: jsonClone(value), ops: jsonClone(ops) }));
            await session.commit(async (tx: any) => {
                await tx.retireDoc(token);
            }, BACKGROUND_CONTEXT);
            const end = await within(watch.closed, "retired watch close");
            const snapshot = await session.snapshot(token, BACKGROUND_CONTEXT);
            assert.equal(frames.length, 1);
            assert.equal(frames[0].value, null);
            assert.deepEqual(frames[0].ops, [["r", null]]);
            assert.equal(snapshot, undefined);
            return {
                name: "retiring a document publishes the canonical root-null batch and closes its watcher",
                source:
                    "packages/durable/src/session/observation.ts:RETIREMENT_OPERATIONS,CommittedWatch; " +
                    "packages/durable/test/session-watches.test.ts:delivers retirement and does not follow recreation; " +
                    "packages/chord/test/delta.test.ts:supports root replacement, root splice, and permutations",
                inputs: { document: "1057.retirement", retire: "tx.retireDoc(token)" },
                expected: {
                    publications,
                    frames,
                    watchEnd: jsonClone(end),
                    snapshotAfterRetirement: snapshot ?? null,
                },
            };
        } finally {
            watch?.stop();
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }

    async function storageFailure(): Promise<Scenario> {
        const token = defineDoc({
            kind: "1057.storage-failure",
            scope: "session",
            version: 1,
            initial: () => ({ value: 0 }),
        });
        const storage = new CountingStorage();
        const session = createSession(storage);
        try {
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 1;
            }, BACKGROUND_CONTEXT);
            const frames: any[] = [];
            const publications: number[] = [];
            const watch = await session.watchDoc(token, BACKGROUND_CONTEXT);
            assert.ok(watch);
            session.subscribeCommits((publication: any) => publications.push(Number(publication.seq)));
            watch.start((value: any) => frames.push(jsonClone(value)));
            const originalCommit = storage.commit.bind(storage);
            storage.commit = async () => {
                throw new Error("disk vanished");
            };
            let commitError: any;
            try {
                await session.commit(async (tx: any) => {
                    const doc = await tx.doc(token);
                    doc.value = 2;
                }, BACKGROUND_CONTEXT);
            } catch (error) {
                commitError = errorShape(error);
            }
            await sleep();
            storage.commit = originalCommit;
            const end = await within(session.closed, "failed Session close");
            let snapshotError: any;
            try {
                await session.snapshot(token, BACKGROUND_CONTEXT);
            } catch (error) {
                snapshotError = errorShape(error);
            }
            assert.deepEqual(publications, []);
            assert.equal(frames.length, 0);
            assert.equal((end as any).reason, "failed");
            assert.match(snapshotError?.message ?? "", /Session failed after a storage error/);
            return {
                name: "a storage failure publishes nothing and ends the Session without a partial commit",
                source:
                    "packages/durable/test/session-documents.test.ts:Session document transactions; " +
                    "packages/durable/src/session/session.ts:SessionImpl.#runCommit",
                inputs: { document: "1057.storage-failure", failingStorageMessage: "disk vanished" },
                expected: {
                    publications,
                    frames,
                    storageCommits: storage.commits,
                    commitError,
                    sessionEndReason: (end as any).reason,
                    snapshotError,
                },
            };
        } finally {
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }
}

/** Context propagation, detached cleanup, wait cancellation, and cancellation of observation/transaction waits. */
export async function contextScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    const key = createContextKey<string>("audit-1057-context");

    {
        const root = withContextValue(key, "root", BACKGROUND_CONTEXT);
        const rootSignal = signalState(root);
        const child = withCancel(root);
        const sibling = withCancel(root);
        const seen: any[] = [];
        const childAbort = new Promise<void>((resolve) => child.context.abortSignal!.addEventListener("abort", () => resolve(), { once: true }));
        child.cancel("child-only");
        await childAbort;
        seen.push({ observer: "child", aborted: child.context.abortSignal!.aborted, reason: String(child.context.abortSignal!.reason), rootAborted: rootSignal.aborted });
        seen.push({ observer: "sibling", aborted: sibling.context.abortSignal!.aborted, rootAborted: rootSignal.aborted });
        sibling.cancel("sibling-only");
        seen.push({ observer: "sibling-after-cancel", aborted: sibling.context.abortSignal!.aborted, reason: String(sibling.context.abortSignal!.reason) });
        const parent = withCancel(BACKGROUND_CONTEXT);
        const inheritor = withAbortSignal(parent.context.abortSignal!, withContextValue(key, "child", BACKGROUND_CONTEXT));
        parent.cancel("parent-reason");
        await sleep();
        assert.equal(child.context.abortSignal!.reason, "child-only");
        assert.equal(rootSignal.aborted, false);
        assert.equal(inheritor.abortSignal!.aborted, true);
        assert.equal(inheritor.abortSignal!.reason, "parent-reason");
        assert.equal(inheritor.value(key), "child");
        scenarios.push({
            name: "a child cancellation is isolated while parent cancellation reaches an already-created child context",
            source:
                "packages/chord/test/context.test.ts:inherits parent cancellation and isolates child cancellation; " +
                "packages/chord/src/context/index.ts:withCancel,withAbortSignal",
            inputs: { rootValue: "root", childReason: "child-only", parentReason: "parent-reason" },
            expected: {
                root: { value: root.value, ...rootSignal },
                child: { value: child.context.value(key), ...signalState(child.context) },
                sibling: { value: sibling.context.value(key), ...signalState(sibling.context) },
                inheritor: { value: inheritor.value(key), ...signalState(inheritor) },
                observations: seen,
            },
        });
    }

    {
        const masked = withoutAbortSignal(withContextValue(key, "cleanup", withCancel(BACKGROUND_CONTEXT).context));
        const visible = withAbortSignal(new AbortController().signal, BACKGROUND_CONTEXT);
        scenarios.push({
            name: "withoutAbortSignal removes cancellation while preserving typed values",
            source:
                "packages/chord/test/context.test.ts:masks caller cancellation for mandatory cleanup; " +
                "packages/chord/src/context/index.ts:withoutAbortSignal",
            inputs: { value: "cleanup" },
            expected: {
                masked: { value: masked.value(key), ...signalState(masked) },
                plainBackground: signalState(BACKGROUND_CONTEXT),
                withUndefinedSignal: signalState(visible),
            },
        });
    }

    {
        const controller = new AbortController();
        const workGate = deferred<string>();
        const work = workGate.promise;
        const waiting = awaitWithContext(work, withAbortSignal(controller.signal, BACKGROUND_CONTEXT));
        controller.abort("stop waiting");
        let waitError: any;
        try {
            await waiting;
        } catch (error) {
            waitError = errorShape(error);
        }
        workGate.resolve("work finished");
        const workResult = await work;
        scenarios.push({
            name: "a context wait rejects with the caller abort reason while the underlying work still completes",
            source:
                "packages/chord/test/context.test.ts:stops waiting when the invocation is cancelled; " +
                "packages/chord/src/context/index.ts:awaitWithContext",
            inputs: { abortReason: "stop waiting", workResult: "work finished" },
            expected: { waitError, workResult, workResolvedAfterWaitRejected: true },
        });
    }

    {
        const token = defineDoc({
            kind: "1057.watch-cancel",
            scope: "session",
            version: 1,
            initial: () => ({ value: 0 }),
        });
        const session = createSession(new MemoryStorage());
        await session.commit(async (tx: any) => {
            const doc = await tx.doc(token);
            doc.value = 0;
        }, BACKGROUND_CONTEXT);
        const scope = withCancel(BACKGROUND_CONTEXT);
        const keyForCommit = createContextKey<string>("audit-1057-watch-commit");
        const watch = await session.watchDoc(token, scope.context);
        assert.ok(watch);
        const frames: any[] = [];
        watch.start((value: any, _ops: any, context: any) => frames.push({
            value: jsonClone(value),
            abortSignal: context.abortSignal ?? null,
            contextValue: context.value(keyForCommit) ?? null,
        }));
        try {
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 2;
            }, withContextValue(keyForCommit, "commit-context", BACKGROUND_CONTEXT));
            await sleep();
            scope.cancel("stop watching");
            await sleep();
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 3;
            }, BACKGROUND_CONTEXT);
            await sleep();
            assert.equal(frames.length, 1);
            assert.equal(frames[0].abortSignal, null);
            assert.equal(frames[0].contextValue, "commit-context");
            const watchEnd = await watch.closed;
            assert.deepEqual(watchEnd, { reason: "cancelled" });
            assert.deepEqual(jsonClone(watch.value), { value: 2 });
            scenarios.push({
                name: "cancelling a watch ends delivery without aborting a completed callback or later commits",
                source:
                    "packages/durable/test/session-watches.test.ts:Session document watches; " +
                    "packages/chord/test/replicated-state-delivery.test.ts:unsubscribe drops queued callbacks without joining or aborting the running callback",
                inputs: { document: "1057.watch-cancel", committedValues: [2, 3], cancelReason: "stop watching" },
                expected: {
                    frames,
                    closed: jsonClone(watchEnd),
                    valueAfterCancel: jsonClone(watch.value),
                    snapshotAfterCancel: jsonClone(await session.snapshot(token, BACKGROUND_CONTEXT)),
                },
            });
        } finally {
            watch.stop();
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }

    {
        const token = defineDoc({
            kind: "1057.close-cancel",
            scope: "session",
            version: 1,
            initial: () => ({ value: 0 }),
        });
        const storage = new GatedCloseStorage();
        const session = createSession(storage);
        await session.commit(async (tx: any) => {
            const doc = await tx.doc(token);
            doc.value = 1;
        }, BACKGROUND_CONTEXT);
        const closeScope = withCancel(BACKGROUND_CONTEXT);
        const closing = session.close(closeScope.context);
        await within(storage.enterClose(), "close entry");
        closeScope.cancel("stop close");
        storage.gate.resolve();
        let closeError: any;
        try {
            await closing;
        } catch (error) {
            closeError = errorShape(error);
        }
        assert.equal(storage.closeStarted, true);
        assert.equal(storage.closeCompleted, true);
        assert.equal(closeError?.name, "AbortError");
        assert.match(closeError?.message ?? "", /aborted/);
        assert.deepEqual(await session.closed, { reason: "closed" });
        scenarios.push({
            name: "cancelling the close caller rejects close while storage cleanup still completes",
            source:
                "packages/chord/test/context.test.ts:masks caller cancellation for mandatory cleanup; " +
                "packages/durable/src/session/session.ts:SessionImpl.close",
            inputs: { document: "1057.close-cancel", cancelReason: "stop close" },
            expected: {
                closeError,
                storageCloseStarted: storage.closeStarted,
                storageCloseCompleted: storage.closeCompleted,
                sessionEnd: jsonClone(await session.closed),
            },
        });
    }

    {
        const token = defineDoc({
            kind: "1057.acquire-cancel",
            scope: "session",
            version: 1,
            initial: () => ({ value: 0 }),
        });
        const storage = new GatedFindStorage();
        const writer = createSession(storage);
        await writer.commit(async (tx: any) => {
            const doc = await tx.doc(token);
            doc.value = 1;
        }, BACKGROUND_CONTEXT);
        const session = createSession(storage);
        storage.gateArmed = true;
        const acquireScope = withCancel(BACKGROUND_CONTEXT);
        const acquisition = session.watchDoc(token, acquireScope.context);
        await within(storage.enter.promise, "watch acquisition");
        acquireScope.cancel("cancel acquisition");
        storage.gate.resolve();
        let acquisitionError: any;
        try {
            await acquisition;
        } catch (error) {
            acquisitionError = errorShape(error);
        }
        assert.equal(acquisitionError?.name, "AbortError");
        assert.match(acquisitionError?.message ?? "", /aborted/);
        assert.deepEqual(jsonClone(await session.snapshot(token, BACKGROUND_CONTEXT)), { value: 1 });
        scenarios.push({
            name: "cancelling a watch acquisition rejects the wait and leaves no write or partial observer",
            source:
                "packages/durable/test/session-watches.test.ts:Session document watches; " +
                "packages/chord/src/context/index.ts:awaitWithContext",
            inputs: { document: "1057.acquire-cancel", cancelReason: "cancel acquisition" },
            expected: {
                acquisitionError,
                snapshotAfterCancellation: jsonClone(await session.snapshot(token, BACKGROUND_CONTEXT)),
            },
        });
        await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        await writer.close(BACKGROUND_CONTEXT).catch(() => undefined);
    }

    return scenarios;
}

/** Delta tracking, immutable/mutable application, validation, and the exact operation vocabulary. */
export async function deltaScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        const tracker = track({ value: 1, items: ["a"] });
        const first = tracker.beginChange();
        first.state.value = 2;
        const preparedFirst = first.prepare();
        const second = tracker.beginChange();
        second.state.value = 3;
        const preparedSecond = second.prepare();
        const before = jsonClone(tracker.value);
        tracker.adopt(preparedFirst);
        const afterFirst = jsonClone(tracker.value);
        let staleError: any;
        try {
            tracker.adopt(preparedSecond);
        } catch (error) {
            staleError = errorShape(error);
        }
        const afterStale = jsonClone(tracker.value);
        assert.equal(tracker.revision, 1);
        assert.deepEqual(afterStale, afterFirst);
        assert.equal(staleError?.message, "Prepared change is stale");
        scenarios.push({
            name: "prepared revisions are immutable and a same-base preparation becomes stale after adoption",
            source:
                "packages/chord/test/delta.test.ts:rejects foreign, stale, and repeated preparations while allowing competing changes; " +
                "packages/chord/src/delta/tracker.ts:Tracker.adopt",
            inputs: { initial: { value: 1, items: ["a"] }, firstValue: 2, secondValue: 3 },
            expected: {
                before,
                firstOps: jsonClone(preparedFirst.ops),
                revisionAfterFirst: 1,
                afterFirst,
                staleError,
                valueAfterStale: afterStale,
                revisionAfterStale: tracker.revision,
            },
            normalizations: [
                "`firstOps` are exact for this run; the C++ comparison may use the recorded value and revision fields plus the exact stale error.",
            ],
        });
    }

    {
        const tracker = track({ value: 1 });
        const change = tracker.beginChange();
        change.state.value = 9;
        const escaped = change.state;
        const prepared = change.prepare();
        change.abort();
        let escapedError: any;
        try {
            escaped.value = 10;
        } catch (error) {
            escapedError = errorShape(error);
        }
        let adoptError: any;
        try {
            tracker.adopt(prepared);
        } catch (error) {
            adoptError = errorShape(error);
        }
        assert.equal(escapedError?.name, "TypeError");
        assert.match(escapedError?.message ?? "", /settled overlay/);
        assert.deepEqual(tracker.value, { value: 1 });
        assert.equal(tracker.revision, 0);
        scenarios.push({
            name: "aborting a changed draft revokes escaped handles and leaves the committed revision unchanged",
            source:
                "packages/chord/test/delta.test.ts:invalidates a prepared result when its change is aborted; " +
                "packages/chord/src/delta/tracker.ts:Change.abort",
            inputs: { initial: { value: 1 }, draftedValue: 9, escapedWrite: 10 },
            expected: {
                escapedError,
                adoptError,
                value: jsonClone(tracker.value),
                revision: tracker.revision,
            },
        });
    }

    {
        const tracker = track({ items: ["a"] });
        const change = tracker.beginChange();
        change.state.items.shift();
        change.state.items.unshift("a");
        const prepared = change.prepare();
        const before = jsonClone(tracker.value);
        tracker.adopt(prepared);
        const emptyTracker = track({ value: 1 });
        const emptyChange = emptyTracker.beginChange();
        const emptyPrepared = emptyChange.prepare();
        emptyTracker.adopt(emptyPrepared);
        assert.ok(prepared.ops.length > 0);
        assert.deepEqual(emptyPrepared.ops, []);
        assert.equal(emptyTracker.revision, 1);
        assert.equal(tracker.revision, 1);
        scenarios.push({
            name: "a structural no-op emits a nonempty batch while an untouched change emits none and still adopts",
            source:
                "packages/chord/test/delta.test.ts:normalizes a deep no-op to exact previous identity; " +
                "packages/chord/src/delta/tracker.ts:Tracker.prepare",
            inputs: {
                structural: { initial: { items: ["a"] }, mutation: 'items.shift(); items.unshift("a")' },
                empty: { initial: { value: 1 } },
            },
            expected: {
                structural: { before, ops: jsonClone(prepared.ops), after: jsonClone(tracker.value), revision: tracker.revision },
                empty: { ops: jsonClone(emptyPrepared.ops), value: jsonClone(emptyTracker.value), revision: emptyTracker.revision },
            },
            normalizations: [
                "`ops` are exact emitted tuples for this run; a C++ producer may emit an equivalent batch, but it must be nonempty for the structural no-op and empty for the untouched change.",
            ],
        });
    }

    {
        const base: any = {
            value: 1,
            nested: { count: 0, retained: { deep: true } },
            retained: { list: [1, 2] },
            items: ["a"],
        };
        const baseBefore = jsonClone(base);
        const ops = [
            ["s", ["value"], 2],
            ["s", ["nested", "count"], 1],
            ["p", ["items"], 1, 0, ["b"]],
        ];
        const next = applyImmutable(base, ops as any);
        const nextAgain = applyImmutable(next, [["s", ["value"], 3]] as any);
        assert.deepEqual(base, baseBefore);
        assert.notEqual(next, base);
        assert.equal(next.retained, base.retained);
        assert.equal(next.nested.retained, base.nested.retained);
        assert.equal(next.value, 2);
        assert.deepEqual(next.items, ["a", "b"]);
        assert.equal(nextAgain.value, 3);
        assert.equal(nextAgain.retained, base.retained);
        scenarios.push({
            name: "immutable application reproduces each revision without mutating the base and shares untouched subtrees",
            source:
                "packages/chord/test/delta-apply-immutable.test.ts:copies each touched container once while preserving input and payload ownership; " +
                "packages/chord/src/delta/index.ts:applyImmutable",
            inputs: { base: baseBefore, ops },
            expected: {
                baseAfter: jsonClone(base),
                next,
                nextAgain,
                touchedRootIsNew: next !== base,
                retainedRootShared: next.retained === base.retained,
                retainedNestedShared: next.nested.retained === base.nested.retained,
                nextAgainRetainedShared: nextAgain.retained === base.retained,
            },
        });
    }

    {
        const base = { items: ["a"], value: 0 };
        const batches = [
            [["s", ["value"], 1], ["p", ["items"], 1, 0, ["b"]]],
            [["s", ["value"], 2], ["p", ["items"], 0, 1, []]],
        ];
        const final = applyImmutableBatches(base, batches as any);
        const emptyTarget = applyImmutableBatches(undefined, [[["r", { created: true }]]] as any);
        assert.deepEqual(base, { items: ["a"], value: 0 });
        assert.deepEqual(final, { items: ["b"], value: 2 });
        assert.deepEqual(emptyTarget, { created: true });
        scenarios.push({
            name: "batched immutable application applies ordered batches and returns only the final revision",
            source:
                "packages/chord/test/delta-apply-immutable.test.ts:shares one private copy-on-write scope across batch partitions; " +
                "packages/chord/src/delta/index.ts:applyImmutableBatches",
            inputs: { base, batches, createdBase: null },
            expected: { baseAfter: { items: ["a"], value: 0 }, final, emptyTarget },
        });
    }

    {
        const target = { items: ["a"], value: 0 };
        const result = apply(target, [["s", ["value"], 1], ["p", ["items"], 1, 0, ["b"]]] as any);
        const replaced = apply(target, [["r", { replacement: true }]] as any);
        assert.equal(result, target);
        assert.deepEqual(target, { items: ["a", "b"], value: 1 });
        assert.notEqual(replaced, target);
        assert.deepEqual(replaced, { replacement: true });
        assert.equal(isBase([["r", { x: 1 }]] as any), true);
        assert.equal(isReplace(["r", { x: 1 }]), true);
        scenarios.push({
            name: "mutable application mutates in place except for a root replacement, which returns a new value",
            source:
                "packages/chord/test/delta.test.ts:applies mutable and immutable operations; " +
                "packages/chord/src/delta/index.ts:apply,isBase,isReplace",
            inputs: { target, ops: [["s", ["value"], 1], ["p", ["items"], 1, 0, ["b"]]], replacement: [["r", { replacement: true }]] },
            expected: {
                sameTarget: result === target,
                targetAfter: jsonClone(target),
                replacementIsNew: replaced !== target,
                replacement: jsonClone(replaced),
                isBaseRoot: isBase([["r", { x: 1 }]] as any),
                isReplaceRoot: isReplace(["r", { x: 1 }]),
            },
        });
    }

    {
        let unsafeError: any;
        try {
            applyImmutable({}, [["s", ["__proto__", "polluted"], true]] as any);
        } catch (error) {
            unsafeError = errorShape(error);
        }
        let malformedError: any;
        try {
            assertValidOp(["x", ["value"]]);
        } catch (error) {
            malformedError = errorShape(error);
        }
        let spliceError: any;
        try {
            applyImmutable([], [["p", [], "0", 0, ["x"]]] as any);
        } catch (error) {
            spliceError = errorShape(error);
        }
        assert.equal((unsafeError as any)?.name, "UnsafePathError");
        assert.equal((malformedError as any)?.name, "TypeError");
        scenarios.push({
            name: "unsafe prototype segments and malformed operation tuples are rejected before mutation",
            source:
                "packages/chord/test/delta.test.ts:rejects unsafe and malformed paths; " +
                "packages/chord/test/delta.test.ts:rejects unknown verbs, malformed tuples, paths, and splice payloads; " +
                "packages/chord/src/delta/index.ts:assertSafePath,assertValidOp",
            inputs: {
                unsafe: [["s", ["__proto__", "polluted"], true]],
                unknownVerb: ["x", ["value"]],
                stringSpliceIndex: [["p", [], "0", 0, ["x"]]],
            },
            expected: { unsafeError, malformedError, spliceError },
        });
    }

    {
        const cases: Record<string, unknown> = {
            set: { base: { o: { a: 1 } }, mutate: (state: any) => { state.o.a = 2; } },
            append: { base: { s: "abc" }, mutate: (state: any) => { state.s += "d"; } },
            splice: { base: { a: ["a", "b", "c"] }, mutate: (state: any) => { state.a.splice(1, 1, "X", "Y"); } },
            permutation: { base: { a: ["a", "b", "c"] }, mutate: (state: any) => { state.a.reverse(); } },
            delete: { base: { o: { a: 1, b: 2 } }, mutate: (state: any) => { delete state.o.b; } },
            truncate: { base: { s: "abcdef" }, mutate: (state: any) => { state.s = state.s.slice(3); } },
            replace: { base: { o: { a: 1 } }, mutate: (state: any) => { state.o = { b: 2 }; } },
            overlap: { base: { a: 1 }, mutate: () => undefined },
        };
        const observations: Record<string, any> = {};
        for (const [name, entry] of Object.entries(cases)) {
            if (name === "overlap") continue;
            const { base, mutate } = entry as { base: any; mutate: (state: any) => void };
            const tracker = track(jsonClone(base));
            const change = tracker.beginChange();
            mutate(change.state);
            const prepared = change.prepare();
            observations[name] = {
                ops: jsonClone(prepared.ops),
                before: jsonClone(base),
                value: jsonClone(prepared.value),
            };
        }
        assert.deepEqual(observations.set.ops, [["s", ["o", "a"], 2]]);
        assert.deepEqual(observations.append.ops, [["a", ["s"], "d"]]);
        assert.deepEqual(observations.splice.ops, [["p", ["a"], 1, 1, []], ["p", ["a"], 1, 0, ["X", "Y"]]]);
        assert.deepEqual(observations.permutation.ops, [["m", ["a"], [2, 1, 0]]]);
        assert.deepEqual(observations.delete.ops, [["d", ["o", "b"]]]);
        assert.deepEqual(observations.truncate.ops, [["t", ["s"], 3]]);
        assert.deepEqual(observations.replace.ops, [["s", ["o"], { b: 2 }]]);
        scenarios.push({
            name: "the tracker emits the set, append, splice, permutation, delete, truncate, and replace vocabulary",
            source:
                "packages/chord/test/delta.test.ts:emits append and rolling-window operations; " +
                "packages/chord/test/delta.test.ts:supports root replacement, root splice, and permutations; " +
                "packages/chord/src/delta/index.ts:Op",
            inputs: { cases: Object.fromEntries(Object.entries(cases).map(([name, entry]: any) => [name, (entry as any).base])) },
            expected: observations,
            normalizations: [
                "`ops` are exact for this run. A C++ producer may choose another equivalent tuple spelling; the requirement is that the durable storage applier accepts it and the recorded `value` results.",
            ],
        });
    }

    {
        const a = "abcdefgh";
        const b = "defghxyz";
        const samples = [overlap(a, b, 65_536), overlap("abcdef", "defghi", 0), overlap("", b, 1)];
        assert.deepEqual(samples, [5, 0, 0]);
        scenarios.push({
            name: "overlap returns the bounded common run used to merge tool output",
            source:
                "packages/chord/test/delta.test.ts:finds bounded overlaps; " +
                "packages/chord/src/delta/index.ts:overlap",
            inputs: { cases: [[a, b, 65_536], ["abcdef", "defghi", 0], ["", b, 1]] },
            expected: { results: samples },
        });
    }

    return scenarios;
}

/** Attached replicated state, cursor gaps, listener failure isolation, and watch delivery scheduling. */
export async function replicatedStateScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        const harness = attachedSource({ value: { value: 1 }, cursor: 0 });
        const errors: any[] = [];
        const state = replicatedState(harness.source as any, { onError: (error: any) => errors.push(errorShape(error)) });
        const deliveries: any[] = [];
        state.subscribe((value: any, context: any, delivery: any) => {
            deliveries.push({
                kind: delivery.kind,
                sequence: delivery.sequence,
                value: jsonClone(value),
                abortSignal: context.abortSignal ?? null,
            });
        });
        harness.push({ cursor: 1, value: { value: 2 }, context: BACKGROUND_CONTEXT, ops: [["s", ["value"], 2]] });
        await sleep(10);
        state.dispose();
        assert.deepEqual(deliveries.map((delivery) => delivery.sequence), [0, 1]);
        assert.deepEqual(deliveries[1].value, { value: 2 });
        assert.equal(errors.length, 0);
        scenarios.push({
            name: "attached replicated state hydrates at cursor zero and then delivers exact frames in sequence order",
            source:
                "packages/chord/test/replicated-state.test.ts:hydrates at sequence zero when attaching after existing commits; publishes exact source values without applying or re-diffing; " +
                "packages/chord/src/state.ts:replicatedState,AttachedReplicatedState",
            inputs: { snapshot: { value: 1, cursor: 0 }, update: { value: 2, sequence: 1 } },
            expected: { deliveries, errors, disposeCalls: harness.disposed() },
        });
    }

    {
        const harness = attachedSource({ value: { value: 1 }, cursor: 0 });
        const errors: any[] = [];
        const state = replicatedState(harness.source as any, { onError: (error: any) => errors.push(errorShape(error)) });
        const deliveries: number[] = [];
        state.subscribe((_value: any, _context: any, delivery: any) => deliveries.push(delivery.sequence));
        harness.push({ cursor: 2, value: { value: 2 }, context: BACKGROUND_CONTEXT, ops: [] });
        await sleep(10);
        harness.push({ cursor: 3, value: { value: 3 }, context: BACKGROUND_CONTEXT, ops: [] });
        await sleep(10);
        state.dispose();
        assert.equal(errors.length, 1);
        assert.match(errors[0].message, /cursor has a gap: expected 1, received 2/);
        assert.deepEqual(deliveries, [0]);
        scenarios.push({
            name: "a cursor gap is reported once and leaves the last good replicated value",
            source:
                "packages/chord/test/replicated-state.test.ts:reports cursor gaps, disposes the broken attachment, and ignores later frames; " +
                "packages/chord/src/state.ts:AttachedReplicatedState",
            inputs: { snapshotCursor: 0, skippedSequence: 2, laterSequence: 3 },
            expected: { errors, deliveredSequences: deliveries, lastValue: jsonClone(state.value), disposeCalls: harness.disposed() },
        });
    }

    {
        const harness = attachedSource({ value: { value: 7 }, cursor: 0 });
        const errors: any[] = [];
        const state = replicatedState(harness.source as any, { onError: (error: any) => errors.push(errorShape(error)) });
        const received: any[] = [];
        state.subscribe(() => {
            throw new Error("listener failed");
        });
        state.subscribe((value: any, _context: any, delivery: any) => received.push({ kind: delivery.kind, sequence: delivery.sequence, value: jsonClone(value) }));
        harness.push({ cursor: 1, value: { value: 8 }, context: BACKGROUND_CONTEXT, ops: [] });
        await sleep(10);
        state.dispose();
        assert.equal(errors.length, 2);
        assert.deepEqual(received.map((entry) => entry.sequence), [0, 1]);
        scenarios.push({
            name: "a failing state listener is reported through onError without stopping another subscription",
            source:
                "packages/chord/test/replicated-state-delivery.test.ts:isolates a synchronous hydration failure without removing the subscription; " +
                "packages/chord/test/replicated-state.test.ts:reports listener failures without throwing them into the source; " +
                "packages/chord/src/state.ts:AttachedReplicatedState",
            inputs: { hydrationValue: 7, updateValue: 8 },
            expected: { errors, received },
        });
    }

    {
        const token = defineDoc({
            kind: "1057.watch-buffer",
            scope: "session",
            version: 1,
            initial: () => ({ value: 0 }),
        });
        const session = createSession(new MemoryStorage());
        await session.commit(async (tx: any) => {
            const doc = await tx.doc(token);
            doc.value = 0;
        }, BACKGROUND_CONTEXT);
        const watch = await session.watchDoc(token, BACKGROUND_CONTEXT);
        assert.ok(watch);
        try {
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 1;
            }, BACKGROUND_CONTEXT);
            await session.commit(async (tx: any) => {
                const doc = await tx.doc(token);
                doc.value = 2;
            }, BACKGROUND_CONTEXT);
            const entered = deferred<void>();
            const release = deferred<void>();
            const values: number[] = [];
            let active = 0;
            let maxActive = 0;
            watch.start(async (value: any) => {
                active += 1;
                maxActive = Math.max(maxActive, active);
                values.push(value.value);
                if (values.length === 1) {
                    entered.resolve();
                    await release.promise;
                }
                active -= 1;
            });
            await within(entered.promise, "first buffered watch callback");
            const beforeRelease = [...values];
            release.resolve();
            await sleep(10);
            assert.deepEqual(beforeRelease, [1]);
            assert.deepEqual(values, [1, 2]);
            assert.equal(maxActive, 1);
            scenarios.push({
                name: "a watch acquired before start buffers frames and serializes callbacks",
                source:
                    "packages/durable/test/session-watches.test.ts:keeps the acquisition revision until start and delivers exact committed frames; serializes callbacks and buffers exact frames committed while one is in flight; " +
                    "packages/chord/test/replicated-state-delivery.test.ts:serializes reentrant hydration and update callbacks",
                inputs: { document: "1057.watch-buffer", commitsBeforeStart: [1, 2] },
                expected: {
                    valuesBeforeRelease: beforeRelease,
                    valuesAfterRelease: values,
                    maxConcurrentCallbacks: maxActive,
                    finalValue: jsonClone(watch.value),
                },
            });
        } finally {
            watch.stop();
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }

    {
        const token = defineDoc({
            kind: "1057.watch-overflow",
            scope: "session",
            version: 1,
            initial: () => ({ value: 0 }),
        });
        const session = createSession(new MemoryStorage());
        await session.commit(async (tx: any) => {
            const doc = await tx.doc(token);
            doc.value = 0;
        }, BACKGROUND_CONTEXT);
        const watch = await session.watchDoc(token, BACKGROUND_CONTEXT);
        assert.ok(watch);
        try {
            for (let value = 1; value <= 101; value += 1) {
                await session.commit(async (tx: any) => {
                    const doc = await tx.doc(token);
                    doc.value = value;
                }, BACKGROUND_CONTEXT);
            }
            const frames: any[] = [];
            watch.start((value: any, ops: any) => frames.push({ value: value.value, ops: jsonClone(ops) }));
            await sleep(10);
            assert.equal(frames.length, 1);
            assert.equal(frames[0].ops[0][0], "r");
            assert.equal(frames[0].value, 101);
            scenarios.push({
                name: "a watch whose pending frames overflow delivers one root replacement with the latest value",
                source:
                    "packages/durable/test/session-watches.test.ts:Session document watches; " +
                    "packages/chord/src/state.ts:MutableReplicatedState.capture",
                inputs: { document: "1057.watch-overflow", commitsBeforeStart: 101 },
                expected: {
                    frameCount: frames.length,
                    rootReplacement: frames[0].ops[0][0] === "r",
                    value: frames[0].value,
                    finalValue: jsonClone(watch.value),
                },
            });
        } finally {
            watch.stop();
            await session.close(BACKGROUND_CONTEXT).catch(() => undefined);
        }
    }

    return scenarios;
}

/** Strict JSON copying and the bounded text/progress helpers consumed by durable tools. */
export async function jsonOutputScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        const source: any = { keep: { nested: [1, 2] }, omit: undefined };
        let defaultError: any;
        try {
            copyJson(source);
        } catch (error) {
            defaultError = errorShape(error);
        }
        const copied = copyJson(source, { omitUndefinedProperties: true }) as any;
        copied.keep.nested.push(3);
        assert.equal(source.keep.nested.length, 2);
        assert.deepEqual(source, { keep: { nested: [1, 2] }, omit: undefined });
        assert.deepEqual(copied, { keep: { nested: [1, 2, 3] } });
        assert.equal("omit" in copied, false);
        assert.match(defaultError?.message ?? "", /strict JSON/);
        scenarios.push({
            name: "copyJson returns a detached strict-JSON value and omits undefined object properties",
            source:
                "packages/chord/test/json.test.ts:copyJson returns detached strict JSON and omits undefined properties; " +
                "packages/chord/src/json.ts:copyJson",
            inputs: { value: { keep: { nested: [1, 2] }, omit: "undefined" } },
            expected: {
                defaultError,
                copied,
                copiedIsDetached: source.keep.nested.length === 2,
                copiedHasOmit: "omit" in copied,
                sourceAfterNestedPush: { keep: { nested: [1, 2] } },
            },
        });
    }

    {
        const failures: Record<string, any> = {};
        const cases: Array<[string, any]> = [
            ["function", { f: () => 1 }],
            ["undefinedArray", [undefined]],
            ["nonFinite", { n: Number.NaN }],
            ["classInstance", new (class Point { x = 1; })()],
            ["sparse", [1, , 3]],
        ];
        for (const [name, value] of cases) {
            try {
                copyJson(value);
                failures[name] = null;
            } catch (error) {
                failures[name] = errorShape(error);
            }
        }
        assert.match(failures.function.message, /non-JSON function/);
        assert.match(failures.undefinedArray.message, /undefined/);
        assert.match(failures.nonFinite.message, /non-finite/);
        assert.match(failures.classInstance.message, /strict JSON plain objects or arrays/);
        assert.match(failures.sparse.message, /dense plain arrays/);
        scenarios.push({
            name: "copyJson rejects functions, undefined array entries, non-finite numbers, class instances, and sparse arrays",
            source:
                "packages/chord/test/json.test.ts:copyJson rejects non-strict JSON placements; " +
                "packages/chord/src/json.ts:copyJson",
            inputs: { cases: Object.keys(failures) },
            expected: { failures },
        });
    }

    {
        const text = "aé😀";
        const bytes = Buffer.from(text, "utf8");
        const boundaries = Array.from({ length: bytes.length + 1 }, (_value, index) => characterEnd(bytes, index));
        const lengths = {
            ascii: utf8ByteLength("abc"),
            twoByte: utf8ByteLength("é"),
            fourByte: utf8ByteLength("😀"),
            mixed: utf8ByteLength(text),
        };
        assert.deepEqual(lengths, { ascii: 3, twoByte: 2, fourByte: 4, mixed: 7 });
        assert.deepEqual(boundaries, [0, 1, 1, 3, 3, 3, 3, 7]);
        scenarios.push({
            name: "UTF-8 byte length and character-end boundaries match the frozen output helpers",
            source:
                "packages/chord/src/output.ts:utf8ByteLength,characterEnd; " +
                "packages/durable/test/tools-output.test.ts:durable output helpers",
            inputs: { text, byteLengths: Array.from(bytes) },
            expected: { lengths, boundaries },
        });
    }

    {
        const head = boundOutput("abcdefghij", { maxBytes: 4, maxLines: 1, retain: "head" });
        const tail = boundOutput("abcdefghij", { maxBytes: 4, maxLines: 1, retain: "tail" });
        const buffer = new OutputBuffer({ maxBytes: 4, maxLines: 1, retain: "tail" });
        const accepted = [buffer.push("abc"), buffer.push("def"), buffer.push("gh")];
        const snapshot = buffer.snapshot();
        assert.deepEqual(head, { text: "abcd", bytes: 4, droppedBytes: 6, droppedLines: 0 });
        assert.deepEqual(tail, { text: "ghij", bytes: 4, droppedBytes: 6, droppedLines: 0 });
        assert.deepEqual(accepted, [true, true, true]);
        assert.deepEqual(snapshot, { text: "efgh", droppedBytes: 4, droppedLines: 0 });
        scenarios.push({
            name: "boundOutput and OutputBuffer retain the configured head or tail and report exact dropped counts",
            source:
                "packages/chord/test/output.test.ts:keeps output within the limits unchanged; cuts a single line longer than the byte limit on a character boundary; " +
                "packages/chord/test/output-skip.test.ts:OutputBuffer skipped output; " +
                "packages/chord/src/output.ts:boundOutput,OutputBuffer",
            inputs: { text: "abcdefghij", limits: { maxBytes: 4, maxLines: 1 }, pushes: ["abc", "def", "gh"] },
            expected: { head, tail, accepted, snapshot },
        });
    }

    {
        assert.equal(PROGRESS_BYTES_PER_SECOND, 102_400);
        scenarios.push({
            name: "the progress byte rate is the frozen 100 KiB/s constant",
            source: "packages/chord/src/output.ts:PROGRESS_BYTES_PER_SECOND; packages/durable/src/harness/tool.ts:Progress",
            inputs: { constant: "PROGRESS_BYTES_PER_SECOND" },
            expected: { bytesPerSecond: PROGRESS_BYTES_PER_SECOND },
        });
    }

    return scenarios;
}
