/**
 * `audit-1056` generation recovery and durable storage formats: a retryable provider error and the
 * JSONL storage round trip.
 */

import { mkdtempSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import {
    BACKGROUND_CONTEXT,
    Harness,
    MemoryStorage,
    createModels,
    createRegistry,
    fauxAssistantMessage,
    fauxProvider,
    fauxText,
    openNodeJsonlStorage,
} from "./pi.mts";
import { answer, closeChat, entriesOf, openChat, textOf, transcript, type Scenario } from "./support.mts";

export async function retryPolicy(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage(), {
        settings: { retry: { enabled: true, maxRetries: 2, baseDelayMs: 1, maxAgentDelayMs: 1000 } },
    });
    chat.faux.setResponses([
        fauxAssistantMessage([fauxText("")], { stopReason: "error", errorMessage: "rate limit exceeded" }),
        answer("recovered"),
    ]);
    const settled = await (
        await chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT)
    ).wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const expected = {
        status: settled.status,
        callCount: chat.faux.state.callCount,
        entries: transcript(entries),
        assistant: entries
            .filter((entry: any) => entry.kind === "pi.assistant")
            .map((entry: any) => ({
                stopReason: entry.model[0].stopReason,
                errorMessage: entry.model[0].errorMessage ?? null,
                text: textOf(entry.model[0]) ?? "",
            })),
    };
    await closeChat(chat);
    return {
        name: "a retryable provider error is appended and the same run retries to a completed answer",
        source: "packages/durable/src/harness/generation.ts:GenerationTask.#response;packages/durable/src/harness/agent.ts:DEFAULT_RETRY_POLICY",
        inputs: {
            responses: ["rate limit exceeded", "recovered"],
            retry: { enabled: true, maxRetries: 2, baseDelayMs: 1, maxAgentDelayMs: 1000 },
        },
        expected,
    };
}

export async function jsonlStorage(): Promise<Scenario> {
    const directory = mkdtempSync(path.join(tmpdir(), "pi-1056-jsonl-"));
    try {
        const first = await openChat(await openNodeJsonlStorage(directory, BACKGROUND_CONTEXT));
        first.faux.setResponses([answer("jsonl answer")]);
        await (await first.root.submit({ type: "input", content: "jsonl question" }, BACKGROUND_CONTEXT)).wait(
            BACKGROUND_CONTEXT,
        );
        const before = transcript(await entriesOf(first.root));
        await closeChat(first);
        const files = readdirSync(directory).sort();

        const faux = fauxProvider({});
        const models = createModels();
        models.setProvider(faux.provider);
        const reopened = await Harness.open(
            await openNodeJsonlStorage(directory, BACKGROUND_CONTEXT),
            { models, registry: createRegistry() },
            BACKGROUND_CONTEXT,
        );
        const root = await reopened.root(BACKGROUND_CONTEXT);
        const expected = {
            files,
            before,
            after: transcript(await entriesOf(root)),
            rootId: root.id,
        };
        await reopened.close(BACKGROUND_CONTEXT);
        return {
            name: "JSONL storage writes its files and reopens the same conversation and transcript",
            source: "packages/durable/src/storage/jsonl/node.ts:openNodeJsonlStorage;packages/durable/src/storage/jsonl/storage.ts:JsonlStorage",
            inputs: { storage: "node:jsonl directory", turn: ["jsonl question", "jsonl answer"], reopen: true },
            expected,
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

export async function allGenerationStorageScenarios(): Promise<Scenario[]> {
    return [await retryPolicy(), await jsonlStorage()];
}
