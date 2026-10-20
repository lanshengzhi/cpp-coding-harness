/**
 * Captured scenarios for audit-1063: the frozen pi v1.3.0 durable Session
 * properties that ticket #1063's C++ owner slice must reproduce.
 *
 * Every scenario creates fresh storage, uses only public exports, and asserts the
 * discriminating property it names before recording it, so a broken capture run
 * fails instead of recording a non-observation.
 *
 * The seven scenarios are:
 *   N1  a table read after the first table write is rejected and the transaction still commits its document
 *   N2  a close rejects every later session operation and closes the storage backend
 *   N3  a failed commit leaves the storage sequence unchanged and the session reopens from what was committed
 *   N4  a commit callback that fails after staging writes publishes nothing and leaves the session usable
 *   N5  the same public operation yields the same session and transaction records on a reopen over memory storage
 *   N6  a watch acquired at an older revision replays every op-bearing committed frame to the current revision
 *   N7  a document watch acquired before start buffers committed frames and delivers them in commit order
 */

import {
    BACKGROUND_CONTEXT,
    MemoryStorage,
    applyImmutable,
    createSession,
    defineDoc,
    defineEntry,
} from "./pi.mts";

export interface Scenario {
    readonly name: string;
    readonly source: string;
    readonly inputs: Record<string, unknown>;
    readonly expected: Record<string, unknown>;
    readonly normalizations: readonly string[];
}

/* eslint-disable @typescript-eslint/no-explicit-any */
type Any = any;

const context = BACKGROUND_CONTEXT as Any;

const NoteKind = defineEntry<{ n: number }>("audit1063.note");

const LatestNote = defineDoc<{ text: string; revision: number }>({
    kind: "audit1063.latest",
    version: 1,
    scope: "conversation",
    history: "latest",
    fork: "current",
    initial: () => ({ text: "", revision: 0 }),
});

const FrameDoc = defineDoc<{ value: number; items: string[]; nested: { count: number } }>({
    kind: "audit1063.frames",
    version: 1,
    scope: "session",
    initial: () => ({ value: 0, items: ["a"], nested: { count: 0 } }),
});

function jsonClone(value: Any): Any {
    return JSON.parse(JSON.stringify(value));
}

function errorShape(error: Any): { name: string; message: string } {
    return { name: String(error?.name ?? "Error"), message: String(error?.message ?? error) };
}

async function rejection(promise: Promise<unknown>): Promise<Record<string, unknown>> {
    try {
        await promise;
        return { rejected: false };
    } catch (error) {
        return { rejected: true, ...errorShape(error) };
    }
}

function flush(): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, 0));
}

async function createConversation(session: Any): Promise<Any> {
    return session.commit(async (tx: Any) => (await tx.createConversation({ ownership: { kind: "ownerless" } })).id, context);
}

/** A `Storage` wrapper that counts the calls the Session makes and can fail the next commit. */
function countedStorage(inner: Any): Any {
    const counts = { commit: 0, mintId: 0, close: 0, conversation: 0, entry: 0, scanEntries: 0 };
    let failure: Error | undefined;
    const storage = new Proxy(inner, {
        get(target, property, receiver) {
            if (property === "commit") {
                return async (writes: readonly unknown[], ctx: unknown) => {
                    counts.commit++;
                    if (failure !== undefined) {
                        const error = failure;
                        failure = undefined;
                        throw error;
                    }
                    return target.commit(writes, ctx);
                };
            }
            if (property === "mintId") {
                return async () => {
                    counts.mintId++;
                    return target.mintId();
                };
            }
            if (property === "close") {
                return async (ctx: unknown) => {
                    counts.close++;
                    return target.close(ctx);
                };
            }
            if (property === "conversation" || property === "entry" || property === "scanEntries") {
                return async (...args: unknown[]) => {
                    counts[property as "conversation" | "entry" | "scanEntries"]++;
                    return (target as Any)[property](...args);
                };
            }
            const value = Reflect.get(target, property, receiver);
            return typeof value === "function" ? value.bind(target) : value;
        },
    });
    return { storage, counts, failNextCommit(error: Error) { failure = error; } };
}

