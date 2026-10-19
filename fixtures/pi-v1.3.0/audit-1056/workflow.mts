/**
 * `audit-1056` public Harness workflow: admission, scripted generation, a real local injected tool,
 * committed entries and final caller-visible settlement, plus the registry/hook transitions that show
 * the extension integration is live rather than a startup-only list.
 */

import {
    BACKGROUND_CONTEXT,
    GenerationTask,
    MemoryStorage,
    ToolTask,
    createRegistry,
    defineExtension,
    defineTool,
    hook,
    section,
    Type,
} from "./pi.mts";
import {
    answer,
    assert,
    closeChat,
    entriesOf,
    entryText,
    openChat,
    toolCall,
    toolResults,
    transcript,
    type Scenario,
} from "./support.mts";

export async function publicWorkflow(): Promise<Scenario> {
    const executions: Array<{ text: string }> = [];
    const registry = createRegistry();
    const echo = defineTool({
        name: "echo",
        description: "Echo the text argument back",
        parameters: Type.Object({ text: Type.String() }),
        execute: async ({ text }: { text: string }) => {
            executions.push({ text });
            return { output: [{ type: "text", text: `echo:${text}` }] };
        },
    });
    registry.install(
        defineExtension({
            name: "audit-tools",
            tools: [echo],
            sections: [section("audit-note", () => "workflow under audit", { tag: true })],
        }),
    );
    const chat = await openChat(new MemoryStorage(), { registry });
    chat.faux.setResponses([toolCall("echo", { text: "hello" }, "call-1"), answer("finished")]);
    const submission = await chat.root.submit({ type: "input", content: "run the echo tool" }, BACKGROUND_CONTEXT);
    const settled = await submission.wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const result = toolResults(entries)[0];
    const expected = {
        admission: {
            submissionIdIsSettledId: settled.id === submission.id,
            conversationIsRoot: settled.conversationId === chat.root.id,
            requestId: settled.requestId ?? null,
        },
        settlement: {
            status: settled.status,
            entryIsUser: entries.find((entry: any) => entry.id === settled.entry)?.kind,
            answerIsAssistant: entries.find((entry: any) => entry.id === settled.answer)?.kind,
        },
        entries: transcript(entries),
        tool: {
            executions,
            callId: result?.toolCallId,
            toolName: result?.toolName,
            text: result?.content?.map((item: any) => item.text).join("|"),
            isError: result?.isError,
            durationPresent: result?.durationMs !== undefined,
            resultTaskPresent: entries.find((entry: any) => entry.kind === "pi.tool-result")?.byTaskId !== undefined,
        },
        offeredTools: (await chat.root.agent(BACKGROUND_CONTEXT)).tools.map((tool: any) => tool.name),
    };
    await closeChat(chat);
    return {
        name: "a scripted model drives admission, one real injected tool, committed entries and final settlement",
        source: "packages/durable/src/harness/harness.ts:Harness.open;packages/durable/src/harness/generation.ts:GenerationTask;packages/durable/src/harness/tool.ts:ToolTask",
        inputs: {
            storage: "MemoryStorage",
            model: "faux/faux-1",
            turns: ["toolUse echo {text:hello}", "stop finished"],
            tool: "echo",
            section: "audit-note",
        },
        expected,
    };
}

export async function registryTransition(): Promise<Scenario> {
    const registry = createRegistry();
    const first = defineTool({
        name: "first",
        description: "The first tool",
        parameters: Type.Object({}),
        execute: async () => ({ output: [{ type: "text", text: "first ran" }] }),
    });
    registry.install(defineExtension({ name: "first-extension", tools: [first] }));
    const chat = await openChat(new MemoryStorage(), { registry });
    const before = (await chat.root.agent(BACKGROUND_CONTEXT)).tools.map((tool: any) => tool.name);
    const second = defineTool({
        name: "second",
        description: "The second tool",
        parameters: Type.Object({}),
        execute: async () => ({ output: [{ type: "text", text: "second ran" }] }),
    });
    registry.install(
        defineExtension({
            name: "second-extension",
            tools: [second],
            sections: [section("late-section", () => "registered after creation")],
        }),
    );
    const afterInstall = (await chat.root.agent(BACKGROUND_CONTEXT)).tools.map((tool: any) => tool.name);
    chat.faux.setResponses([answer("seen")]);
    await (await chat.root.submit({ type: "input", content: "what tools do you have" }, BACKGROUND_CONTEXT)).wait(
        BACKGROUND_CONTEXT,
    );
    const system = (await entriesOf(chat.root)).find((entry: any) => entry.kind === "pi.system")!.model![0];
    const expected = {
        before,
        afterInstall,
        sectionsAdded: Object.keys(system.sections ?? {}),
        toolsAdded: (system.toolsAdded ?? []).map((tool: any) => tool.name),
    };
    await closeChat(chat);
    return {
        name: "a registry install after conversation creation changes the next request's tools and prompt sections",
        source: "packages/durable/src/harness/registry.ts:Registry.install;packages/durable/src/harness/agent.ts:resolveAgent",
        inputs: {
            installedBeforeCreation: ["first"],
            installedAfterCreation: ["second", "late-section"],
            requests: 1,
        },
        expected,
    };
}

