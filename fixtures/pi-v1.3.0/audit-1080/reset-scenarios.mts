/**
 * Frozen pi v1.3.0 observations for the `audit-1080` reset and stale-write boundaries — a
 * pinned-head write at an entry before the reset head settles unanswered and stale while one at
 * the reset head is done, a queued `head: "self"` reset at the postTools boundary ends the current
 * run unanswered and reset before starting the selected queued input in the reset context, and a
 * display-only write queues behind a queued reset instead of settling done at once.
 *
 * Every scenario drives public `Harness`/`Conversation` calls and returns one comparison unit for
 * the manifest. The local helpers are adapted from `scenarios.mts`, which stays byte-identical so
 * the original five observations keep their pinned generator source.
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

/** Role and text of each request message; the system baseline text is elided. */
function requestMessages(messages: readonly any[]): string[] {
    return messages.map((message) => {
        if (message.role === "system") return `${message.role}:`;
        if (typeof message.content === "string") return `${message.role}:${message.content}`;
        const text = message.content?.find?.((content: any) => content.type === "text");
        return `${message.role}:${text?.type === "text" ? text.text : ""}`;
    });
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

/** A settled-submission status with its optional reason, entry and answer. */
function settled(record: any) {
    return {
        status: record.status,
        reason: record.reason ?? null,
        entry: record.entry ?? null,
        answer: record.answer ?? null,
    };
}

export async function stalePinnedHeadWriteAfterResetHead(): Promise<Scenario> {
    const { harness, root } = await openChat(new MemoryStorage());
    const old = await root.commit((tx: any) => tx.appendEntry(root.id, { kind: "note" }), BACKGROUND_CONTEXT);
    await root.reset(undefined, BACKGROUND_CONTEXT);
    const reset = (await root.context(BACKGROUND_CONTEXT)).head!;
    const stale = await root.submit({ type: "write", entry: { kind: "summary", head: old.id } }, BACKGROUND_CONTEXT);
    const fresh = await root.submit({ type: "write", entry: { kind: "summary", head: reset.id } }, BACKGROUND_CONTEXT);
    const staleStatus = await stale.status(BACKGROUND_CONTEXT);
    const freshStatus = await fresh.status(BACKGROUND_CONTEXT);
    const entries = await entriesOf(root);
    const inbox = await inboxItems(harness, root);
    const freshEntry = entries.find((entry) => entry.id === freshStatus.entry);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a pinned-head write at the entry before the reset head settles unanswered and stale while one at the reset head is done",
        source: "packages/durable/src/harness/inbox.ts:isStale,packages/durable/src/harness/submissions.ts:admitSubmission",
        expected: {
            oldBeforeReset: old.id < reset.id,
            resetHead: { id: reset.id, kind: reset.kind, head: reset.head, headEqualsId: reset.head === reset.id },
            stale: settled(staleStatus),
            fresh: settled(freshStatus),
            freshEntryHead: freshEntry?.head ?? null,
            transcript: transcript(entries),
            inbox: inbox.length,
        },
    };
}

export async function queuedResetAtPostToolsEndsRunAndStartsQueuedInputsWithResetContext(): Promise<Scenario> {
    const reached = deferred();
    let hold!: ReturnType<typeof holdTool>;
    const requests: string[][] = [];
    const { harness, root, faux } = await openChat(new MemoryStorage(), {
        tools: (registry) => {
            hold = holdTool(registry, () => reached.resolve());
        },
    });
    const record = (request: any) => {
        requests.push(requestMessages(request.messages));
        return answer("fresh");
    };
    faux.setResponses([
        fauxAssistantMessage([fauxToolCall("hold", {}, { id: "c1" })], { stopReason: "toolUse" }),
        record,
    ]);
    const first = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reached.promise;
    const followUp = await root.submit({ type: "input", content: "f" }, BACKGROUND_CONTEXT);
    await root.reset(undefined, BACKGROUND_CONTEXT);
    const queued = {
        first: await statusOf(first),
        followUp: await statusOf(followUp),
        inbox: (await inboxItems(harness, root)).map((item: any) => ({
            mode: item.mode,
            isReset: item.entry?.head === "self",
            isFollowUp: item.id === followUp.id,
        })),
    };
    hold.release();
    const settledFollowUp = await followUp.wait(BACKGROUND_CONTEXT);
    const firstStatus = await first.status(BACKGROUND_CONTEXT);
    const entries = await entriesOf(root);
    const assistant = entries.filter((entry) => entry.kind === "pi.assistant");
    const assistantIds = assistant.map((entry) => entry.id);
    const reset = entries.find((entry) => entry.kind === "pi.reset");
    const inbox = await inboxItems(harness, root);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a queued head:self reset at the postTools boundary ends the current run unanswered and reset and starts the selected queued input in the reset context",
        source: "packages/durable/src/harness/generation.ts:finishToolRound,packages/durable/src/harness/inbox.ts:applyBoundary,hasQueuedReset,packages/durable/src/harness/live.ts:endRun",
        expected: {
            queued,
            first: settled(firstStatus),
            followUp: {
                ...settled(settledFollowUp),
                answerIndex: assistantIds.indexOf(settledFollowUp.answer),
            },
            transcript: transcript(entries),
            assistantAnswers: assistant.map((entry) => textOf(entry) ?? null),
            reset: reset === undefined ? null : { id: reset.id, head: reset.head, headEqualsId: reset.head === reset.id },
            followUpRequests: requests,
            requestCount: faux.state.callCount,
            inbox: inbox.length,
        },
    };
}

export async function displayOnlyWriteBehindQueuedResetQueues(): Promise<Scenario> {
    const reached = deferred();
    let hold!: ReturnType<typeof holdTool>;
    const { harness, root, faux } = await openChat(new MemoryStorage(), {
        tools: (registry) => {
            hold = holdTool(registry, () => reached.resolve());
        },
    });
    faux.setResponses([
        fauxAssistantMessage([fauxToolCall("hold", {}, { id: "c1" })], { stopReason: "toolUse" }),
    ]);
    const first = await root.submit({ type: "input", content: "a" }, BACKGROUND_CONTEXT);
    await reached.promise;
    await root.reset(undefined, BACKGROUND_CONTEXT);
    const note = await root.submit({ type: "write", entry: { kind: "note" } }, BACKGROUND_CONTEXT);
    const before = {
        first: await statusOf(first),
        note: await statusOf(note),
        inbox: (await inboxItems(harness, root)).map((item: any) => ({
            mode: item.mode,
            isReset: item.entry?.head === "self",
            isNote: item.id === note.id,
        })),
    };
    hold.release();
    const settledFirst = await first.wait(BACKGROUND_CONTEXT);
    const noteStatus = await note.status(BACKGROUND_CONTEXT);
    const entries = await entriesOf(root);
    const inbox = await inboxItems(harness, root);
    await harness.close(BACKGROUND_CONTEXT);
    return {
        name: "a display-only write behind a queued reset queues and lands after the reset when the run ends",
        source: "packages/durable/src/harness/submissions.ts:admitSubmission,packages/durable/src/harness/inbox.ts:isDisplayOnly,hasQueuedReset,applyBoundary",
        expected: {
            before,
            first: settled(settledFirst),
            note: settled(noteStatus),
            transcript: transcript(entries),
            inbox: inbox.length,
        },
    };
}