/** Compact a committed publication to the fields the comparison scenarios name. */
function summarizePublications(publications: readonly Any[]): Any[] {
    return publications.map((publication) => ({
        seq: publication.seq,
        changes: publication.changes.map((change: Any) => {
            if (change.type === "document" || change.type === "document.copy") {
                return {
                    type: change.type,
                    recordId: change.record.id,
                    kind: change.record.kind,
                    value: change.value ?? null,
                    ops: change.ops ?? [],
                };
            }
            return { type: change.type, id: change.value.id, kind: change.kind ?? null };
        }),
    }));
}

// ─── N1 ────────────────────────────────────────────────────────────────────

async function n1TableReadAfterFirstWrite(): Promise<Scenario> {
    const storage = new MemoryStorage();
    const session = createSession(storage);
    const publications: Any[] = [];
    session.subscribeCommits((publication: Any) => publications.push(publication));
    const conversationId = await createConversation(session);

    const conversationRead = await rejection(
        session.commit(async (tx: Any) => {
            await tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 1 } });
            return tx.conversation(conversationId);
        }, context),
    );
    const taskRead = await rejection(
        session.commit(async (tx: Any) => {
            await tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 2 } });
            return tx.task(1);
        }, context),
    );
    const entryRead = await rejection(
        session.commit(async (tx: Any) => {
            await tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 3 } });
            return tx.entry(1);
        }, context),
    );
    const scanRead = await rejection(
        session.commit(async (tx: Any) => {
            await tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 4 } });
            return tx.scanConversations({}, 10);
        }, context),
    );

    // The document access stays available after the table write and is committed by the same commit.
    const documentAfterWrite = await session.commit(async (tx: Any) => {
        await tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 5 } });
        const note = await tx.doc(LatestNote, conversationId);
        note.text = "after-write";
        note.revision = 1;
        return note.text;
    }, context);
    await flush();
    const snapshot = await session.snapshot(LatestNote, conversationId, context);
    const entries = await session.commit((tx: Any) => tx.scanEntries({ conversationId }, 10, undefined), context);

    if (conversationRead.name !== "ReadAfterWrite") {
        throw new Error(`a table read after the first table write was not a ReadAfterWrite: ${JSON.stringify(conversationRead)}`);
    }
    if (conversationRead.message !== "Tx.conversation() cannot read tables after the first table write") {
        throw new Error(`unexpected ReadAfterWrite message: ${JSON.stringify(conversationRead)}`);
    }
    if (taskRead.message !== "Tx.task() cannot read tables after the first table write") {
        throw new Error(`unexpected task read-after-write message: ${JSON.stringify(taskRead)}`);
    }
    if (entryRead.name !== "ReadAfterWrite" || scanRead.name !== "ReadAfterWrite") {
        throw new Error("a table read after the first table write was not rejected");
    }
    if (documentAfterWrite !== "after-write") {
        throw new Error("document access was not available after a table write");
    }
    if (JSON.stringify(snapshot) !== JSON.stringify({ text: "after-write", revision: 1 })) {
        throw new Error(`the document after a table write was not committed: ${JSON.stringify(snapshot)}`);
    }
    if (entries.items.length !== 1) {
        // The rejected read fails the whole commit, so the entry staged before it is rolled back.
        throw new Error(`the rejected commits persisted ${entries.items.length} entry/entries`);
    }

    await session.close(context);
    return {
        name: "a table read after the first table write is rejected and the transaction still commits its document",
        source:
            "packages/durable/test/session-tables.test.ts:allows table reads only before the first table write; " +
            "packages/durable/src/errors.ts:ReadAfterWrite; " +
            "packages/durable/src/session/transaction.ts:Transaction.#read,Transaction.#write",
        inputs: {
            backend: "MemoryStorage",
            operation: "append an entry, then read a table, then access a document",
        },
        expected: {
            conversationRead,
            taskRead,
            entryRead,
            scanRead,
            documentAfterWrite,
            snapshot,
            entryCount: entries.items.length,
            publicationCount: publications.length,
            publications: summarizePublications(publications),
        },
        normalizations: [],
    };
}

// ─── N2 ────────────────────────────────────────────────────────────────────

