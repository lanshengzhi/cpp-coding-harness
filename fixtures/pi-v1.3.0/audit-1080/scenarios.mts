/**
 * Frozen pi v1.3.0 observations for `audit-1080`: submission/inbox boundary placement,
 * request-ID dedup with a different payload, conversation-abort outcomes and atomic
 * queued admission.
 *
 * Every scenario drives public `Harness`/`Conversation`/`Submission` calls (plus the public
 * `Storage` seam where admission atomicity is the property) and returns one comparison unit for
 * the manifest. Helpers are adapted from the `audit-1055` scenarios; nothing is imported across
 * family directories.
 */

import {
    BACKGROUND_CONTEXT,
    defineExtension,
    defineTool,
    fauxAssistantMessage,
    fauxText,
    fauxToolCall,
    Harness,
    InboxDoc,
    LiveDoc,
    MemoryStorage,
    createModels,
    createRegistry,
    fauxProvider,
    Type,
    type Scenario,
} from "./pi.mts";

function deferred(): { promise: Promise<void>; resolve: () => void } {
    let resolve!: () => void;
    return { promise: new Promise<void>((done) => { resolve = done; }), resolve: () => resolve() };
}

function answer(text: string) {
    return fauxAssistantMessage([fauxText(text)]);
}

/** A faux response held until `release` or cancellation; `reached` resolves when the request is sent. */
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

/** Register a `hold` tool whose first execution resolves `entered` and then waits for `release`. */
function holdTool(registry: any, entered: () => void) {
    const gate = deferred();
    let executions = 0;
    registry.install(defineExtension({
        name: "held-tool",
        tools: [defineTool({
            name: "hold",
            description: "Waits for the test",
            parameters: Type.Object({}),
            execute: async () => {
                executions++;
                entered();
                await gate.promise;
                return { output: [{ type: "text", text: "held" }] };
            },
        })],
    }));
    return {
        executions: () => executions,
        release: () => gate.resolve(),
    };
}

class RecordingStorage extends MemoryStorage {
    readonly batches: any[][] = [];

    override async commit(writes: readonly any[], context: any): Promise<any> {
        this.batches.push(structuredClone(writes));
        return super.commit(writes, context);
    }
}

