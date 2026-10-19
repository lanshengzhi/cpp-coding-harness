/**
 * Captured scenarios for audit-1054: durable Session transactions, documents, observation, and the
 * memory/JSONL/SQLite storage contracts of frozen pi v1.3.0.
 *
 * Each function returns the observed scenario records the capture writes into one artifact. Every
 * scenario creates fresh storage, uses only public exports, and asserts the discriminating property
 * it names before recording it, so a broken capture run fails instead of recording a non-observation.
 */

import { DatabaseSync } from "node:sqlite";
import { appendFile, rm } from "node:fs/promises";
import { join } from "node:path";
import {
    BACKGROUND_CONTEXT,
    createSession,
    defineDoc,
    defineDocFamily,
    defineEntry,
    defineTask,
    JsonlStorage,
    MemoryStorage,
    NodeExecutionEnv,
    openNodeJsonlStorage,
    openNodeSqliteStorage,
} from "./pi.mts";
import {
    controlledStorage,
    countingStorage,
    flush,
    readJsonlStore,
    readSqliteStore,
    recordingFileSystem,
    rejection,
    removeDirectory,
    sessionEnd,
    summarizePublications,
    temporaryDirectory,
    type Scenario,
} from "./stores.mts";

const context = BACKGROUND_CONTEXT as any;

const CounterDoc = defineDoc<{ count: number }>({
    kind: "audit1054.counter",
    version: 1,
    scope: "session",
    initial: () => ({ count: 0 }),
});

const LatestNote = defineDoc<{ text: string; revision: number }>({
    kind: "audit1054.latest",
    version: 1,
    scope: "conversation",
    history: "latest",
    fork: "current",
    initial: () => ({ text: "", revision: 0 }),
    checkpointWhen: (value: any) => value.text === "checkpoint",
});

const LatestInitialNote = defineDoc<{ text: string }>({
    kind: "audit1054.latest-initial",
    version: 1,
    scope: "conversation",
    history: "latest",
    fork: "initial",
    initial: () => ({ text: "" }),
});

const RewindNote = defineDoc<{ text: string }>({
    kind: "audit1054.rewind",
    version: 1,
    scope: "conversation",
    history: "rewindable",
    fork: "asOf",
    initial: () => ({ text: "" }),
});

const NoteKind = defineEntry<{ n: number }>("audit1054.note");

const MemberDoc = defineDocFamily<{ seed: string; hits: number }, string>({
    kind: "audit1054.member",
    version: 1,
    family: true,
    scope: "session",
    initial: (seed: string) => ({ seed, hits: 0 }),
});

const WorkTask = defineTask<{ n: number }, { phase: "start"; n: number }, string>({
    name: "audit1054.work",
    version: 1,
    initial: (input: { n: number }) => ({ phase: "start", n: input.n }),
    phases: {
        start: async () => {
            throw new Error("the audit captures a pending task record, it does not run one");
        },
    },
    abort: async () => {},
});

async function createConversation(session: any): Promise<any> {
    return session.commit(async (tx: any) => (await tx.createConversation({ ownership: { kind: "ownerless" } })).id, context);
}

function latestAddress(conversationId: any): Record<string, unknown> {
    return { kind: "audit1054.latest", scope: { kind: "conversation", conversationId } };
}

async function documentRecord(storage: any, conversationId: any): Promise<unknown> {
    return storage.findDocument(latestAddress(conversationId), "current", context);
}