async function n2CloseRejectsLaterOperations(): Promise<Scenario> {
    const counted = countedStorage(new MemoryStorage());
    const session = createSession(counted.storage);
    let closeNotifications = 0;
    session.subscribeClose(() => {
        closeNotifications++;
    });
    const conversationId = await createConversation(session);

    await session.close(context);
    const commitAfterClose = await rejection(session.commit(async (tx: Any) => tx.createConversation({ ownership: { kind: "ownerless" } }), context));
    const snapshotAfterClose = await rejection(session.snapshot(LatestNote, conversationId, context));
    const storageCallAfterClose = await rejection(counted.storage.commit([], context));
    const end = await session.closed;

    if (closeNotifications !== 1) {
        throw new Error(`the close listeners ran ${closeNotifications} times`);
    }
    if (commitAfterClose.message !== "Session is closed" || snapshotAfterClose.message !== "Session is closed") {
        throw new Error(`a later session operation was not rejected: ${JSON.stringify({ commitAfterClose, snapshotAfterClose })}`);
    }
    if (counted.counts.close !== 1) {
        throw new Error(`the storage backend was closed ${counted.counts.close} times`);
    }
    if (storageCallAfterClose.message !== "MemoryStorage is closed") {
        throw new Error(`a later backend call was not rejected: ${JSON.stringify(storageCallAfterClose)}`);
    }
    if (end.reason !== "closed") {
        throw new Error(`the session did not settle as closed: ${JSON.stringify(end)}`);
    }

    return {
        name: "a close rejects every later session operation and closes the storage backend",
        source:
            "packages/durable/src/session/session.ts:SessionImpl.close,SessionImpl.#notifyClose,SessionImpl.assertUsable; " +
            "packages/durable/src/storage/memory.ts:MemoryStorage.close,MemoryStorage.assertOpen",
        inputs: {
            backend: "MemoryStorage",
            operation: "close the session, then commit, snapshot and call the backend",
        },
        expected: {
            closeNotifications,
            commitAfterClose,
            snapshotAfterClose,
            storageCallAfterClose,
            storageCloseCalls: counted.counts.close,
            sessionEnd: end,
        },
        normalizations: [],
    };
}

// ─── N3 ────────────────────────────────────────────────────────────────────

