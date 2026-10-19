/**
 * `audit-1056` compaction: the public manual and threshold workflows, the summary placement boundary,
 * the compaction hook decision, and the no-cut no-op outcome.
 */

import {
    BACKGROUND_CONTEXT,
    CompactionTask,
    MemoryStorage,
    UsageDoc,
    defineExtension,
    hook,
} from "./pi.mts";
import {
    answer,
    closeChat,
    entriesOf,
    entryText,
    openChat,
    textOf,
    transcript,
    type Scenario,
} from "./support.mts";

const MANUAL = { enabled: false, reserveTokens: 1000, keepRecentTokens: 150, backgroundTokens: 0 };
const BLOCKING = { enabled: true, reserveTokens: 300, keepRecentTokens: 150, backgroundTokens: 0 };

function text(label: string, tokens: number): string {
    return `${label} ${"x".repeat(Math.max(0, tokens * 4 - label.length - 1))}`;
}

type Request = { messages: any[]; options: any; model: string };

type Script = {
    agent: any[];
    summaries: any[];
    agentRequests: Request[];
    summaryRequests: Request[];
};

function isSummaryRequest(messages: readonly any[]): boolean {
    const first = messages[0];
    return (
        first?.role === "system" &&
        typeof first.content === "string" &&
        first.content.startsWith("You are a context summarization assistant")
    );
}

function script(faux: any): Script {
    const result: Script = { agent: [], summaries: [], agentRequests: [], summaryRequests: [] };
    faux.setResponses(
        Array.from(
            { length: 500 },
            () =>
                async (transcriptContext: any, options?: any, _state?: unknown, model?: { id: string }) => {
                    const request = {
                        messages: [...transcriptContext.messages],
                        options,
                        model: model!.id,
                    };
                    const summary = isSummaryRequest(request.messages);
                    (summary ? result.summaryRequests : result.agentRequests).push(request);
                    const step = (summary ? result.summaries : result.agent).shift();
                    if (step === undefined) throw new Error(`No scripted ${summary ? "summary" : "agent"} response`);
                    return typeof step === "function" ? step(request) : step;
                },
        ),
    );
    return result;
}

async function openCompaction(contextWindow: number, policy: Record<string, unknown>) {
    const settings: Record<string, unknown> = { compaction: policy, stream: { cacheRetention: "none" } };
    const chat = await openChat(new MemoryStorage(), {
        settings,
        fauxOptions: { models: [{ id: "faux-1", contextWindow, maxTokens: 900 }] },
    });
    const requests = script(chat.faux);
    chat.harness.resume();
    return { ...chat, settings, requests };
}

async function turn(chat: any, user: string, reply: string): Promise<void> {
    chat.requests.agent.push(answer(reply));
    const submission = await chat.root.submit({ type: "input", content: user }, BACKGROUND_CONTEXT);
    const settled = await submission.wait(BACKGROUND_CONTEXT);
    if (settled.status !== "done") throw new Error(`turn settled ${settled.status}`);
}

async function history(chat: any): Promise<void> {
    await turn(chat, text("u1", 100), text("a1", 100));
    await turn(chat, text("u2", 100), text("a2", 100));
    await turn(chat, text("u3", 100), text("a3", 100));
}

function userText(message: any): string {
    return textOf(message) ?? "";
}

export async function manualCompaction(): Promise<Scenario> {
    const chat = await openCompaction(100_000, MANUAL);
    await history(chat);
    const before = await entriesOf(chat.root);
    const usageBefore = (await chat.harness.snapshot(UsageDoc, chat.root.id, BACKGROUND_CONTEXT))!.models["faux/faux-1"]!;
    chat.requests.summaries.push(answer("SUMMARY"));
    const id = await chat.root.compact("focus on files", BACKGROUND_CONTEXT);
    const record = await chat.harness.waitForTask(id, BACKGROUND_CONTEXT);
    const outcome = record.state.outcome;
    const submissionId = outcome.status === "completed" ? outcome.result.submissionId : undefined;
    const placed = await (await chat.harness.submission(submissionId, BACKGROUND_CONTEXT))!.wait(BACKGROUND_CONTEXT);
    const after = await entriesOf(chat.root);
    const marker = after.at(-1)!;
    const view = await chat.root.context(BACKGROUND_CONTEXT);
    const request = chat.requests.summaryRequests[0]!;
    const usageAfter = (await chat.harness.snapshot(UsageDoc, chat.root.id, BACKGROUND_CONTEXT))!.models["faux/faux-1"]!;
    const expected = {
        outcome: outcome.status,
        placed: { status: placed.status, entryMatchesMarker: placed.entry === marker.id },
        rawHistoryPreserved: after.slice(0, before.length).map((entry: any) => [entry.kind, entryText(entry)]),
        marker: {
            kind: marker.kind,
            reason: marker.data?.reason,
            headKind: before.find((entry: any) => entry.id === marker.head)?.kind,
            text: entryText(marker),
        },
        context: {
            roles: view.messages.map((message: any) => message.role),
            texts: view.messages.map(userText),
        },
        summaryRequest: {
            messages: request.messages.length,
            firstPromptStartsWith: userText(request.messages[1]).slice(0, 20),
            containsA2: userText(request.messages[1]).includes("[Assistant]: a2"),
            excludesU3: !userText(request.messages[1]).includes("u3 "),
            endsWithFocus: userText(request.messages[1]).endsWith("\n\nAdditional focus: focus on files"),
            options: {
                cacheRetention: request.options?.cacheRetention,
                maxTokens: request.options?.maxTokens,
                sessionIdPresent: typeof request.options?.sessionId === "string",
                deferred: request.options?.deferred ?? null,
            },
        },
        usageIncreased: usageAfter.input > usageBefore.input,
    };
    await closeChat(chat);
    return {
        name: "manual compaction summarizes old context, keeps raw history, and places one summary marker at the cut",
        source: "packages/durable/src/harness/compaction.ts:CompactionTask;packages/durable/src/harness/harness.ts:ConversationImpl.compact",
        inputs: {
            turns: ["u1/a1", "u2/a2", "u3/a3"],
            tokensPerMessage: 100,
            instructions: "focus on files",
            modelWindow: 100000,
            compaction: MANUAL,
        },
        expected,
    };
}