export async function sessionTransactionScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        const storage = new MemoryStorage();
        const session = createSession(storage);
        const publications: any[] = [];
        session.subscribeCommits((publication: any) => publications.push(publication));
        const conversationId = await createConversation(session);
        const created = await session.commit(async (tx: any) => {
            const entry = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 1 } });
            const latest = await tx.doc(LatestNote, conversationId);
            latest.text = "hello";
            latest.revision = 1;
            const counter = await tx.doc(CounterDoc);
            counter.count = 1;
            const member = await tx.doc(MemberDoc, "alpha", "seed-first");
            member.hits = 1;
            const rewind = await tx.doc(RewindNote, conversationId);
            rewind.text = "v1";
            return { entry };
        }, context);
        await flush();
        const storedEntry = await storage.entry(created.entry.id, context);
        const pages = await session.commit(
            (tx: any) => tx.scanEntries({ conversationId }, 10, undefined),
            context,
        );
        const typed = await session.commit((tx: any) => tx.entry(NoteKind, created.entry.id), context);
        if (!NoteKind.is(typed)) throw new Error("the committed typed entry did not narrow");
        scenarios.push({
            name: "a commit that changes an entry and documents observes one committed revision",
            inputs: {
                backend: "MemoryStorage",
                documentRetentionMs: 600000,
                operations: [
                    "create one ownerless conversation",
                    "append one audit1054.note entry and create four documents in one commit",
                ],
            },
            expected: {
                conversationId,
                entry: created.entry,
                entryCommitSeq: storedEntry.commitSeq,
                typedEntryIsNote: true,
                snapshots: {
                    latest: await session.snapshot(LatestNote, conversationId, context),
                    counter: await session.snapshot(CounterDoc, context),
                    memberAlpha: await session.snapshot(MemberDoc, "alpha", context),
                    rewind: await session.snapshot(RewindNote, conversationId, context),
                },
                scannedEntries: pages.items,
                documentRecord: await documentRecord(storage, conversationId),
                publicationCount: publications.length,
                publications: summarizePublications(publications),
            },
            normalizations: [],
        });
        await session.close(context);
    }

    {
        const inner = new MemoryStorage();
        const controlled = controlledStorage(inner);
        const session = createSession(controlled.storage);
        const publications: any[] = [];
        session.subscribeCommits((publication: any) => publications.push(publication));
        const conversationId = await createConversation(session);
        await session.commit(async (tx: any) => {
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "committed";
            note.revision = 1;
        }, context);
        await flush();
        const before = await session.snapshot(LatestNote, conversationId, context);
        const publishedBefore = publications.length;

        const gate = controlled.holdCommits();
        const pending = rejection(
            session.commit(async (tx: any) => {
                const note = await tx.doc(LatestNote, conversationId);
                note.text = "pending";
                note.revision = 2;
                await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 2 } });
            }, context),
        );
        await gate.entered;
        const during = await session.snapshot(LatestNote, conversationId, context);
        if (JSON.stringify(during) !== JSON.stringify(before)) {
            throw new Error("a prepared draft leaked into the committed snapshot before settlement");
        }
        if (publications.length !== publishedBefore) {
            throw new Error("a prepared transaction published before settlement");
        }
        const publicationCountWhileHeld = publications.length;
        gate.release();
        const settled = await pending;
        await flush();
        const afterSettlement = await session.snapshot(LatestNote, conversationId, context);
        if (JSON.stringify(afterSettlement) !== JSON.stringify({ text: "pending", revision: 2 })) {
            throw new Error(`the settled commit was not adopted: ${JSON.stringify(afterSettlement)}`);
        }
        if (publications.length !== publishedBefore + 1) {
            throw new Error("the settled commit published a count other than one");
        }
        const entriesAfterSettlement = await session.commit(
            (tx: any) => tx.scanEntries({ conversationId }, 10, undefined),
            context,
        );
        scenarios.push({
            name: "a prepared draft is not observable before Storage settles its commit",
            inputs: {
                backend: "MemoryStorage",
                heldAt: "public Storage.commit",
                operation: "a document draft and an entry append are prepared, then the commit is released",
            },
            expected: {
                observationBefore: before,
                observationWhileHeld: during,
                publicationCountWhileHeld,
                settledCommit: settled,
                settledSnapshot: afterSettlement,
                settledEntryIds: entriesAfterSettlement.items.map((item: any) => item.id),
                publicationCountAfterSettlement: publications.length,
                publicationsAfterSettlement: summarizePublications(publications),
            },
            normalizations: [],
        });
        await session.close(context);
    }

    {
        const session = createSession(new MemoryStorage());
        const publications: any[] = [];
        session.subscribeCommits((publication: any) => publications.push(publication));
        const conversationId = await createConversation(session);
        await session.commit(async (tx: any) => {
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "baseline";
            note.revision = 1;
        }, context);
        await flush();
        const before = await session.snapshot(LatestNote, conversationId, context);
        const publishedBefore = publications.length;
        const outcome = await rejection(
            session.commit(async (tx: any) => {
                const note = await tx.doc(LatestNote, conversationId);
                note.text = "lost";
                note.revision = 2;
                await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 2 } });
                throw new Error("callback failed");
            }, context),
        );
        await flush();
        const afterFailure = await session.snapshot(LatestNote, conversationId, context);
        const entriesAfterFailure = await session.commit(
            (tx: any) => tx.scanEntries({ conversationId }, 10, undefined),
            context,
        );
        if (publications.length !== publishedBefore) {
            throw new Error("a failed transaction callback published a commit");
        }
        const publicationCountAfterFailure = publications.length;
        const publicationsAfterFailure = summarizePublications(publications);
        const recovered = await session.commit(async (tx: any) => {
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "after-failure";
            note.revision = 3;
        }, context);
        await flush();
        if (JSON.stringify(before) !== JSON.stringify(afterFailure)) {
            throw new Error("a failed transaction callback changed the committed snapshot");
        }
        if (entriesAfterFailure.items.length !== 0) {
            throw new Error("a failed transaction callback persisted an entry");
        }
        scenarios.push({
            name: "a failed transaction callback publishes nothing and leaves the Session usable",
            inputs: {
                backend: "MemoryStorage",
                failure: "the commit callback appends an entry and mutates a document, then throws",
            },
            expected: {
                observationBefore: before,
                failure: outcome,
                observationAfterFailure: afterFailure,
                entriesAfterFailure: entriesAfterFailure.items,
                publicationCountAfterFailure,
                publicationsAfterFailure,
                recoveredCommit: recovered,
                recoveredSnapshot: await session.snapshot(LatestNote, conversationId, context),
                publicationCountAfterRecovery: publications.length,
            },
            normalizations: [],
        });
        await session.close(context);
    }

    {
        const inner = new MemoryStorage();
        const controlled = controlledStorage(inner);
        const session = createSession(controlled.storage);
        const publications: any[] = [];
        session.subscribeCommits((publication: any) => publications.push(publication));
        const conversationId = await createConversation(session);
        await session.commit(async (tx: any) => {
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "baseline";
            note.revision = 1;
        }, context);
        await flush();
        const before = await session.snapshot(LatestNote, conversationId, context);
        const publishedBefore = publications.length;

        controlled.failNextCommit(new Error("disk vanished"));
        const outcome = await rejection(
            session.commit(async (tx: any) => {
                const note = await tx.doc(LatestNote, conversationId);
                note.text = "uncertain";
                note.revision = 2;
                await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 2 } });
            }, context),
        );
        await flush();
        const snapshotAfter = await rejection(session.snapshot(LatestNote, conversationId, context));
        const commitAfter = await rejection(session.commit(() => undefined, context));
        const end = await sessionEnd(session);
        scenarios.push({
            name: "a failed Storage commit fails the Session and publishes nothing",
            inputs: {
                backend: "MemoryStorage",
                failureInjection: "one public Storage.commit call throws before reaching MemoryStorage",
            },
            expected: {
                observationBefore: before,
                failure: outcome,
                publicationCountBefore: publishedBefore,
                publicationCountAfter: publications.length,
                publicationsAfter: summarizePublications(publications),
                snapshotAfter,
                commitAfter,
                sessionEnd: end,
            },
            normalizations: [],
        });
    }

    {
        const session = createSession(new MemoryStorage());
        const conversationId = await createConversation(session);
        await session.commit(async (tx: any) => {
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "a";
            note.revision = 1;
            (await tx.doc(CounterDoc)).count = 1;
        }, context);
        await flush();
        const watch = await session.watchDoc(LatestNote, conversationId, context);
        if (watch === undefined) throw new Error("the committed document had no watch");
        const frames: unknown[] = [];
        watch.start(async (value: any, ops: any) => {
            frames.push({ value, ops });
        });
        const state = await session.documentState(CounterDoc, context);
        if (state === undefined) throw new Error("the committed session document had no state");
        await session.commit(async (tx: any) => {
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "b";
            note.revision = 2;
            (await tx.doc(CounterDoc)).count = 2;
        }, context);
        await flush();
        const stopped = await watch.stop();
        const secondWatch = await session.watchDoc(LatestNote, conversationId, context);
        if (secondWatch === undefined) throw new Error("the committed document had no second watch");
        secondWatch.start(async () => {});
        await session.close(context);
        const closedWatch = await secondWatch.closed;
        scenarios.push({
            name: "document watches and states deliver only committed frames",
            inputs: {
                backend: "MemoryStorage",
                watch: "one started watch is stopped; a second watch observes Session close",
            },
            expected: {
                frames,
                watchValueAfterCommit: watch.value,
                stateValueAfterCommit: state.value,
                stateDisposed: true,
                stopped,
                closedWatch,
            },
            normalizations: [],
        });
        state.dispose();
    }

    {
        const session = createSession(new MemoryStorage());
        const conversationId = await createConversation(session);
        const first = await session.commit(async (tx: any) => {
            const entry = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 1 } });
            (await tx.doc(RewindNote, conversationId)).text = "v1";
            (await tx.doc(LatestNote, conversationId)).text = "l1";
            return entry;
        }, context);
        const second = await session.commit(async (tx: any) => {
            const entry = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 2 } });
            (await tx.doc(RewindNote, conversationId)).text = "v2";
            (await tx.doc(LatestNote, conversationId)).text = "l2";
            return entry;
        }, context);
        const currentRewind = await session.snapshot(RewindNote, conversationId, context);
        const asOfFirst = await session.snapshotAsOf(RewindNote, conversationId, first.id, context);
        const asOfSecond = await session.snapshotAsOf(RewindNote, conversationId, second.id, context);
        const publications: any[] = [];
        session.subscribeCommits((publication: any) => publications.push(publication));
        await session.commit(async (tx: any) => {
            await tx.retireDoc(RewindNote, conversationId);
            await tx.retireDoc(LatestNote, conversationId);
        }, context);
        await flush();
        const retiredRewind = await session.snapshot(RewindNote, conversationId, context);
        const retiredAsOf = await session.snapshotAsOf(RewindNote, conversationId, first.id, context);
        const retiredLatest = await session.snapshot(LatestNote, conversationId, context);
        scenarios.push({
            name: "rewindable history reads an entry revision and retirement keeps it addressable",
            inputs: {
                backend: "MemoryStorage",
                history: "rewindable conversation document",
                cutoffs: "the two entry IDs created with each document revision",
            },
            expected: {
                firstEntry: first.id,
                secondEntry: second.id,
                currentBeforeRetire: currentRewind,
                asOfFirst,
                asOfSecond,
                currentAfterRetire: retiredRewind,
                asOfFirstAfterRetire: retiredAsOf,
                latestAfterRetire: retiredLatest,
                publications: summarizePublications(publications),
            },
            normalizations: [],
        });
        await session.close(context);
    }

    {
        const session = createSession(new MemoryStorage());
        const publications: any[] = [];
        session.subscribeCommits((publication: any) => publications.push(publication));
        const parent = await createConversation(session);
        const first = await session.commit(async (tx: any) => {
            const entry = await tx.appendEntry(parent, { kind: "audit1054.note", data: { n: 1 } });
            (await tx.doc(RewindNote, parent)).text = "v1";
            (await tx.doc(LatestNote, parent)).text = "c1";
            (await tx.doc(LatestInitialNote, parent)).text = "i1";
            return entry;
        }, context);
        await session.commit(async (tx: any) => {
            await tx.appendEntry(parent, { kind: "audit1054.note", data: { n: 2 } });
            (await tx.doc(RewindNote, parent)).text = "v2";
            (await tx.doc(LatestNote, parent)).text = "c2";
            (await tx.doc(LatestInitialNote, parent)).text = "i2";
        }, context);
        const child = await session.commit(
            async (tx: any) => (await tx.forkConversation(parent, first.id, { ownership: { kind: "ownerless" } })).id,
            context,
        );
        await flush();
        const copyPublication = publications.at(-1);
        const initialBefore = await session.snapshot(LatestInitialNote, child, context);
        await session.commit(async (tx: any) => {
            (await tx.doc(LatestInitialNote, child)).text = "initialized";
        }, context);
        scenarios.push({
            name: "forking a conversation copies as-of and current documents and leaves initial-only members absent",
            inputs: {
                backend: "MemoryStorage",
                forkAt: "the first entry of the parent conversation",
                documentForkModes: ["asOf", "current", "initial"],
            },
            expected: {
                childConversationId: child,
                parentRewind: await session.snapshot(RewindNote, parent, context),
                childRewind: await session.snapshot(RewindNote, child, context),
                childLatestCurrent: await session.snapshot(LatestNote, child, context),
                childLatestInitialBeforeAccess: initialBefore,
                childLatestInitialAfterAccess: await session.snapshot(LatestInitialNote, child, context),
                copyChanges: summarizePublications([copyPublication]).at(0)?.changes ?? [],
            },
            normalizations: [],
        });
        await session.close(context);
    }

    {
        const session = createSession(new MemoryStorage());
        const conversationId = await createConversation(session);
        const entries = await session.commit(async (tx: any) => {
            const first = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 1 } });
            const second = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 2 } });
            const headed = await tx.appendEntry(conversationId, { kind: "audit1054.head", head: "self" });
            return [first, second, headed];
        }, context);
        const firstPage = await session.commit(
            (tx: any) => tx.scanEntries({ conversationId }, 2, undefined),
            context,
        );
        const secondPage = await session.commit(
            (tx: any) => tx.scanEntries({ conversationId }, 2, firstPage.next),
            context,
        );
        const ascending = await session.commit(
            (tx: any) => tx.scanEntries({ conversationId, order: "ascending" }, 10, undefined),
            context,
        );
        const headMarker = await session.commit((tx: any) => tx.latestHeadMarker(conversationId), context);
        const conversations = await session.commit(
            (tx: any) => tx.scanConversations({ order: "ascending" }, 10, undefined),
            context,
        );
        await session.commit(async (tx: any) => {
            await tx.doc(MemberDoc, "alpha", "first-seed");
            await tx.doc(MemberDoc, "beta", "second-seed");
        }, context);
        await session.commit(async (tx: any) => {
            const existing = await tx.doc(MemberDoc, "alpha", "ignored-seed");
            existing.hits = 2;
        }, context);
        scenarios.push({
            name: "table scans continue by cursor and family members initialize from their first seed",
            inputs: {
                backend: "MemoryStorage",
                entries: 3,
                pageSize: 2,
                families: ["alpha", "beta"],
            },
            expected: {
                entryIds: entries.map((entry: any) => entry.id),
                firstPage,
                secondPage,
                ascendingIds: ascending.items.map((item: any) => item.id),
                headMarker,
                conversations,
                members: {
                    alpha: await session.snapshot(MemberDoc, "alpha", context),
                    beta: await session.snapshot(MemberDoc, "beta", context),
                },
            },
            normalizations: [],
        });
        await session.close(context);
    }

    {
        const session = createSession(new MemoryStorage());
        await createConversation(session);
        const closeNotifications: number[] = [];
        session.subscribeClose(() => closeNotifications.push(closeNotifications.length + 1));
        await session.close(context);
        const admissionAfterClose = await rejection(session.commit(() => undefined, context));
        scenarios.push({
            name: "closing seals admission and notifies close subscribers once",
            inputs: { backend: "MemoryStorage", operationAfterClose: "Session.commit" },
            expected: {
                closeNotifications,
                admissionAfterClose,
                sessionEnd: await sessionEnd(session),
            },
            normalizations: [],
        });
    }

    {
        const counted = countingStorage(new MemoryStorage());
        const session = createSession(counted.storage, { documentRetentionMs: 0 });
        await session.commit(async (tx: any) => {
            (await tx.doc(CounterDoc)).count = 1;
        }, context);
        await session.commit(async (tx: any) => {
            (await tx.doc(CounterDoc)).count = 2;
        }, context);
        const retained = await session.snapshot(CounterDoc, context);

        const defaultCounted = countingStorage(new MemoryStorage());
        const defaultSession = createSession(defaultCounted.storage);
        await defaultSession.commit(async (tx: any) => {
            (await tx.doc(CounterDoc)).count = 1;
        }, context);
        await defaultSession.commit(async (tx: any) => {
            (await tx.doc(CounterDoc)).count = 2;
        }, context);
        const defaultRetained = await defaultSession.snapshot(CounterDoc, context);
        if (counted.counts.document <= defaultCounted.counts.document) {
            throw new Error("retention 0 did not reload the document more often than the default retention");
        }
        scenarios.push({
            name: "document retention controls whether a committed document is reloaded from Storage",
            inputs: {
                backend: "MemoryStorage",
                configurations: ["documentRetentionMs: 0", "default 600000 ms"],
                operations: ["commit count 1", "commit count 2", "snapshot"],
            },
            expected: {
                retentionZero: { value: retained, reads: counted.counts },
                defaultRetention: { value: defaultRetained, reads: defaultCounted.counts },
            },
            normalizations: [],
        });
        await session.close(context);
        await defaultSession.close(context);
    }

    return scenarios;
}