async function n3FailedCommitKeepsSequence(): Promise<Scenario> {
    // Sub-case one: a failing callback never reaches the storage, so it consumes no sequence and the
    // session stays usable. A second Session over the same storage sees only the committed records.
    const counted = countedStorage(new MemoryStorage());
    const session = createSession(counted.storage);
    const publications: Any[] = [];
    session.subscribeCommits((publication: Any) => publications.push(publication));
    const conversationId = await createConversation(session);
    const committedEntry = await session.commit(
        async (tx: Any) => tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 1 } }),
        context,
    );
    await flush();
    const commitCallsBeforeFailure = counted.counts.commit;
    const publishedBeforeFailure = publications.length;

    const failure = await rejection(
        session.commit(async (tx: Any) => {
            await tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 2 } });
            throw new Error("callback failed");
        }, context),
    );
    await flush();
    const commitCallsAfterFailure = counted.counts.commit;

    // The next successful commit takes the sequence the failed one would have taken.
    const later = await session.commit(
        async (tx: Any) => tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 3 } }),
        context,
    );
    await flush();
    const laterSeq = publications[publications.length - 1].seq;

    // A fresh Session over the same storage sees only the committed records.
    const reopened = createSession(counted.storage);
    const reopenedEntries = await reopened.commit((tx: Any) => tx.scanEntries({ conversationId }, 10, undefined), context);
    const reopenedConversation = await reopened.commit((tx: Any) => tx.conversation(conversationId), context);
    await reopened.close(context);
    await session.close(context);

    if (failure.message !== "callback failed") {
        throw new Error(`the callback failure was not observed: ${JSON.stringify(failure)}`);
    }
    if (commitCallsAfterFailure !== commitCallsBeforeFailure) {
        throw new Error(`the failed callback reached the storage ${commitCallsAfterFailure - commitCallsBeforeFailure} time(s)`);
    }
    if (publications.length !== publishedBeforeFailure + 1) {
        throw new Error(`the failed callback published a commit: ${publications.length} publications`);
    }
    if (laterSeq !== commitCallsBeforeFailure + 1) {
        throw new Error(`the later commit did not take the sequence the failed one would have taken: ${laterSeq}`);
    }
    if (reopenedEntries.items.length !== 2) {
        throw new Error(`the reopened session observed ${reopenedEntries.items.length} entries`);
    }
    if (reopenedConversation === undefined || reopenedConversation.id !== conversationId) {
        throw new Error("the reopened session did not observe the conversation");
    }

    // Sub-case two: a failing storage commit ends the Session as failed, publishes nothing and
    // closes its backend.
    const failing = countedStorage(new MemoryStorage());
    const failingSession = createSession(failing.storage);
    const failingPublications: Any[] = [];
    failingSession.subscribeCommits((publication: Any) => failingPublications.push(publication));
    const failingConversation = await failingSession.commit(
        async (tx: Any) => (await tx.createConversation({ ownership: { kind: "ownerless" } })).id,
        context,
    );
    failing.failNextCommit(new Error("disk vanished"));
    const storageFailure = await rejection(
        failingSession.commit(
            async (tx: Any) => tx.appendEntry(failingConversation, { kind: "audit1063.note", data: { n: 4 } }),
            context,
        ),
    );
    await flush();
    const rawEnd = await failingSession.closed;
    const end: Record<string, unknown> =
        rawEnd.reason === "closed"
            ? { reason: "closed" }
            : { reason: "failed", errorMessage: String(rawEnd.error?.message ?? rawEnd.error) };

    if (storageFailure.message !== "disk vanished") {
        throw new Error(`the storage failure was not observed: ${JSON.stringify(storageFailure)}`);
    }
    if (failingPublications.length !== 1) {
        throw new Error(`the failed storage commit published ${failingPublications.length} commit(s)`);
    }
    if (end.reason !== "failed" || end.errorMessage !== "disk vanished") {
        throw new Error(`the session did not settle as failed: ${JSON.stringify(end)}`);
    }
    if (failing.counts.close !== 1) {
        throw new Error(`the failed session closed its backend ${failing.counts.close} time(s)`);
    }

    return {
        name: "a failed commit leaves the storage sequence unchanged and the session reopens from what was committed",
        source:
            "packages/durable/src/storage/memory.ts:MemoryStorage.commit,MemoryStorage.prepareCommit,MemoryStorage.applyPreparedCommit; " +
            "packages/durable/src/session/session.ts:SessionImpl.#runCommit,SessionImpl.close,SessionImpl.fail",
        inputs: {
            backend: "MemoryStorage",
            operation: "a failing commit callback, a later commit, a reopen over the same storage, and a failing storage commit",
        },
        expected: {
            failure,
            storageCommitCallsBeforeFailure: commitCallsBeforeFailure,
            storageCommitCallsAfterFailure: commitCallsAfterFailure,
            laterCommitSeq: laterSeq,
            laterEntryId: later.id,
            committedEntryId: committedEntry.id,
            reopenedEntryIds: reopenedEntries.items.map((item: Any) => item.id),
            reopenedConversationId: reopenedConversation?.id ?? null,
            storageFailure,
            sessionEnd: end,
            storageFailurePublications: failingPublications.length,
            storageFailureCloseCalls: failing.counts.close,
        },
        normalizations: [],
    };
}

// ─── N4 ────────────────────────────────────────────────────────────────────

