/**
 * `audit-1056` conversation lifecycle: lazy root creation, history forks with as-of agent state,
 * reset/handoff, task ownership, abort, and reopen of the same durable conversations.
 */

import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import {
    AgentDoc,
    BACKGROUND_CONTEXT,
    Harness,
    LiveDoc,
    MemoryStorage,
    ProviderDoc,
    Type,
    createModels,
    createRegistry,
    defineExtension,
    defineTask,
    defineTool,
    fauxProvider,
    openNodeSqliteStorage,
} from "./pi.mts";
import {
    answer,
    assert,
    closeChat,
    entriesOf,
    entryText,
    openChat,
    textOf,
    transcript,
    type Scenario,
} from "./support.mts";

export async function rootCreation(): Promise<Scenario> {
    const faux = fauxProvider({});
    const models = createModels();
    models.setProvider(faux.provider);
    const registry = createRegistry();
    const harness = await Harness.open(new MemoryStorage(), { models, registry }, BACKGROUND_CONTEXT);
    const publications: any[] = [];
    harness.subscribeCommits((publication: any) => publications.push(publication));
    const root = await harness.root(BACKGROUND_CONTEXT, { agent: { thinkingLevel: "high" } });
    const again = await harness.root(BACKGROUND_CONTEXT, { agent: { thinkingLevel: "low" } });
    const expected = {
        rootId: root.id,
        sameRoot: again.id === root.id,
        commits: publications.length,
        changes: publications[0].changes
            .map((change: any) =>
                change.type === "document"
                    ? `document:${change.record.kind}:v${change.version}`
                    : change.type,
            )
            .sort(),
        agent: await harness.snapshot(AgentDoc, root.id, BACKGROUND_CONTEXT),
    };
    await harness.close(BACKGROUND_CONTEXT);
    void faux;
    return {
        name: "the root conversation is created lazily in one commit with its five built-in documents",
        source: "packages/durable/src/harness/harness.ts:HarnessImpl.root,#create;packages/durable/src/harness/agent.ts:AgentDoc",
        inputs: { storage: "MemoryStorage", rootCreationOptions: "thinkingLevel high", secondRoot: "thinkingLevel low" },
        expected,
    };
}