export async function jsonlStoreScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        const directory = await temporaryDirectory("cch-audit1054-jsonl-order-");
        try {
            const recording = recordingFileSystem(new NodeExecutionEnv({ cwd: directory }));
            const storage = await JsonlStorage.open(directory, recording.fs, context);
            const session = createSession(storage);
            const conversationId = await createConversation(session);
            recording.appends.length = 0;
            await session.commit(async (tx: any) => {
                await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 1 } });
                const note = await tx.doc(LatestNote, conversationId);
                note.text = "ordered";
                note.revision = 1;
            }, context);
            const store = await readJsonlStore(directory);
            const main = store.find((file) => file.name === "main.jsonl");
            const sidecar = store.find((file) => file.name !== "main.jsonl");
            if (main === undefined || sidecar === undefined) throw new Error("the JSONL commit wrote no complete pair");
            if (recording.appends.at(-1) !== "main.jsonl") {
                throw new Error(`the main marker was not appended last: ${recording.appends.join(",")}`);
            }
            scenarios.push({
                name: "a JSONL commit appends every sidecar before the main marker",
                inputs: {
                    backend: "JsonlStorage",
                    commit: "one entry and one conversation document",
                    observation: "public FileSystem append order",
                },
                expected: {
                    appendOrder: recording.appends,
                    files: store,
                    mainMarker: main.lines.at(-1),
                    sidecarRecord: sidecar.lines.at(-1),
                },
                normalizations: [],
            });
            await session.close(context);
        } finally {
            await removeDirectory(directory);
        }
    }

    {
        const directory = await temporaryDirectory("cch-audit1054-jsonl-reclaim-");
        try {
            const storage = await openNodeJsonlStorage(directory, context);
            const session = createSession(storage);
            const conversationId = await createConversation(session);
            const phases: Record<string, unknown> = {};
            await session.commit(async (tx: any) => {
                (await tx.doc(LatestNote, conversationId)).text = "one";
            }, context);
            phases.afterFirstBase = await readJsonlStore(directory);
            await session.commit(async (tx: any) => {
                (await tx.doc(LatestNote, conversationId)).text = "two";
            }, context);
            phases.afterDelta = await readJsonlStore(directory);
            await session.commit(async (tx: any) => {
                (await tx.doc(LatestNote, conversationId)).text = "checkpoint";
            }, context);
            phases.afterCheckpoint = await readJsonlStore(directory);
            await session.commit(async (tx: any) => {
                await tx.retireDoc(LatestNote, conversationId);
            }, context);
            phases.afterRetire = await readJsonlStore(directory);
            if (phases.afterDelta === undefined) throw new Error("the delta phase recorded nothing");
            const deltaFiles = phases.afterDelta as Awaited<ReturnType<typeof readJsonlStore>>;
            if (deltaFiles.some((file) => file.lines.some((line: any) => line.payload?.content?.kind === "delta")) === false) {
                throw new Error("the second update wrote no delta sidecar record");
            }
            const checkpointFiles = phases.afterCheckpoint as Awaited<ReturnType<typeof readJsonlStore>>;
            if (checkpointFiles.some((file) => file.name === "main.jsonl")) {
                const entries = checkpointFiles.filter((file) => file.name !== "main.jsonl");
                if (entries.length !== 1 || entries[0]!.lines.length !== 1) {
                    throw new Error("the checkpoint did not reclaim the document sidecar to one base record");
                }
            }
            const retiredFiles = phases.afterRetire as Awaited<ReturnType<typeof readJsonlStore>>;
            if (retiredFiles.some((file) => file.name !== "main.jsonl")) {
                throw new Error("retirement left a current-only document sidecar behind");
            }
            scenarios.push({
                name: "a JSONL checkpoint replaces a current-only sidecar and retirement removes it",
                inputs: {
                    backend: "JsonlStorage",
                    document: "latest-history conversation document",
                    updates: ["base one", "delta two", "checkpoint base", "retire"],
                },
                expected: phases,
                normalizations: [],
            });
            await session.close(context);
        } finally {
            await removeDirectory(directory);
        }
    }

    {
        const directory = await temporaryDirectory("cch-audit1054-jsonl-recover-");
        try {
            const storage = await openNodeJsonlStorage(directory, context);
            const session = createSession(storage);
            const conversationId = await createConversation(session);
            await session.commit(async (tx: any) => {
                (await tx.doc(LatestNote, conversationId)).text = "kept";
            }, context);
            await session.close(context);
            const before = await readJsonlStore(directory);
            const main = before.find((file) => file.name === "main.jsonl")!;
            const sidecar = before.find((file) => file.name !== "main.jsonl")!;
            await appendFile(join(directory, "main.jsonl"), '{"format":1,"type":"commit","seq":3,"writes":[');
            await appendFile(
                join(directory, sidecar.name),
                `${JSON.stringify({
                    format: 1,
                    type: "record",
                    seq: 4,
                    ordinal: 0,
                    payload: {
                        type: "document",
                        id: Number(sidecar.name.slice("doc-".length, -".jsonl".length)),
                        content: { version: 1, kind: "delta", ops: [["s", ["text"], "unconfirmed"]] },
                    },
                })}\n`,
            );
            const reopenedStorage = await openNodeJsonlStorage(directory, context);
            const reopened = createSession(reopenedStorage);
            const recovered = await reopened.snapshot(LatestNote, conversationId, context);
            if (JSON.stringify(recovered) !== JSON.stringify({ text: "kept", revision: 0 })) {
                throw new Error(`recovery adopted an unconfirmed tail: ${JSON.stringify(recovered)}`);
            }
            const after = await readJsonlStore(directory);
            const afterMain = after.find((file) => file.name === "main.jsonl")!;
            if (afterMain.text !== main.text) throw new Error("recovery retained the torn main marker");
            const afterSidecar = after.find((file) => file.name === sidecar.name)!;
            if (afterSidecar.text !== sidecar.text) throw new Error("recovery retained the unconfirmed sidecar tail");
            scenarios.push({
                name: "JSONL recovery truncates a torn main marker and an unconfirmed sidecar tail",
                inputs: {
                    backend: "JsonlStorage",
                    simulatedCrash:
                        "after close, a torn main marker and a complete but unconfirmed sidecar record are appended to the files",
                },
                expected: {
                    filesBefore: before,
                    filesAfterRecovery: after,
                    recovered,
                },
                normalizations: [],
            });
            await reopened.close(context);
        } finally {
            await removeDirectory(directory);
        }
    }

    {
        const directory = await temporaryDirectory("cch-audit1054-jsonl-missing-");
        try {
            const storage = await openNodeJsonlStorage(directory, context);
            const session = createSession(storage);
            const conversationId = await createConversation(session);
            await session.commit(async (tx: any) => {
                (await tx.doc(LatestNote, conversationId)).text = "x";
            }, context);
            await session.close(context);
            const files = await readJsonlStore(directory);
            const sidecar = files.find((file) => file.name !== "main.jsonl")!;
            await rm(join(directory, sidecar.name));
            const outcome = await rejection(openNodeJsonlStorage(directory, context));
            if (outcome.rejected !== true) throw new Error("JSONL accepted a missing confirmed sidecar");
            scenarios.push({
                name: "JSONL refuses a main marker whose confirmed sidecar record is missing",
                inputs: {
                    backend: "JsonlStorage",
                    corruption: "delete one document sidecar after a committed close",
                },
                expected: { missingSidecar: sidecar.name, open: outcome },
                normalizations: ["temp-dir path in the error is not recorded"],
            });
        } finally {
            await removeDirectory(directory);
        }
    }

    {
        const directory = await temporaryDirectory("cch-audit1054-jsonl-poison-");
        try {
            const recording = recordingFileSystem(new NodeExecutionEnv({ cwd: directory }));
            const storage = await JsonlStorage.open(directory, recording.fs, context);
            const session = createSession(storage);
            const conversationId = await createConversation(session);
            recording.failNextAppendOf("main.jsonl");
            const outcome = await rejection(
                session.commit(async (tx: any) => {
                    await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 2 } });
                }, context),
            );
            const afterFailure = await rejection(storage.conversation(conversationId, context));
            const end = await sessionEnd(session);
            const reopenedStorage = await openNodeJsonlStorage(directory, context);
            const reopened = createSession(reopenedStorage);
            const entriesAfterRecovery = await reopened.commit(
                (tx: any) => tx.scanEntries({ conversationId }, 10, undefined),
                context,
            );
            const filesAfterRecovery = await readJsonlStore(directory);
            scenarios.push({
                name: "a JSONL main-marker I/O failure poisons the instance while reopen recovers the published prefix",
                inputs: {
                    backend: "JsonlStorage",
                    failureInjection: "the next public FileSystem append to main.jsonl returns a file error",
                },
                expected: {
                    failure: outcome,
                    readAfterFailure: afterFailure,
                    sessionEnd: end,
                    filesAfterRecovery,
                    entriesAfterRecovery: entriesAfterRecovery.items,
                },
                normalizations: [],
            });
            await reopened.close(context);
        } finally {
            await removeDirectory(directory);
        }
    }

    {
        const directory = await temporaryDirectory("cch-audit1054-jsonl-fsync-");
        try {
            const recording = recordingFileSystem(new NodeExecutionEnv({ cwd: directory }));
            const storage = await JsonlStorage.open(directory, recording.fs, context, { fsync: true });
            const session = createSession(storage);
            const conversationId = await createConversation(session);
            recording.appends.length = 0;
            recording.flushes.length = 0;
            await session.commit(async (tx: any) => {
                (await tx.doc(LatestNote, conversationId)).text = "fsynced";
            }, context);
            scenarios.push({
                name: "JSONL fsync flushes sidecar bytes before the main marker is appended",
                inputs: { backend: "JsonlStorage", options: { fsync: true }, commit: "one conversation document" },
                expected: {
                    appendOrder: recording.appends,
                    flushOrder: recording.flushes,
                    files: await readJsonlStore(directory),
                },
                normalizations: [],
            });
            await session.close(context);
        } finally {
            await removeDirectory(directory);
        }
    }

    return scenarios;
}

