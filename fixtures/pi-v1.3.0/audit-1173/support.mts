/**
 * Shared helpers for `audit-1173`: open a public Harness over a scripted faux model and read back
 * committed entries/state and the public event stream. No private scheduler or storage state is
 * touched.
 */

import {
    BACKGROUND_CONTEXT,
    Harness,
    MemoryStorage,
    createModels,
    createRegistry,
    fauxAssistantMessage,
    fauxProvider,
    fauxToolCall,
} from "./pi.mts";

export function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

export async function waitUntil(
    check: () => boolean | Promise<boolean>,
    label: string,
    timeoutMs = 5000,
): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!(await check())) {
        if (Date.now() > deadline) throw new Error(`Condition was not reached: ${label}`);
        await new Promise((resolve) => setTimeout(resolve, 2));
    }
}

export type Deferred = { readonly promise: Promise<void>; resolve: () => void };

export function deferred(): Deferred {
    let resolve!: () => void;
    const promise = new Promise<void>((done) => {
        resolve = done;
    });
    return { promise, resolve };
}

export function answer(text: string) {
    return fauxAssistantMessage([{ type: "text", text }]);
}

export function toolCall(name: string, args: Record<string, unknown> = {}, id = "c1") {
    return fauxAssistantMessage([{ type: "toolCall", id, name, arguments: args }], { stopReason: "toolUse" });
}

export type Chat = {
    readonly harness: any;
    readonly root: any;
    readonly faux: any;
    readonly models: any;
    readonly registry: any;
};

export type OpenChatOptions = {
    readonly storage?: any;
    readonly registry?: any;
    readonly settings?: Record<string, unknown>;
    readonly reports?: unknown[];
};

/** Open a Harness over `storage` and create/read the root with the faux model on first creation. */
export async function openChat(options: OpenChatOptions = {}): Promise<Chat> {
    const faux = fauxProvider({});
    const models = createModels();
    models.setProvider(faux.provider);
    const registry = options.registry ?? createRegistry();
    const harness = await Harness.open(
        options.storage ?? new MemoryStorage(),
        {
            models,
            registry,
            ...(options.settings === undefined ? {} : { settings: options.settings }),
            ...(options.reports === undefined
                ? {}
                : { onReport: (error: unknown) => options.reports!.push(error) }),
        },
        BACKGROUND_CONTEXT,
    );
    const root = await harness.root(BACKGROUND_CONTEXT, {
        agent: { model: { provider: "faux", modelId: "faux-1" } },
    });
    return { harness, root, faux, models, registry };
}

/** Raw committed entries of one conversation, oldest first. */
export async function entriesOf(conversation: any): Promise<any[]> {
    const page = await conversation.entries({}, 1000, undefined, BACKGROUND_CONTEXT);
    return [...page.items].reverse();
}

export async function closeChat(chat: Chat): Promise<void> {
    await chat.harness.close(BACKGROUND_CONTEXT);
}

/** The stable event fields of one frozen `AgentEvent`, reduced for comparison. */
export function summarize(event: any): Record<string, unknown> {
    const summary: Record<string, unknown> = { type: event.type };
    for (const key of ["toolCallId", "toolName", "parentToolCallId"]) {
        if (event[key] !== undefined) summary[key] = event[key];
    }
    if (event.taskId !== undefined) summary.hasTaskId = true;
    if (event.parentTaskId !== undefined) summary.hasParentTaskId = true;
    if (event.args !== undefined) summary.args = event.args;
    if (event.output !== undefined) summary.output = event.output;
    if (event.details !== undefined) summary.details = event.details;
    if (event.diagnostics !== undefined) summary.diagnostics = event.diagnostics;
    if (event.entry !== undefined) summary.entryKind = event.entry.kind;
    if (event.result !== undefined) summary.resultKeys = Object.keys(event.result).sort();
    if (event.message !== undefined) {
        summary.role = event.message.role;
        if (event.message.role === "toolResult") summary.contentLength = event.message.content.length;
        else if (event.message.role === "system") summary.sections = Object.keys(event.message.sections ?? {});
        else summary.text = textOf(event.message) ?? "";
    }
    if (event.inputs !== undefined) summary.inputsCount = event.inputs.length;
    if (event.changes !== undefined) summary.changeTypes = event.changes.map((change: any) => change.type);
    if (event.usage !== undefined) summary.hasUsage = true;
    if (event.record !== undefined) summary.submission = { type: event.record.type, status: event.record.status };
    if (event.agent !== undefined) summary.agentKeys = Object.keys(event.agent).sort();
    if (event.items !== undefined) summary.items = event.items.length;
    if (event.type === "task_failed") {
        summary.kind = event.kind;
        summary.message = event.message;
    }
    if (event.error !== undefined) summary.error = event.error;
    if (event.errorMessage !== undefined) summary.errorMessage = event.errorMessage;
    if (event.attempt !== undefined) summary.attempt = event.attempt;
    if (event.blocking !== undefined) summary.blocking = event.blocking;
    if (event.reason !== undefined) summary.reason = event.reason;
    return summary;
}