/** Open a harness whose root conversation uses a faux provider table. */
async function openChat(storage: any, options: { readonly settings?: any; readonly tools?: (registry: any) => void } = {}): Promise<any> {
    const faux = fauxProvider();
    const registry = createRegistry();
    if (options.tools !== undefined) options.tools(registry);
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

function textOf(entry: any): string | undefined {
    const message = entry.model?.[0];
    if (message?.role === "system") return undefined;
    if (message?.role === "toolResult") return undefined;
    if (typeof message?.content === "string") return message.content;
    const text = message?.content?.find?.((content: any) => content.type === "text");
    return text?.type === "text" ? text.text : undefined;
}

/** Kind and text of each entry, skipping system entries. */
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

async function statusOf(submission: any): Promise<string> {
    return (await submission.status(BACKGROUND_CONTEXT)).status;
}

async function inboxItems(harness: any, root: any): Promise<any[]> {
    const snapshot = await harness.snapshot(InboxDoc, root.id, BACKGROUND_CONTEXT);
    return snapshot === undefined ? [] : snapshot.items;
}

/** Status, answering assistant-entry identity and ordinal for one settled submission. */
function answerIdentity(settled: any, assistantIds: readonly string[]) {
    return {
        status: settled.status,
        answer: settled.answer,
        answerIndex: assistantIds.indexOf(settled.answer),
    };
}

export async function steerAtPostToolsFollowUpHeldToFinal(): Promise<Scenario> {
    const reached = deferred();
    let hold!: ReturnType<typeof holdTool>;
    const { harness, root, faux } = await openChat(new MemoryStorage(), {
        tools: (registry) => {
            hold = holdTool(registry, () => reached.resolve());
        },
    });
    faux.setResponses([
        fauxAssistantMessage([fauxToolCall("hold", {}, { id: "c1" })], { stopReason: "toolUse" }),
        answer("after tools"),
        answer("follow-up"),
    ]);
    const first = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reached.promise;
    const steer = await root.submit({ type: "input", content: "s", whenBusy: "steer" }, BACKGROUND_CONTEXT);
    const followUp = await root.submit({ type: "input", content: "f" }, BACKGROUND_CONTEXT);
    const before = {
        first: await statusOf(first),
        steer: await statusOf(steer),
        followUp: await statusOf(followUp),
        inbox: (await inboxItems(harness, root)).map((item: any) => ({
            mode: item.mode,
            isSteer: item.id === steer.id,
            isFollowUp: item.id === followUp.id,
        })),
    };
    const entriesBefore = await entriesOf(root);
    hold.release();
    const settledFollowUp = await followUp.wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(root);
    const assistant = entries.filter((entry) => entry.kind === "pi.assistant");
    const assistantIds = assistant.map((entry) => entry.id);
    const firstSettled = await first.status(BACKGROUND_CONTEXT);
    const steerSettled = await steer.status(BACKGROUND_CONTEXT);
    const followUpSettled = settledFollowUp;
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a busy steer input is placed at the postTools boundary and a busy follow-up is held to the final boundary",
        source: "packages/durable/src/harness/inbox.ts:applyBoundary,prepareBoundary",
        expected: {
            before,
            busyTranscript: transcript(entriesBefore),
            transcript: transcript(entries),
            assistantAnswers: assistant.map((entry) => textOf(entry) ?? null),
            answers: assistantIds,
            first: answerIdentity(firstSettled, assistantIds),
            steer: answerIdentity(steerSettled, assistantIds),
            followUp: answerIdentity(followUpSettled, assistantIds),
        },
    };
}

export async function requestIdDifferentPayloadReturnsExisting(): Promise<Scenario> {
    const storage = new RecordingStorage();
    const reached = deferred();
    const held = heldStep(() => reached.resolve());
    const { harness, root, faux } = await openChat(storage);
    faux.setResponses([held.step]);
    const first = await root.submit({ type: "input", content: "a", requestId: "req-1080" }, BACKGROUND_CONTEXT);
    await reached.promise;
    const batchesBefore = storage.batches.length;
    const inboxBefore = (await inboxItems(harness, root)).length;
    const duplicate = await root.submit(
        { type: "input", content: "a different payload", requestId: "req-1080" },
        BACKGROUND_CONTEXT,
    );
    const duplicateBatches = storage.batches.length - batchesBefore;
    const inboxAfter = (await inboxItems(harness, root)).length;
    const originalStatus = await statusOf(first);
    const duplicateStatus = await statusOf(duplicate);
    held.release();
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a duplicate request ID with a different payload returns the existing submission with zero new storage batches",
        source: "packages/durable/src/harness/submissions.ts:admitSubmission,submissionByRequest",
        expected: {
            sameId: duplicate.id === first.id,
            duplicateBatches,
            originalStatus,
            duplicateStatus,
            inboxBefore,
            inboxAfter,
        },
    };
}