async function n4FailedCallbackAfterStaging(): Promise<Scenario> {
    const storage = new MemoryStorage();
    const session = createSession(storage);
    const publications: Any[] = [];
    session.subscribeCommits((publication: Any) => publications.push(publication));
    const conversationId = await createConversation(session);

    // A committed baseline the failing callback must not disturb.
    await session.commit(async (tx: Any) => {
        const note = await tx.doc(LatestNote, conversationId);
        note.text = "baseline";
        note.revision = 1;
    }, context);
    await flush();
    const snapshotBefore = await session.snapshot(LatestNote, conversationId, context);
    const publishedBefore = publications.length;

    let mintedEntryId: Any;
    const failure = await rejection(
        session.commit(async (tx: Any) => {
            const entry = await tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 7 } });
            mintedEntryId = entry.id;
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "lost";
            note.revision = 2;
            throw new Error("callback failed");
        }, context),
    );
    await flush();
    const entriesAfterFailure = await session.commit((tx: Any) => tx.scanEntries({ conversationId }, 10, undefined), context);
    const snapshotAfterFailure = await session.snapshot(LatestNote, conversationId, context);
    const publicationCountAfterFailure = publications.length;

    // The next commit succeeds and mints the next ID: the failed callback's ID was consumed but not stored.
    const nextEntry = await session.commit(
        async (tx: Any) => tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 8 } }),
        context,
    );

    if (failure.message !== "callback failed") {
        throw new Error(`the callback failure was not observed: ${JSON.stringify(failure)}`);
    }
    if (entriesAfterFailure.items.length !== 0) {
        throw new Error(`the failed callback persisted ${entriesAfterFailure.items.length} entry/entries`);
    }
    if (publicationCountAfterFailure !== publishedBefore) {
        throw new Error(`the failed callback published ${publicationCountAfterFailure - publishedBefore} commit(s)`);
    }
    if (JSON.stringify(snapshotAfterFailure) !== JSON.stringify(snapshotBefore)) {
        throw new Error(`the failed callback changed the committed snapshot: ${JSON.stringify(snapshotAfterFailure)}`);
    }
    if (nextEntry.id <= mintedEntryId) {
        throw new Error(`the next commit did not mint the next ID: ${nextEntry.id} <= ${mintedEntryId}`);
    }

    await session.close(context);
    return {
        name: "a commit callback that fails after staging writes publishes nothing and leaves the session usable",
        source:
            "packages/durable/src/session/session.ts:SessionImpl.#runCommit; " +
            "packages/durable/src/session/transaction.ts:Transaction.settleFailure,Transaction.#abortChanges; " +
            "packages/durable/test/session-documents.test.ts:aborts every change when the callback fails",
        inputs: {
            backend: "MemoryStorage",
            operation: "the commit callback appends an entry and mutates a committed document, then throws",
        },
        expected: {
            failure,
            mintedEntryId,
            snapshotBefore,
            entriesAfterFailure: entriesAfterFailure.items,
            snapshotAfterFailure,
            publicationCountAfterFailure,
            nextEntryId: nextEntry.id,
            nextEntryKind: nextEntry.kind,
        },
        normalizations: [],
    };
}

// ─── N5 ────────────────────────────────────────────────────────────────────

async function n5ReopenOverMemoryStorage(): Promise<Scenario> {
    // The memory backend has no reopen of its own: its close is terminal. The reopen contract is
    // therefore observed over the same open storage: a second Session over one storage observes the
    // same records and the same commit sequence per entry, and the ID namespace continues above
    // every previously minted or stored ID.
    const storage = new MemoryStorage();
    const session = createSession(storage);
    const conversationId = await createConversation(session);
    const entry = await session.commit(
        async (tx: Any) => tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 9 } }),
        context,
    );
    const documentValue = await session.commit(async (tx: Any) => {
        const note = await tx.doc(LatestNote, conversationId);
        note.text = "kept";
        note.revision = 4;
        return note.text;
    }, context);
    await flush();
    const storedEntry = await storage.entry(entry.id, context);
    const documentRecord = await storage.findDocument(
        { kind: "audit1063.latest", scope: { kind: "conversation", conversationId } },
        "current",
        context,
    );

    // A second Session over the same open storage observes the same records and the same commit
    // sequence per entry.
    const reopened = createSession(storage);
    const reopenedConversation = await reopened.commit((tx: Any) => tx.conversation(conversationId), context);
    const reopenedEntry = await reopened.commit((tx: Any) => tx.entry(entry.id), context);
    const reopenedEntries = await reopened.commit((tx: Any) => tx.scanEntries({ conversationId }, 10, undefined), context);
    const reopenedSnapshot = await reopened.snapshot(LatestNote, conversationId, context);
    // The ID namespace continues above every previously minted or stored ID.
    const reopenedEntryId = await reopened.commit(
        async (tx: Any) => tx.appendEntry(conversationId, { kind: "audit1063.note", data: { n: 10 } }),
        context,
    );
    await reopened.close(context);
    await session.close(context);

    if (reopenedConversation === undefined) {
        throw new Error("the second session did not observe the conversation");
    }
    if (reopenedEntry === undefined || reopenedEntry.id !== entry.id || reopenedEntry.kind !== entry.kind) {
        throw new Error(`the second session observed another entry: ${JSON.stringify(reopenedEntry)}`);
    }
    if (reopenedEntries.items.length !== 1 || reopenedEntries.items[0].id !== entry.id) {
        throw new Error(`the second session observed ${reopenedEntries.items.length} entries`);
    }
    if (JSON.stringify(reopenedSnapshot) !== JSON.stringify({ text: "kept", revision: 4 })) {
        throw new Error(`the second session observed another document value: ${JSON.stringify(reopenedSnapshot)}`);
    }
    if (reopenedEntryId.id <= entry.id) {
        throw new Error(`the second session did not continue the ID namespace: ${reopenedEntryId.id} <= ${entry.id}`);
    }

    return {
        name: "the same public operation yields the same session and transaction records on a reopen over memory storage",
        source:
            "packages/durable/src/types.ts:Storage.commit,Storage.mintId; " +
            "packages/durable/src/storage/memory.ts:MemoryStorage.commit,MemoryStorage.mintId,MemoryStorage.entry",
        inputs: {
            backend: "MemoryStorage",
            operation: "a conversation, one entry and one document commit, then a second session over the same storage",
        },
        expected: {
            conversationId,
            entryId: entry.id,
            entryKind: entry.kind,
            entryCommitSeq: storedEntry?.commitSeq ?? null,
            documentRecordId: documentRecord?.id ?? null,
            documentCreatedAt: documentRecord?.createdAt ?? null,
            documentValue,
            reopenedConversationId: reopenedConversation?.id ?? null,
            reopenedEntryId: reopenedEntry?.id ?? null,
            reopenedEntryKind: reopenedEntry?.kind ?? null,
            reopenedEntryCount: reopenedEntries.items.length,
            reopenedSnapshot,
            reopenedNextEntryId: reopenedEntryId.id,
        },
        normalizations: [],
    };
}