async function sqliteRows(path: string): Promise<Record<string, unknown>> {
    const database = new DatabaseSync(path);
    try {
        const rows = (sql: string): unknown[] => database.prepare(sql).all();
        return {
            tables: rows("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name"),
            indexes: rows("SELECT name FROM sqlite_master WHERE type = 'index' ORDER BY name"),
            strict: rows("SELECT name, strict FROM pragma_table_list WHERE schema = 'main' AND name NOT LIKE 'sqlite_%' ORDER BY name"),
            journalMode: rows("PRAGMA journal_mode"),
            schema: rows("SELECT * FROM durable_schema"),
            metadata: rows("SELECT * FROM durable_metadata"),
            recordIds: rows("SELECT * FROM record_ids ORDER BY id"),
            conversations: rows("SELECT * FROM conversations ORDER BY id"),
            entries: rows("SELECT * FROM entries ORDER BY id"),
            tasks: rows("SELECT * FROM tasks ORDER BY id"),
            submissions: rows("SELECT * FROM submissions ORDER BY id"),
            documents: rows("SELECT * FROM documents ORDER BY id"),
            documentRevisions: rows("SELECT * FROM document_revisions ORDER BY document_id, seq"),
        };
    } finally {
        database.close();
    }
}

export async function sqliteStoreScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        const directory = await temporaryDirectory("cch-audit1054-sqlite-schema-");
        try {
            const path = join(directory, "storage.sqlite");
            const storage = await openNodeSqliteStorage(path);
            const session = createSession(storage);
            const conversationId = await createConversation(session);
            await session.commit(async (tx: any) => {
                await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 1 } });
                const note = await tx.doc(LatestNote, conversationId);
                note.text = "one";
                note.revision = 1;
            }, context);
            await session.commit(async (tx: any) => {
                const note = await tx.doc(LatestNote, conversationId);
                note.text = "two";
                note.revision = 2;
            }, context);
            await session.commit(async (tx: any) => {
                const note = await tx.doc(LatestNote, conversationId);
                note.text = "checkpoint";
                note.revision = 3;
                await tx.createTask(
                    WorkTask,
                    { n: 7 },
                    { ownership: { kind: "conversation" }, conversationId },
                );
            }, context);
            await session.close(context);
            const expected = await sqliteRows(path);
            const fixture = await readSqliteStore(path);
            scenarios.push({
                name: "the SQLite schema records the durable tables, metadata and document revisions",
                inputs: {
                    backend: "SqliteStorage",
                    prefix: null,
                    operations: ["conversation", "entry", "document base", "document delta", "document checkpoint", "pending task"],
                },
                expected: { rows: expected, fixture },
                normalizations: [],
            });
        } finally {
            await removeDirectory(directory);
        }
    }

    {
        const directory = await temporaryDirectory("cch-audit1054-sqlite-prefix-");
        try {
            const path = join(directory, "storage.sqlite");
            const prefixed = await openNodeSqliteStorage(path, { prefix: "t1054_" });
            const prefixedSession = createSession(prefixed);
            const conversationId = await createConversation(prefixedSession);
            await prefixedSession.commit(async (tx: any) => {
                await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 1 } });
            }, context);
            await prefixedSession.close(context);
            const database = new DatabaseSync(path);
            const tables = database
                .prepare("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name")
                .all();
            const indexes = database
                .prepare("SELECT name FROM sqlite_master WHERE type = 'index' AND name NOT LIKE 'sqlite_%' ORDER BY name")
                .all();
            database.close();

            const unprefixed = await openNodeSqliteStorage(path);
            const unprefixedSession = createSession(unprefixed);
            const unprefixedRead = await unprefixedSession.commit(
                (tx: any) => tx.conversation(conversationId),
                context,
            );
            await unprefixedSession.close(context);

            const reopened = await openNodeSqliteStorage(path, { prefix: "t1054_" });
            const reopenedSession = createSession(reopened);
            const prefixedRead = await reopenedSession.commit(
                (tx: any) => tx.conversation(conversationId),
                context,
            );
            scenarios.push({
                name: "the SQLite schema prefix renames every durable table and isolates an unprefixed schema",
                inputs: {
                    backend: "SqliteStorage",
                    prefix: "t1054_",
                    secondOpen: "the same file without a prefix",
                },
                expected: {
                    prefixedTables: tables,
                    prefixedIndexes: indexes,
                    unprefixedRead: unprefixedRead ?? null,
                    prefixedRead: prefixedRead ?? null,
                },
                normalizations: [],
            });
            await reopenedSession.close(context);
        } finally {
            await removeDirectory(directory);
        }
    }

    {
        const directory = await temporaryDirectory("cch-audit1054-sqlite-rollback-");
        try {
            const path = join(directory, "storage.sqlite");
            const storage = await openNodeSqliteStorage(path);
            const firstSeq = await storage.commit([{ type: "conversation", value: { id: 2 } }], context);
            const invalidTask = {
                id: 5,
                conversationId: 2,
                kind: "audit1054.work",
                version: 1,
                input: {},
                background: false,
                abortRequested: false,
                state: { status: "bogus", checkpoint: { phase: "start" } },
            };
            const failure = await rejection(
                storage.commit(
                    [
                        { type: "conversation", value: { id: 3 } },
                        { type: "task", value: invalidTask },
                    ],
                    context,
                ),
            );
            if (failure.rejected !== true) throw new Error("SQLite accepted an invalid status constraint");
            const conversationAfterFailure = await storage.conversation(3, context);
            if (conversationAfterFailure !== undefined) {
                throw new Error("SQLite did not roll back the conversation written before the failed task insert");
            }
            const secondSeq = await storage.commit([{ type: "conversation", value: { id: 4 } }], context);
            await storage.close(context);
            const rows = await sqliteRows(path);
            const fixture = await readSqliteStore(path);
            scenarios.push({
                name: "a failing SQLite batch rolls back every write and leaves the sequence unchanged",
                inputs: {
                    backend: "SqliteStorage",
                    firstCommit: "conversation 2",
                    failingCommit: "conversation 3 followed by a task row that violates the status CHECK",
                },
                expected: {
                    firstSeq,
                    failure,
                    conversationAfterFailure,
                    secondSeq,
                    rows,
                    fixture,
                },
                normalizations: [],
            });
        } finally {
            await removeDirectory(directory);
        }
    }

    {
        const directory = await temporaryDirectory("cch-audit1054-sqlite-version-");
        try {
            const path = join(directory, "storage.sqlite");
            const storage = await openNodeSqliteStorage(path);
            await storage.commit([{ type: "conversation", value: { id: 2 } }], context);
            await storage.close(context);
            const database = new DatabaseSync(path);
            database.prepare("UPDATE durable_schema SET version = 2 WHERE singleton = 1").run();
            const version = database.prepare("SELECT version FROM durable_schema").all();
            database.close();
            const outcome = await rejection(openNodeSqliteStorage(path));
            if (outcome.rejected !== true) throw new Error("SQLite opened a store with a newer schema version");
            scenarios.push({
                name: "SQLite refuses a store written by a newer durable schema version",
                inputs: {
                    backend: "SqliteStorage",
                    mutation: "set durable_schema.version to 2 outside the runtime after close",
                },
                expected: { versionAfterMutation: version, open: outcome },
                normalizations: [],
            });
        } finally {
            await removeDirectory(directory);
        }
    }

    return scenarios;
}