export async function hookContinuation(): Promise<Scenario> {
    const registry = createRegistry();
    const yields: string[] = [];
    registry.install(
        defineExtension({
            name: "audit-hooks",
            hooks: [
                hook(GenerationTask, {
                    onYield: (message: any) => {
                        const text = message.content?.find((content: any) => content.type === "text")?.text ?? "";
                        yields.push(text);
                        return text === "first answer"
                            ? { continue: "go on" }
                            : undefined;
                    },
                }),
            ],
        }),
    );
    const chat = await openChat(new MemoryStorage(), { registry });
    chat.faux.setResponses([answer("first answer"), answer("second answer")]);
    const settled = await (
        await chat.root.submit({ type: "input", content: "start" }, BACKGROUND_CONTEXT)
    ).wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const expected = {
        status: settled.status,
        yields,
        callCount: chat.faux.state.callCount,
        entries: transcript(entries),
    };
    await closeChat(chat);
    return {
        name: "a generation onYield hook appends a user message and continues the same admitted run",
        source: "packages/durable/src/harness/generation.ts:GenerationTask.onYield;packages/durable/src/harness/types.ts:GenerationHooks",
        inputs: {
            hook: "GenerationTask.onYield",
            firstAnswer: "first answer",
            continuesWith: "go on",
            scriptedAnswers: ["first answer", "second answer"],
        },
        expected,
    };
}

export async function toolHook(): Promise<Scenario> {
    const registry = createRegistry();
    const seen: Array<Record<string, unknown>> = [];
    registry.install(
        defineExtension({
            name: "audit-tool",
            tools: [
                defineTool({
                    name: "echo",
                    description: "Echo the text argument back",
                    parameters: Type.Object({ text: Type.String() }),
                    execute: async ({ text }: { text: string }) => ({
                        output: [{ type: "text", text: `echo:${text}` }],
                    }),
                }),
            ],
            hooks: [
                hook(ToolTask, {
                    beforeTool: (call: any) => {
                        seen.push({ phase: "beforeTool", id: call.id, arguments: call.arguments });
                        return { arguments: { text: "rewritten" } };
                    },
                    afterTool: (call: any, result: any) => {
                        seen.push({
                            phase: "afterTool",
                            id: call.id,
                            text: result.output?.[0]?.text,
                        });
                        return result;
                    },
                }),
            ],
        }),
    );
    const chat = await openChat(new MemoryStorage(), { registry });
    chat.faux.setResponses([toolCall("echo", { text: "original" }, "call-1"), answer("done")]);
    await (await chat.root.submit({ type: "input", content: "echo" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const result = toolResults(await entriesOf(chat.root))[0];
    const expected = {
        hooks: seen,
        resultText: result?.content?.map((item: any) => item.text).join("|"),
        isError: result?.isError,
    };
    await closeChat(chat);
    return {
        name: "tool hooks rewrite arguments before execution and observe the committed result",
        source: "packages/durable/src/harness/tool.ts:ToolTask;packages/durable/src/harness/types.ts:ToolHooks",
        inputs: {
            call: { name: "echo", arguments: { text: "original" } },
            beforeTool: "replace arguments with {text:'rewritten'}",
            afterTool: "observe result",
        },
        expected,
    };
}

export async function allWorkflowScenarios(): Promise<Scenario[]> {
    const scenarios = [await publicWorkflow(), await registryTransition(), await hookContinuation(), await toolHook()];
    assert(scenarios.length === 4, "workflow scenario count");
    assert(entryText({ model: [{ role: "user", content: "x" }] }) === "x", "text helper");
    return scenarios;
}