// ─── N6 ────────────────────────────────────────────────────────────────────

async function n6WatchReplaysOpBearingFrames(): Promise<Scenario> {
    const storage = new MemoryStorage();
    const session = createSession(storage);
    const publications: Any[] = [];
    session.subscribeCommits((publication: Any) => publications.push(publication));

    await session.commit(async (tx: Any) => {
        const doc = await tx.doc(FrameDoc);
        doc.value = 1;
    }, context);
    const watch = await session.watchDoc(FrameDoc, context);
    if (watch === undefined) throw new Error("the watch was not acquired");

    const frames: Any[] = [];
    watch.start((value: Any, ops: Any) => {
        frames.push({ value: jsonClone(value), ops: jsonClone(ops) });
    });
    const acquisitionValue = jsonClone(watch.value);

    await session.commit(async (tx: Any) => {
        const doc = await tx.doc(FrameDoc);
        doc.value = 2;
    }, context);
    await session.commit(async (tx: Any) => {
        const doc = await tx.doc(FrameDoc);
        doc.items.push("c");
        doc.nested.count = 1;
    }, context);
    await flush();

    const finalSnapshot = jsonClone(await session.snapshot(FrameDoc, context));
    let replayed: Any = acquisitionValue;
    for (const frame of frames) replayed = applyImmutable(replayed, frame.ops);
    const replayMatchesFinal = JSON.stringify(replayed) === JSON.stringify(finalSnapshot);
    const commitSeqs = publications.map((publication) => publication.seq);
    const publicationOps = publications.map((publication) => {
        const change = publication.changes.find((entry: Any) => entry.type === "document");
        return change === undefined ? [] : jsonClone(change.ops);
    });

    if (frames.length !== 2) {
        throw new Error(`the watch delivered ${frames.length} frame(s)`);
    }
    if (!frames.every((frame) => frame.ops.length > 0)) {
        throw new Error("a delivered frame carried no operations");
    }
    if (!replayMatchesFinal) {
        throw new Error(`the replayed value differs from the final snapshot: ${JSON.stringify(replayed)}`);
    }
    if (JSON.stringify(acquisitionValue) !== JSON.stringify({ value: 1, items: ["a"], nested: { count: 0 } })) {
        throw new Error(`the acquisition value is not the older revision: ${JSON.stringify(acquisitionValue)}`);
    }

    await watch.stop();
    await session.close(context);
    return {
        name: "a watch acquired at an older revision replays every op-bearing committed frame to the current revision",
        source:
            "packages/durable/test/session-watches.test.ts:can replay every delivered exact operation batch from the acquisition revision; " +
            "packages/durable/src/session/observation.ts:CommittedWatch; " +
            "packages/durable/src/session/session.ts:SessionImpl.#attachDocument; " +
            "packages/chord/src/state.ts:ReplicatedStateSource,ReplicatedStateSourceAttachment",
        inputs: {
            backend: "MemoryStorage",
            document: "audit1063.frames",
            initial: { value: 0, items: ["a"], nested: { count: 0 } },
            watchedAfterCommit: 1,
            commits: [{ mutation: "value = 2" }, { mutation: 'items.push("c"); nested.count = 1' }],
        },
        expected: {
            acquisitionValue,
            commitSeqs,
            publicationOps,
            frames,
            nonEmptyFrameOps: frames.every((frame) => frame.ops.length > 0),
            replayMatchesFinal,
            finalSnapshot,
        },
        normalizations: [
            "`ops` are the exact emitted tuples for this run; a C++ test compares frame order, the replay of the recorded tuples onto the acquisition value, and the final value. Chord does not document operation spelling as canonical, so a C++ producer may emit an equivalent batch.",
        ],
    };
}