type BackendObservation = Record<string, unknown>;

async function runBackend(backend: "memory" | "jsonl" | "sqlite"): Promise<BackendObservation> {
    const directory = backend === "memory" ? undefined : await temporaryDirectory(`cch-audit1054-backend-${backend}-`);
    const memory = backend === "memory" ? new MemoryStorage() : undefined;
    const sqlitePath = directory === undefined ? undefined : join(directory, "storage.sqlite");
    let storage =
        backend === "memory"
            ? memory
            : backend === "jsonl"
              ? await openNodeJsonlStorage(directory!, context)
              : await openNodeSqliteStorage(sqlitePath!);
    try {
        let session = createSession(storage);
        const conversationId = await createConversation(session);
        const ids = await session.commit(async (tx: any) => {
            const first = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 1 } });
            const second = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 2 } });
            const headed = await tx.appendEntry(conversationId, { kind: "audit1054.head", head: "self" });
            (await tx.doc(LatestNote, conversationId)).text = "first";
            (await tx.doc(RewindNote, conversationId)).text = "r1";
            const task = await tx.createTask(
                WorkTask,
                { n: 5 },
                { ownership: { kind: "conversation" }, conversationId },
            );
            return { first: first.id, second: second.id, headed: headed.id, task };
        }, context);
        await session.commit(async (tx: any) => {
            (await tx.doc(LatestNote, conversationId)).text = "second";
            (await tx.doc(RewindNote, conversationId)).text = "r2";
        }, context);
        const firstPage = await session.commit(
            (tx: any) => tx.scanEntries({ conversationId }, 2, undefined),
            context,
        );
        const secondPage = await session.commit(
            (tx: any) => tx.scanEntries({ conversationId }, 2, firstPage.next),
            context,
        );
        const observationBefore = {
            conversationId,
            ids,
            latest: await session.snapshot(LatestNote, conversationId, context),
            rewind: await session.snapshot(RewindNote, conversationId, context),
            asOfFirst: await session.snapshotAsOf(RewindNote, conversationId, ids.first, context),
            firstPage,
            secondPage,
            headMarker: await session.commit((tx: any) => tx.latestHeadMarker(conversationId), context),
            task: await session.commit((tx: any) => tx.task(ids.task), context),
        };
        await session.close(context);
        if (backend === "memory") {
            return { backend, observation: observationBefore, reopened: false };
        }
        storage =
            backend === "jsonl"
                ? await openNodeJsonlStorage(directory!, context)
                : await openNodeSqliteStorage(sqlitePath!);
        session = createSession(storage);
        const observationAfter = {
            latest: await session.snapshot(LatestNote, conversationId, context),
            rewind: await session.snapshot(RewindNote, conversationId, context),
            asOfFirst: await session.snapshotAsOf(RewindNote, conversationId, ids.first, context),
            entries: (await session.commit((tx: any) => tx.scanEntries({ conversationId }, 10, undefined), context)).items,
            task: await session.commit((tx: any) => tx.task(ids.task), context),
            mintedAfterReopen: await storage.mintId(),
        };
        await session.close(context);
        return { backend, observation: observationBefore, reopenedObservation: observationAfter, reopened: true };
    } finally {
        if (directory !== undefined) await removeDirectory(directory);
    }
}