export function textOf(message: any): string | undefined {
    if (message === undefined || message === null) return undefined;
    if (typeof message.content === "string") return message.content;
    if (!Array.isArray(message.content)) return undefined;
    const text = message.content.find((content: any) => content.type === "text");
    return text?.type === "text" ? text.text : undefined;
}

/** Snapshot fields a stream reports at attachment, reduced for comparison. */
export function snapshotSummary(snapshot: any): Record<string, unknown> {
    return {
        type: snapshot.type,
        entries: snapshot.entries.length,
        runInputs: snapshot.run?.inputs.length ?? 0,
        generation: snapshot.generation === undefined ? null : { hasMessage: snapshot.generation.message !== undefined },
        tools: (snapshot.tools ?? []).map((slot: any) => ({ callId: slot.callId, name: slot.name, status: slot.status })),
        nestedTools: (snapshot.nestedTools ?? []).length,
        compactions: (snapshot.compactions ?? []).length,
        inbox: snapshot.inbox.length,
        agentKeys: Object.keys(snapshot.agent ?? {}).sort(),
        usageKeys: Object.keys(snapshot.usage ?? {}).sort(),
    };
}

/** The batch boundaries as event-type lists: frozen frames stay per-commit. */
export function batchTypes(batches: readonly (readonly any[])[]): string[][] {
    return batches.map((batch) => batch.map((event) => event.type));
}

/** Fold consecutive batches that hold only tool updates into one, as the C++ writer commits each report. */
export function collapseBatchTypes(batches: string[][]): (string[] | string)[][] {
    const folded: (string[] | string)[][] = [];
    let inUpdate = false;
    for (const batch of batches) {
        const onlyUpdates = batch.length > 0 && batch.every((type) => type === "tool_execution_update");
        if (onlyUpdates) {
            if (!inUpdate) folded.push(["tool_execution_update"]);
            inUpdate = true;
            continue;
        }
        inUpdate = false;
        folded.push(batch);
    }
    return folded;
}

/** The flattened event summaries of every delivered batch. */
export function flatten(batches: readonly (readonly any[])[]): any[] {
    return batches.flat();
}

/** Start a stream and collect its deliveries. */
export function collect(stream: any): any[][] {
    const batches: any[][] = [];
    stream.start(async (events: any) => {
        batches.push([...events]);
    });
    return batches;
}

/** Let the frozen watch drain its pending frames (microtasks and the listener's promise). */
export async function drain(): Promise<void> {
    for (let index = 0; index < 8; index++) await new Promise((resolve) => setTimeout(resolve, 0));
}

/** A settled submission as a comparable record, without IDs. */
export function settlement(value: any): Record<string, unknown> {
    return value.status === "done"
        ? { status: "done" }
        : { status: value.status, reason: value.reason ?? null };
}

export async function settleSubmission(submission: Promise<any>): Promise<Record<string, unknown>> {
    return settlement(await (await submission).wait(BACKGROUND_CONTEXT));
}