export async function forkAsOf(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    await chat.root.configure({ thinkingLevel: "low" }, BACKGROUND_CONTEXT);
    chat.faux.setResponses([answer("a1"), answer("a2")]);
    await (await chat.root.submit({ type: "input", content: "u1" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const afterFirst = await entriesOf(chat.root);
    await chat.root.configure({ thinkingLevel: "high" }, BACKGROUND_CONTEXT);
    await (await chat.root.submit({ type: "input", content: "u2" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const all = await entriesOf(chat.root);
    const before = await chat.root.fork(afterFirst[0].id, { ownership: { kind: "ownerless" } }, BACKGROUND_CONTEXT);
    const after = await chat.root.fork(all.at(-1).id, { ownership: { kind: "ownerless" } }, BACKGROUND_CONTEXT);
    const forkBeforeAgent = await before.agent(BACKGROUND_CONTEXT);
    const forkAfterAgent = await after.agent(BACKGROUND_CONTEXT);
    await before.commit((tx: any) => tx.appendEntry(before.id, { kind: "fork.note", data: { text: "only here" } }), BACKGROUND_CONTEXT);
    const expected = {
        rootEntries: transcript(all),
        beforeEntries: transcript(await entriesOf(before)),
        afterEntries: transcript(await entriesOf(after)),
        beforeThinking: forkBeforeAgent.thinkingLevel,
        afterThinking: forkAfterAgent.thinkingLevel,
        rootStillHasTwoTurns: (await entriesOf(chat.root)).length === all.length,
        forksAreDistinct: before.id !== after.id && before.id !== chat.root.id,
    };
    await closeChat(chat);
    return {
        name: "a fork copies the transcript and agent state as of its entry and stays independent",
        source: "packages/durable/src/harness/harness.ts:ConversationImpl.fork;#create;packages/durable/src/harness/agent.ts:AgentDoc.asOf",
        inputs: {
            turns: ["u1", "u2"],
            configure: ["low before u1", "high before u2"],
            forkAt: ["first entry", "last entry"],
            forkOwnership: "ownerless",
        },
        expected,
    };
}

export async function resetHandoff(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    chat.faux.setResponses([answer("a1"), answer("a2")]);
    await (await chat.root.submit({ type: "input", content: "u1" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    await chat.root.reset("carry this forward", BACKGROUND_CONTEXT);
    await (await chat.root.submit({ type: "input", content: "u2" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const view = await chat.root.context(BACKGROUND_CONTEXT);
    const expected = {
        entries: transcript(entries),
        reset: entries
            .filter((entry: any) => entry.kind === "pi.reset")
            .map((entry: any) => ({ kind: entry.kind, headIsSelf: entry.head === entry.id, modelText: entryText(entry) })),
        contextRoles: view.messages.map((message: any) => message.role),
        contextTexts: view.messages.map((message: any) => textOf(message) ?? message.role),
    };
    await closeChat(chat);
    return {
        name: "reset starts a new context and carries its handoff text as the first user message",
        source: "packages/durable/src/harness/harness.ts:ConversationImpl.reset;packages/durable/src/entries.ts:ResetEntry",
        inputs: { turns: ["u1", "u2"], resetHandoff: "carry this forward" },
        expected,
    };
}

export async function taskOwnedConversation(): Promise<Scenario> {
    const Step = defineTask<null, { phase: "run" }, { childId: number | undefined }>({
        name: "audit.owner",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (task: any, runtime: any, context: any) => {
                let child: number | undefined;
                await runtime.commit(async (tx: any) => {
                    const conversation = await tx.createConversation({ ownership: { kind: "task", taskId: task.id } });
                    await tx.appendEntry(conversation.id, { kind: "owner.note", data: { from: "task" } });
                    child = conversation.id;
                    return undefined;
                }, context);
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: { childId: child } } }),
                    context,
                );
            },
        },
        abort: async () => {},
    });
    const chat = await openChat(new MemoryStorage());
    chat.registry.install(defineExtension({ name: "owner-task", tasks: [Step] }));
    const taskId = await chat.root.commit(
        (tx: any) => tx.createTask(Step, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    chat.harness.resume();
    const receipt = await chat.harness.waitForTask(taskId, BACKGROUND_CONTEXT);
    const childId = receipt.state.outcome.status === "completed" ? receipt.state.outcome.result.childId : undefined;
    const record = await chat.root.commit((tx: any) => tx.conversation(childId), BACKGROUND_CONTEXT);
    const child = await chat.harness.conversation(childId, BACKGROUND_CONTEXT);
    const expected = {
        outcome: receipt.state.outcome.status,
        childIdMatchesRecord: record.id === childId,
        ownerIsTask: record.owner?.taskId === taskId,
        ownerConversationIsRoot: record.owner?.conversationId === chat.root.id,
        childEntries: transcript(await entriesOf(child)),
    };
    await closeChat(chat);
    return {
        name: "a task-created conversation records its task owner and stays addressable by ID",
        source: "packages/durable/src/session/transaction.ts:Transaction.createConversation;packages/durable/src/types.ts:ConversationOwnership",
        inputs: { ownership: "task", taskPhase: "create conversation and one note, then complete" },
        expected,
    };
}

export async function abortBusyConversation(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    const never = (_request: unknown, options: any) =>
        new Promise((_resolve, reject) => {
            reached();
            options!.signal!.addEventListener("abort", () => reject(options!.signal!.reason), { once: true });
        });
    chat.faux.setResponses([never]);
    const submission = await chat.root.submit({ type: "input", content: "wait forever" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const before = await chat.harness.inspect(BACKGROUND_CONTEXT);
    await chat.root.abort(BACKGROUND_CONTEXT);
    const settled = await submission.wait(BACKGROUND_CONTEXT);
    const after = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const expected = {
        beforeTasks: before.tasks.map((task: any) => `${task.record.kind}:${task.state.kind}`),
        settled: { status: settled.status, reason: settled.reason ?? null },
        afterScheduling: after.scheduling,
        afterTasks: after.tasks.length,
        live: await chat.harness.snapshot(LiveDoc, chat.root.id, BACKGROUND_CONTEXT),
    };
    await closeChat(chat);
    return {
        name: "conversation abort stops the busy generation and settles its input unanswered",
        source: "packages/durable/src/harness/harness.ts:ConversationImpl.abort;packages/durable/src/harness/scheduler.ts:abortConversation",
        inputs: { generation: "unanswered", abort: "Conversation.abort" },
        expected,
    };
}

export async function reopenConversations(): Promise<Scenario> {
    const directory = mkdtempSync(path.join(tmpdir(), "pi-1056-conversations-"));
    const database = path.join(directory, "session.sqlite");
    try {
        const first = await openChat(await openNodeSqliteStorage(database));
        first.registry.install(
            defineExtension({
                name: "reopen-tools",
                tools: [
                    defineTool({
                        name: "present",
                        description: "present only in the first process",
                        parameters: Type.Object({}),
                        execute: async () => ({ output: [] }),
                    }),
                ],
            }),
        );
        await first.root.configure({ thinkingLevel: "medium", cwd: "/audit" }, BACKGROUND_CONTEXT);
        first.faux.setResponses([answer("persisted answer")]);
        await (await first.root.submit({ type: "input", content: "persisted question" }, BACKGROUND_CONTEXT)).wait(
            BACKGROUND_CONTEXT,
        );
        const child = await first.harness.createConversation({ ownership: { kind: "ownerless" } }, BACKGROUND_CONTEXT);
        const entries = await entriesOf(first.root);
        const fork = await first.root.fork(entries[0].id, { ownership: { kind: "ownerless" } }, BACKGROUND_CONTEXT);
        const providerIds = await Promise.all(
            [first.root, child, fork].map(async (conversation: any) => ({
                id: conversation.id,
                sessionId: (await first.harness.snapshot(ProviderDoc, conversation.id, BACKGROUND_CONTEXT))?.sessionId,
            })),
        );
        const beforeThinking = (await first.root.agent(BACKGROUND_CONTEXT)).thinkingLevel;
        const childEntries = transcript(await entriesOf(child));
        const forkEntries = transcript(await entriesOf(fork));
        await first.harness.close(BACKGROUND_CONTEXT);

        const faux = fauxProvider({});
        const models = createModels();
        models.setProvider(faux.provider);
        const reopened = await Harness.open(
            await openNodeSqliteStorage(database),
            { models, registry: createRegistry() },
            BACKGROUND_CONTEXT,
        );
        const reopenedRoot = await reopened.root(BACKGROUND_CONTEXT);
        const reopenedChild = await reopened.conversation(child.id, BACKGROUND_CONTEXT);
        const reopenedFork = await reopened.conversation(fork.id, BACKGROUND_CONTEXT);
        const reopenedIds = await Promise.all(
            [reopenedRoot, reopenedChild, reopenedFork].map(async (conversation: any) => ({
                id: conversation.id,
                sessionId: (await reopened.snapshot(ProviderDoc, conversation.id, BACKGROUND_CONTEXT))?.sessionId,
            })),
        );
        const expected = {
            rootEntries: transcript(entries),
            childEntries,
            forkEntries,
            before: {
                thinking: beforeThinking,
                providerIdsDistinct: new Set(providerIds.map((entry) => entry.sessionId)).size === 3,
            },
            after: {
                rootId: reopenedRoot.id,
                childFound: reopenedChild !== undefined,
                forkFound: reopenedFork !== undefined,
                thinking: (await reopenedRoot.agent(BACKGROUND_CONTEXT)).thinkingLevel,
                tools: (await reopenedRoot.agent(BACKGROUND_CONTEXT)).tools.map((tool: any) => tool.name),
                entries: transcript(await entriesOf(reopenedRoot)),
                providerIdsStable: providerIds.every(
                    (entry: any) => reopenedIds.find((after: any) => after.id === entry.id)?.sessionId === entry.sessionId,
                ),
            },
        };
        await reopened.close(BACKGROUND_CONTEXT);
        return {
            name: "close and reopen keeps conversations, fork identity, agent choices and provider state",
            source: "packages/durable/src/harness/harness.ts:Harness.open,root,conversation;packages/durable/src/storage/sqlite/node.ts:openNodeSqliteStorage",
            inputs: {
                storage: "node:sqlite",
                conversations: ["root", "ownerless child", "fork at first entry"],
                reopenRegistry: "empty",
            },
            expected,
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
    assert(false, "unreachable");
}

export async function allConversationScenarios(): Promise<Scenario[]> {
    return [
        await rootCreation(),
        await forkAsOf(),
        await resetHandoff(),
        await taskOwnedConversation(),
        await abortBusyConversation(),
        await reopenConversations(),
    ];
}