export async function backendContractScenarios(): Promise<Scenario[]> {
    const memory = await runBackend("memory");
    const jsonl = await runBackend("jsonl");
    const sqlite = await runBackend("sqlite");
    if (JSON.stringify(memory.observation) !== JSON.stringify(jsonl.observation)) {
        throw new Error("memory and JSONL recorded different observations for the same public operations");
    }
    if (JSON.stringify(memory.observation) !== JSON.stringify(sqlite.observation)) {
        throw new Error("memory and SQLite recorded different observations for the same public operations");
    }
    const reopened = [jsonl.reopenedObservation, sqlite.reopenedObservation];
    if (JSON.stringify(reopened[0]) !== JSON.stringify(reopened[1])) {
        throw new Error("JSONL and SQLite reopened with different observations");
    }
    return [
        {
            name: "the same public operation yields the same records on memory, JSONL and SQLite",
            inputs: {
                backends: ["MemoryStorage", "JsonlStorage", "SqliteStorage"],
                operations: [
                    "create a conversation",
                    "append three entries including a self head marker",
                    "create a latest document and a rewindable document",
                    "create a pending task",
                    "update both documents",
                    "scan two pages of entries by cursor",
                    "read the head marker, task and as-of document",
                    "close and, for file backends, reopen and continue reading",
                ],
            },
            expected: {
                beforeReopen: memory.observation,
                reopenedObservation: reopened[0],
                memory: { reopened: false },
                jsonl: { reopened: true },
                sqlite: { reopened: true },
            },
            normalizations: [],
        },
    ];
}