// ─── N7 ────────────────────────────────────────────────────────────────────

async function n7WatchBuffersBeforeStart(): Promise<Scenario> {
    const storage = new MemoryStorage();
    const session = createSession(storage);
    await session.commit(async (tx: Any) => {
        const doc = await tx.doc(FrameDoc);
        doc.value = 1;
    }, context);
    const watch = await session.watchDoc(FrameDoc, context);
    if (watch === undefined) throw new Error("the watch was not acquired");

    await session.commit(async (tx: Any) => {
        const doc = await tx.doc(FrameDoc);
        doc.value = 2;
    }, context);
    await session.commit(async (tx: Any) => {
        const doc = await tx.doc(FrameDoc);
        doc.items.push("c");
        doc.nested.count = 1;
    }, context);
    await flush();

    const acquisitionValue = jsonClone(watch.value);
    const deliveries: Any[] = [];
    let active = 0;
    let maxConcurrentCallbacks = 0;
    watch.start((value: Any, ops: Any) => {
        active++;
        maxConcurrentCallbacks = Math.max(maxConcurrentCallbacks, active);
        deliveries.push({ value: jsonClone(value), ops: jsonClone(ops) });
        active--;
    });
    // The frozen watch buffers every committed frame until it is started, then delivers them in commit
    // order; no frame is lost or duplicated.
    await flush();

    const deliveredValues = deliveries.map((delivery) => delivery.value.value);
    if (deliveries.length !== 2) {
        throw new Error(`the watch delivered ${deliveries.length} frame(s)`);
    }
    if (JSON.stringify(deliveredValues) !== JSON.stringify([2, 2])) {
        throw new Error(`the buffered frames were not delivered in commit order: ${JSON.stringify(deliveredValues)}`);
    }
    if (JSON.stringify(deliveries[0].value.items) !== JSON.stringify(["a"])) {
        throw new Error(`the first delivered frame carried another value: ${JSON.stringify(deliveries[0].value)}`);
    }
    if (JSON.stringify(deliveries[1].value.items) !== JSON.stringify(["a", "c"])) {
        throw new Error(`the second delivered frame carried another value: ${JSON.stringify(deliveries[1].value)}`);
    }
    if (maxConcurrentCallbacks !== 1) {
        throw new Error(`the watch serialized ${maxConcurrentCallbacks} concurrent callbacks`);
    }

    await watch.stop();
    await session.close(context);
    return {
        name: "a document watch acquired before start buffers committed frames and delivers them in commit order",
        source:
            "packages/durable/test/session-watches.test.ts:keeps the acquisition revision until start and delivers exact committed frames; " +
            "packages/durable/src/session/observation.ts:CommittedWatch.start,CommittedWatch.advance",
        inputs: {
            backend: "MemoryStorage",
            document: "audit1063.frames",
            acquisition: "the watch is acquired after the first commit and started after two later commits",
        },
        expected: {
            acquisitionValue,
            deliveredValues,
            deliveryCount: deliveries.length,
            deliveries,
            maxConcurrentCallbacks,
        },
        normalizations: [],
    };
}

export async function durableSessionScenarios(): Promise<Scenario[]> {
    return [
        await n1TableReadAfterFirstWrite(),
        await n2CloseRejectsLaterOperations(),
        await n3FailedCommitKeepsSequence(),
        await n4FailedCallbackAfterStaging(),
        await n5ReopenOverMemoryStorage(),
        await n6WatchReplaysOpBearingFrames(),
        await n7WatchBuffersBeforeStart(),
    ];
}
