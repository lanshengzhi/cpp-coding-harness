/**
 * `audit-1056` code mode and tool search: QuickJS script execution with nested real tools,
 * conversation-scoped store, failure reporting, and deferred-tool loading.
 */

import {
    BACKGROUND_CONTEXT,
    AgentDoc,
    CodemodeSandbox,
    CodemodeStoreDoc,
    MemoryStorage,
    Type,
    createCodemodeExtension,
    createToolSearchExtension,
    defineExtension,
    defineTool,
} from "./pi.mts";
import { answer, closeChat, entriesOf, openChat, textOf, toolCall, toolResults, type Scenario } from "./support.mts";

function script(code: string, id = "s1") {
    return toolCall("codemode", { code }, id);
}

function normalizedCodemodeText(text: string): string {
    return text.replace(/Wall time [\d.]+ seconds/, "Wall time <seconds>");
}

function resultText(result: any): string {
    return result.content.map((item: any) => item.text ?? `<${item.type}>`).join("");
}

export async function codemodeScripts(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    chat.registry.install(
        defineExtension({
            name: "codemode-audit-tool",
            tools: [
                defineTool({
                    name: "double",
                    description: "Doubles a number",
                    parameters: Type.Object({ n: Type.Number() }),
                    execute: async ({ n }: any) => ({ output: [{ type: "text", text: String(n * 2) }] }),
                }),
            ],
        }),
    );
    chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox() }));
    chat.faux.setResponses([
        script(`const doubled = await tools.double({ n: 21 }); store("answer", doubled); return doubled;`),
        answer("first"),
        script(`return load("answer");`, "s2"),
        answer("second"),
    ]);
    await (await chat.root.submit({ type: "input", content: "run code" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    await (await chat.root.submit({ type: "input", content: "read store" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const results = toolResults(await entriesOf(chat.root));
    const store = await chat.harness.snapshot(CodemodeStoreDoc, chat.root.id, BACKGROUND_CONTEXT);
    const expected = {
        calls: results.map((result: any) => ({
            isError: result.isError,
            text: normalizedCodemodeText(resultText(result)),
            details: {
                calls: result.details?.calls?.map((call: any) => ({
                    id: call.id,
                    name: call.name,
                    args: call.args,
                    status: call.status,
                    durationPresent: typeof call.durationMs === "number",
                })),
                fullOutputPathPresent: result.details?.fullOutputPath !== undefined,
            },
        })),
        store,
    };
    await closeChat(chat);
    return {
        name: "code mode runs a script that calls a real nested tool and keeps its store across turns",
        source: "packages/durable/src/codemode/codemode.ts:createCodemodeTool,CodemodeStoreDoc;packages/codemode/src/runtime/host.ts:CodemodeSandbox",
        inputs: { scripts: ["double 21 then store", "load the stored answer"], sandbox: "CodemodeSandbox QuickJS" },
        expected,
        normalizations: ["wall time replaced by <seconds>"],
    };
}

export async function codemodeFailure(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    chat.registry.install(
        defineExtension({
            name: "codemode-failure-tool",
            tools: [
                defineTool({
                    name: "double",
                    description: "Doubles a number",
                    parameters: Type.Object({ n: Type.Number() }),
                    execute: async ({ n }: any) => ({ output: [{ type: "text", text: String(n * 2) }] }),
                }),
            ],
        }),
    );
    chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox() }));
    chat.faux.setResponses([
        script(`await tools.double({ n: 2 }); throw new Error("boom");`),
        answer("failed"),
    ]);
    await (await chat.root.submit({ type: "input", content: "fail" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const result = toolResults(await entriesOf(chat.root))[0];
    const expected = {
        isError: result.isError,
        text: normalizedCodemodeText(resultText(result)),
        details: {
            calls: result.details?.calls?.map((call: any) => ({
                id: call.id,
                name: call.name,
                args: call.args,
                status: call.status,
                durationPresent: typeof call.durationMs === "number",
            })),
            fullOutputPathPresent: result.details?.fullOutputPath !== undefined,
        },
    };
    await closeChat(chat);
    return {
        name: "a failed code mode script reports its error and the nested calls already made",
        source: "packages/durable/src/codemode/codemode.ts:CodemodeCall.execute;packages/durable/src/codemode/output.ts:formatResult",
        inputs: { script: "call double then throw boom" },
        expected,
        normalizations: ["wall time replaced by <seconds>"],
    };
}

export async function toolSearchLoads(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    chat.registry.install(
        defineExtension({
            name: "search-tools",
            tools: [
                defineTool({
                    name: "alpha",
                    description: "Alpha tool",
                    parameters: Type.Object({}),
                    execute: async () => ({ output: [{ type: "text", text: "alpha ran" }] }),
                }),
                defineTool({
                    name: "github_get_issue",
                    description: "Get a GitHub issue by number",
                    parameters: Type.Object({ number: Type.Optional(Type.Number()) }),
                    deferred: true,
                    execute: async () => ({ output: [{ type: "text", text: "github_get_issue ran" }] }),
                }),
                defineTool({
                    name: "github_list_repos",
                    description: "List the repositories of a user",
                    parameters: Type.Object({ user: Type.Optional(Type.String()) }),
                    deferred: true,
                    execute: async () => ({ output: [{ type: "text", text: "github_list_repos ran" }] }),
                }),
            ],
        }),
    );
    chat.registry.install(createToolSearchExtension());
    const before = await chat.root.agent(BACKGROUND_CONTEXT);
    chat.faux.setResponses([
        toolCall("tool_search", { query: "github issue", limit: 1 }, "t1"),
        toolCall("github_get_issue", { number: 42 }, "t2"),
        answer("done"),
    ]);
    await (await chat.root.submit({ type: "input", content: "find a tool" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const after = await chat.root.agent(BACKGROUND_CONTEXT);
    const expected = {
        offeredBefore: before.tools.map((tool: any) => tool.name),
        deferredBefore: before.deferred.map((tool: any) => tool.name),
        searchResult: resultText(toolResults(entries)[0]),
        toolResult: resultText(toolResults(entries)[1]),
        offeredAfter: after.tools.map((tool: any) => tool.name),
        deferredAfter: after.deferred.map((tool: any) => tool.name),
        modelTools: (await chat.harness.snapshot(AgentDoc, chat.root.id, BACKGROUND_CONTEXT))?.modelTools,
        systemToolsAdded: entries
            .filter((entry: any) => entry.kind === "pi.system")
            .map((entry: any) => ({
                added: entry.model[0].toolsAdded?.map((tool: any) => tool.name) ?? [],
                removed: entry.model[0].toolsRemoved?.map((tool: any) => tool.name) ?? [],
            })),
    };
    await closeChat(chat);
    return {
        name: "tool_search ranks a deferred tool, loads it through control.addTools, and the next call runs it",
        source: "packages/durable/src/tool-search/tool-search.ts:createToolSearchTool;packages/durable/src/harness/tool.ts:ToolTask",
        inputs: { query: "github issue", limit: 1, deferred: ["github_get_issue", "github_list_repos"] },
        expected,
    };
}

export async function toolSearchNoMatch(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    chat.registry.install(
        defineExtension({
            name: "search-tools-empty",
            tools: [
                defineTool({
                    name: "github_get_issue",
                    description: "Get a GitHub issue by number",
                    parameters: Type.Object({}),
                    deferred: true,
                    execute: async () => ({ output: [] }),
                }),
            ],
        }),
    );
    chat.registry.install(createToolSearchExtension());
    chat.faux.setResponses([toolCall("tool_search", { query: "weather forecast" }, "t1"), answer("done")]);
    await (await chat.root.submit({ type: "input", content: "find weather" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const expected = {
        searchResult: resultText(toolResults(entries)[0]),
        modelTools: (await chat.harness.snapshot(AgentDoc, chat.root.id, BACKGROUND_CONTEXT))?.modelTools ?? null,
        offered: (await chat.root.agent(BACKGROUND_CONTEXT)).tools.map((tool: any) => tool.name),
        assistantText: textOf(entries.at(-1)?.model?.[0]),
    };
    await closeChat(chat);
    return {
        name: "an unrelated tool_search query loads nothing and leaves the offered set unchanged",
        source: "packages/durable/src/tool-search/tool-search.ts:createToolSearchTool",
        inputs: { query: "weather forecast", deferred: ["github_get_issue"] },
        expected,
    };
}

export async function allCodemodeToolSearchScenarios(): Promise<Scenario[]> {
    return [await codemodeScripts(), await codemodeFailure(), await toolSearchLoads(), await toolSearchNoMatch()];
}