type InterchangePhase = Record<string, unknown>;

async function runInterchange(backend: "jsonl" | "sqlite"): Promise<Record<string, unknown>> {
    const directory = await temporaryDirectory(`cch-audit1054-interchange-${backend}-`);
    const sqlitePath = join(directory, "storage.sqlite");
    try {
        let storage =
            backend === "jsonl"
                ? await openNodeJsonlStorage(directory, context)
                : await openNodeSqliteStorage(sqlitePath);
        let session = createSession(storage);
        const conversationId = await createConversation(session);
        const created = await session.commit(async (tx: any) => {
            const first = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 1 } });
            (await tx.doc(LatestNote, conversationId)).text = "v1";
            (await tx.doc(RewindNote, conversationId)).text = "r1";
            const task = await tx.createTask(
                WorkTask,
                { n: 7 },
                { ownership: { kind: "conversation" }, conversationId },
            );
            return { first: first.id, task };
        }, context);
        const second = await session.commit(async (tx: any) => {
            const entry = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 2 } });
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "v2";
            note.revision = 2;
            (await tx.doc(RewindNote, conversationId)).text = "r2";
            return entry.id;
        }, context);
        await session.commit(async (tx: any) => {
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "checkpoint";
            note.revision = 3;
        }, context);
        created.second = second;
        await session.close(context);

        const fixture =
            backend === "jsonl"
                ? { files: await readJsonlStore(directory) }
                : { sqlite: await readSqliteStore(sqlitePath) };
        const phases: InterchangePhase[] = [];

        storage =
            backend === "jsonl"
                ? await openNodeJsonlStorage(directory, context)
                : await openNodeSqliteStorage(sqlitePath);
        session = createSession(storage);
        phases.push({
            phase: "pi reopen and observe",
            latest: await session.snapshot(LatestNote, conversationId, context),
            rewind: await session.snapshot(RewindNote, conversationId, context),
            asOfFirst: await session.snapshotAsOf(RewindNote, conversationId, created.first, context),
            entries: (await session.commit((tx: any) => tx.scanEntries({ conversationId }, 10, undefined), context)).items,
            task: await session.commit((tx: any) => tx.task(created.task), context),
            minted: await storage.mintId(),
        });
        const continuation = await session.commit(async (tx: any) => {
            const third = await tx.appendEntry(conversationId, { kind: "audit1054.note", data: { n: 3 } });
            const note = await tx.doc(LatestNote, conversationId);
            note.text = "after-cpp-leg";
            note.revision = 4;
            (await tx.doc(RewindNote, conversationId)).text = "r3";
            return { third: third.id };
        }, context);
        await session.close(context);

        storage =
            backend === "jsonl"
                ? await openNodeJsonlStorage(directory, context)
                : await openNodeSqliteStorage(sqlitePath);
        session = createSession(storage);
        phases.push({
            phase: "pi reopen and observe the continued store",
            latest: await session.snapshot(LatestNote, conversationId, context),
            rewind: await session.snapshot(RewindNote, conversationId, context),
            asOfFirst: await session.snapshotAsOf(RewindNote, conversationId, created.first, context),
            asOfSecond: await session.snapshotAsOf(RewindNote, conversationId, created.second, context),
            entries: (await session.commit((tx: any) => tx.scanEntries({ conversationId }, 10, undefined), context)).items,
            task: await session.commit((tx: any) => tx.task(created.task), context),
            minted: await storage.mintId(),
        });
        await session.close(context);
        return {
            backend,
            conversationId,
            ids: created,
            fixture,
            phases,
            continuation,
        };
    } finally {
        await removeDirectory(directory);
    }
}

export async function interchangeScenarios(): Promise<Scenario[]> {
    const jsonl = await runInterchange("jsonl");
    const sqlite = await runInterchange("sqlite");
    const scenarios: Scenario[] = [];
    for (const captured of [jsonl, sqlite]) {
        const backend = captured.backend as string;
        scenarios.push({
            name: `${backend} store created by pi reopens with unfinished task, documents and checkpoints and continues`,
            inputs: {
                backend: backend === "jsonl" ? "JsonlStorage" : "SqliteStorage",
                creation: [
                    "one conversation and two entries",
                    "a latest document with base, delta and checkpoint base",
                    "a rewindable document with two revisions",
                    "one pending task with an initial checkpoint",
                ],
                continuation: "pi reopens, appends one entry and updates both documents, then reopens again",
                laterCppLeg: "not run: the C++ durable owner does not exist; the C++ continuation remains unverified",
            },
            expected: captured,
            normalizations: [],
        });
    }
    return scenarios;
}
