/**
 * Shared helpers for `audit-1056`: open a public Harness over a scripted faux model and read back
 * committed entries/state. No private scheduler or storage state is touched.
 */

import {
    BACKGROUND_CONTEXT,
    Harness,
    createModels,
    createRegistry,
    fauxAssistantMessage,
    fauxProvider,
    fauxText,
    fauxToolCall,
} from "./pi.mts";

export function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

export function errorRecord(error: unknown): Record<string, unknown> {
    return error instanceof Error
        ? { name: error.name, message: error.message }
        : { name: "NonError", message: String(error) };
}

export async function waitUntil(check: () => boolean | Promise<boolean>, label: string, timeoutMs = 5000): Promise<void> {
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
    return fauxAssistantMessage([fauxText(text)]);
}

export function toolCall(name: string, args: Record<string, unknown> = {}, id = "c1") {
    return fauxAssistantMessage([fauxToolCall(name, args, { id })], { stopReason: "toolUse" });
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
    readonly env?: (target: any, context: any) => any;
    readonly now?: () => number;
    readonly reports?: unknown[];
    readonly fauxOptions?: Record<string, unknown>;
    readonly model?: { provider: string; modelId: string };
};

/** Open a Harness over `storage` and create/read the root with the faux model on first creation. */
export async function openChat(storage: any, options: OpenChatOptions = {}): Promise<Chat> {
    const faux = fauxProvider(options.fauxOptions ?? {});
    const models = createModels();
    models.setProvider(faux.provider);
    const registry = options.registry ?? createRegistry();
    const reports = options.reports;
    const harness = await Harness.open(
        storage,
        {
            models,
            registry,
            ...(options.settings === undefined ? {} : { settings: options.settings }),
            ...(options.env === undefined ? {} : { env: options.env }),
            ...(options.now === undefined ? {} : { now: options.now }),
            ...(reports === undefined ? {} : { onReport: (error: unknown) => reports.push(error) }),
        },
        BACKGROUND_CONTEXT,
    );
    const root = await harness.root(BACKGROUND_CONTEXT, {
        agent: { model: options.model ?? { provider: "faux", modelId: "faux-1" } },
    });
    return { harness, root, faux, models, registry };
}

/** Raw committed entries of one conversation, oldest first. */
export async function entriesOf(conversation: any): Promise<any[]> {
    const page = await conversation.entries({}, 1000, undefined, BACKGROUND_CONTEXT);
    return [...page.items].reverse();
}

/** Text of the first text content of a model message. */
export function textOf(message: any): string | undefined {
    if (message === undefined) return undefined;
    if (message.role === "system") return undefined;
    if (typeof message.content === "string") return message.content;
    if (!Array.isArray(message.content)) return undefined;
    const text = message.content.find((content: any) => content.type === "text");
    return text?.type === "text" ? text.text : undefined;
}

export function entryText(entry: any): string | undefined {
    return textOf(entry?.model?.[0]);
}

/** Transcript shape with wall-clock message timestamps dropped: [kind, text|toolName]. */
export function transcript(entries: readonly any[]): unknown[] {
    return entries.map((entry) => {
        const message = entry.model?.[0];
        if (entry.kind === "pi.system") {
            return ["pi.system", Object.keys(message?.sections ?? {}), (message?.toolsAdded ?? []).map((tool: any) => tool.name)];
        }
        if (entry.kind === "pi.tool-result") {
            return [
                "pi.tool-result",
                message?.toolName,
                message?.toolCallId,
                message?.isError,
                message?.durationMs !== undefined,
                Array.isArray(message?.content) ? message.content.map((item: any) => item.text).join("|") : "",
            ];
        }
        return [entry.kind, textOf(message) ?? ""];
    });
}

/** Tool result messages of one transcript. */
export function toolResults(entries: readonly any[]): any[] {
    return entries.filter((entry) => entry.kind === "pi.tool-result").map((entry) => entry.model![0]);
}

export async function closeChat(chat: Chat): Promise<void> {
    await chat.harness.close(BACKGROUND_CONTEXT);
}