export async function thresholdCompaction(): Promise<Scenario> {
    const chat = await openCompaction(1000, BLOCKING);
    await history(chat);
    chat.requests.summaries.push(answer("SUMMARY"));
    chat.requests.agent.push(answer("a4"));
    const submission = await chat.root.submit({ type: "input", content: text("u4", 200) }, BACKGROUND_CONTEXT);
    const settled = await submission.wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const inspection = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const request = chat.requests.agentRequests.at(-1)!;
    const expected = {
        settled: settled.status,
        tailKinds: entries.slice(-3).map((entry: any) => entry.kind),
        compactionTask: inspection.tasks
            .map((task: any) => task.record)
            .filter((record: any) => record.kind === "pi.compaction")
            .map((record: any) => record.state.status),
        marker: entries
            .filter((entry: any) => entry.kind === "pi.compaction")
            .map((entry: any) => ({ reason: entry.data?.reason, text: entryText(entry) })),
        request: {
            firstUserText: userText(request.messages.find((message: any) => message.role === "user")),
            systemSections: request.messages
                .filter((message: any) => message.role === "system")
                .map((message: any) => Object.keys(message.sections ?? {})),
        },
    };
    await closeChat(chat);
    return {
        name: "a blocking threshold compaction appends its summary before the request it protects",
        source: "packages/durable/src/harness/compaction.ts:CompactionTask,#prepare;packages/durable/src/harness/generation.ts:#prepare",
        inputs: {
            turns: ["u1/a1", "u2/a2", "u3/a3", "u4/a4"],
            tokensPerMessage: [100, 100, 100, 200],
            modelWindow: 1000,
            compaction: BLOCKING,
        },
        expected,
    };
}

export async function compactionDecline(): Promise<Scenario> {
    const chat = await openCompaction(1000, BLOCKING);
    chat.registry.install(
        defineExtension({
            name: "decline-compaction",
            hooks: [hook(CompactionTask, { beforeCompact: () => ({ decline: true }) })],
        }),
    );
    await history(chat);
    chat.requests.agent.push(answer("a4"));
    const submission = await chat.root.submit({ type: "input", content: text("u4", 200) }, BACKGROUND_CONTEXT);
    const settled = await submission.wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const request = chat.requests.agentRequests.at(-1)!;
    const expected = {
        settled: settled.status,
        markers: entries.filter((entry: any) => entry.kind === "pi.compaction").length,
        firstUserText: userText(request.messages.find((message: any) => message.role === "user")),
        summaryRequests: chat.requests.summaryRequests.length,
    };
    await closeChat(chat);
    return {
        name: "a compaction beforeCompact hook can decline the cut and the request goes out uncompacted",
        source: "packages/durable/src/harness/compaction.ts:CompactionTask.beforeCompact;packages/durable/src/harness/types.ts:CompactionHooks",
        inputs: {
            compaction: BLOCKING,
            hook: "beforeCompact -> {decline:true}",
            extraTurnTokens: 200,
        },
        expected,
    };
}

export async function nothingToCompact(): Promise<Scenario> {
    const chat = await openCompaction(100_000, MANUAL);
    await turn(chat, "hello", "hi");
    const id = await chat.root.compact(undefined, BACKGROUND_CONTEXT);
    const record = await chat.harness.waitForTask(id, BACKGROUND_CONTEXT);
    const expected = {
        outcome: record.state.outcome,
        markers: (await entriesOf(chat.root)).filter((entry: any) => entry.kind === "pi.compaction").length,
        summaryRequests: chat.requests.summaryRequests.length,
    };
    await closeChat(chat);
    return {
        name: "manual compaction with no eligible cut completes without a summary or a marker",
        source: "packages/durable/src/harness/compaction.ts:#selectCut,CompactionTask",
        inputs: { turn: ["hello", "hi"], instructions: null, compaction: MANUAL },
        expected,
    };
}

export async function allCompactionScenarios(): Promise<Scenario[]> {
    return [await manualCompaction(), await thresholdCompaction(), await compactionDecline(), await nothingToCompact()];
}