export async function conversationAbortQueuedInputVsWrite(): Promise<Scenario> {
    const reached = deferred();
    const held = heldStep(() => reached.resolve());
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([held.step]);
    const placed = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reached.promise;
    const queuedInput = await root.submit({ type: "input", content: "b" }, BACKGROUND_CONTEXT);
    const queuedWrite = await root.submit(
        { type: "write", entry: { kind: "note", model: [{ role: "user", content: "n", timestamp: 0 }] } },
        BACKGROUND_CONTEXT,
    );
    const before = await Promise.all([statusOf(placed), statusOf(queuedInput), statusOf(queuedWrite)]);
    await root.abort(BACKGROUND_CONTEXT);
    const handles = await Promise.all([
        harness.submission(placed.id, BACKGROUND_CONTEXT),
        harness.submission(queuedInput.id, BACKGROUND_CONTEXT),
        harness.submission(queuedWrite.id, BACKGROUND_CONTEXT),
    ]);
    const after = await Promise.all(handles.map((handle) => handle.status(BACKGROUND_CONTEXT)));
    const inbox = await inboxItems(harness, root);
    const live = await harness.snapshot(LiveDoc, root.id, BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a conversation abort settles the queued input unanswered and aborted, keeps the queued write queued and settles the placed input unanswered and aborted",
        source: "packages/durable/src/harness/inbox.ts:withdrawQueuedInputs,packages/durable/src/harness/submissions.ts:Submissions.abort",
        expected: {
            before,
            after: after.map((record: any) =>
                record === undefined ? null : { status: record.status, reason: record.reason ?? null },
            ),
            inbox: inbox.map((item: any) => ({
                mode: item.mode,
                isQueuedWrite: item.id === queuedWrite.id,
                isQueuedInput: item.id === queuedInput.id,
            })),
            live: live ?? {},
        },
    };
}

export async function followUpAtFinalBoundaryAnsweredBySecondRun(): Promise<Scenario> {
    const reached = deferred();
    const held = heldStep(() => reached.resolve());
    const { harness, root, faux } = await openChat(new MemoryStorage());
    faux.setResponses([held.step, answer("second")]);
    const first = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reached.promise;
    const followUp = await root.submit({ type: "input", content: "f" }, BACKGROUND_CONTEXT);
    const before = {
        first: await statusOf(first),
        followUp: await statusOf(followUp),
        inbox: (await inboxItems(harness, root)).map((item: any) => ({
            mode: item.mode,
            isFollowUp: item.id === followUp.id,
        })),
    };
    held.release();
    const settledFollowUp = await followUp.wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(root);
    const assistant = entries.filter((entry) => entry.kind === "pi.assistant");
    const assistantIds = assistant.map((entry) => entry.id);
    const firstSettled = await first.status(BACKGROUND_CONTEXT);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a busy follow-up is placed at the final boundary and answered by a second run",
        source: "packages/durable/src/harness/generation.ts:endRun,packages/durable/src/harness/inbox.ts:applyBoundary",
        expected: {
            before,
            transcript: transcript(entries),
            assistantAnswers: assistant.map((entry) => textOf(entry) ?? null),
            answers: assistantIds,
            first: answerIdentity(firstSettled, assistantIds),
            followUp: answerIdentity(settledFollowUp, assistantIds),
        },
    };
}

export async function atomicQueuedAdmissionBatch(): Promise<Scenario> {
    const storage = new RecordingStorage();
    const reached = deferred();
    const held = heldStep(() => reached.resolve());
    const { harness, root, faux } = await openChat(storage);
    faux.setResponses([held.step]);
    await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reached.promise;
    const before = storage.batches.length;
    await root.submit({ type: "input", content: "queued" }, BACKGROUND_CONTEXT);
    const batch = storage.batches[before]!;
    const submission = batch.find((write) => write.type === "submission");
    const document = batch.find((write) => write.type === "document.change");
    const appended = document?.content?.ops?.find((op: any) => op[1]?.[0] === "items");
    const documentKind = `${InboxDoc.definition.kind}:v${InboxDoc.definition.version}`;
    held.release();
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a busy queued admission commits one batch with the submission and the pi.inbox items change",
        source: "packages/durable/src/session/transaction.ts:createSubmission,packages/durable/src/session/session.ts:#runCommit",
        expected: {
            batchTypes: batch.map((write) => write.type),
            submissionStatus: submission?.value?.status,
            submissionType: submission?.value?.type,
            documentKind,
            contentKind: document?.content?.kind,
            documentVersion: document?.content?.version,
            inboxPath: Array.isArray(appended?.[1]) ? appended[1][0] : null,
        },
    };
}
