/**
 * `audit-1056` observation and inspection: derived inspection states, the task graph, conversation
 * view/delta frames, the agent event stream, and context-as-of-entry.
 */

import {
    BACKGROUND_CONTEXT,
    LiveDoc,
    MemoryStorage,
    ToolResultEntry,
    createRegistry,
    defineExtension,
    defineTask,
    defineTool,
    Type,
    watchEvents,
} from "./pi.mts";
import {
    answer,
    closeChat,
    deferred,
    entriesOf,
    openChat,
    toolCall,
    transcript,
    waitUntil,
    type Scenario,
} from "./support.mts";

type Step = { phase: "run" };

function oneStep(name: string, gate?: Promise<void>) {
    return defineTask<null, Step, null>({
        name,
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                await gate;
                await runtime.commit(() => ({ status: "terminal", outcome: { status: "completed", result: null } }), context);
            },
        },
        abort: async () => {},
    });
}

export async function inspectStates(): Promise<Scenario> {
    const gate = deferred();
    const Gate = oneStep("audit.inspect-gate", gate.promise);
    type WaitState = { phase: "wait" } | { phase: "done" };
    let gateId: any;
    const Dependent = defineTask<null, WaitState, null>({
        name: "audit.inspect-dependent",
        version: 1,
        initial: () => ({ phase: "wait" }),
        phases: {
            wait: async (_task: any, runtime: any, context: any) =>
                runtime.commit(
                    () => ({ status: "waiting", checkpoint: { phase: "done" }, on: [gateId], policy: "allSettled" }),
                    context,
                ),
            done: async (_task: any, runtime: any, context: any) =>
                runtime.commit(() => ({ status: "terminal", outcome: { status: "completed", result: null } }), context),
        },
        abort: async () => {},
    });
    const Missing = oneStep("audit.inspect-missing");
    const chat = await openChat(new MemoryStorage());
    chat.registry.install(defineExtension({ name: "inspect-tasks", tasks: [Gate, Dependent] }));
    chat.registry.install(defineExtension({ name: "missing-task", tasks: [Missing] }));
    const ids = await chat.root.commit(async (tx: any) => {
        const gateTask = await tx.createTask(Gate, null, { ownership: { kind: "conversation" } });
        gateId = gateTask;
        return {
            gate: gateTask,
            dependent: await tx.createTask(Dependent, null, { ownership: { kind: "conversation" } }),
            missing: await tx.createTask(Missing, null, { ownership: { kind: "conversation" } }),
        };
    }, BACKGROUND_CONTEXT);
    chat.registry.uninstall(chat.registry.snapshot().extension("missing-task"));

    const paused = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const stateOf = (inspection: any, id: any) =>
        inspection.tasks.find((entry: any) => entry.record.id === id)?.state;
    const pausedStates = {
        scheduling: paused.scheduling,
        gate: stateOf(paused, ids.gate),
        dependent: stateOf(paused, ids.dependent),
        missing: stateOf(paused, ids.missing),
    };

    chat.harness.resume();
    let running = await chat.harness.inspect(BACKGROUND_CONTEXT);
    await waitUntil(async () => {
        running = await chat.harness.inspect(BACKGROUND_CONTEXT);
        return (
            stateOf(running, ids.gate)?.kind === "running" && stateOf(running, ids.dependent)?.kind === "waiting"
        );
    }, "gate running and dependent waiting");
    const runningStates = {
        scheduling: running.scheduling,
        gate: stateOf(running, ids.gate),
        dependent: (() => {
            const state = stateOf(running, ids.dependent);
            return state?.kind === "waiting"
                ? { kind: state.kind, onLength: state.on.length, onIsGate: state.on.includes(ids.gate) }
                : state;
        })(),
    };
    gate.resolve();
    await chat.harness.waitForTask(ids.dependent, BACKGROUND_CONTEXT);
    const settled = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const expected = {
        paused: pausedStates,
        running: runningStates,
        settled: {
            liveTaskKinds: settled.tasks.map((entry: any) => entry.record.kind),
            missing: stateOf(settled, ids.missing),
        },
    };
    await closeChat(chat);
    return {
        name: "inspect derives ready, running, waiting and blocked task states without running task code",
        source: "packages/durable/src/harness/harness.ts:Harness.inspect;packages/durable/src/harness/scheduler.ts:#inspect",
        inputs: {
            tasks: ["gate", "dependent waiting on gate", "missing definition"],
            pausedBeforeResume: true,
            gateResolvedAfterInspection: true,
        },
        expected,
    };
}

export async function inspectSubmissions(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    let reached!: () => void;
    const reachedPromise = new Promise<void>((resolve) => {
        reached = resolve;
    });
    chat.faux.setResponses([
        (_request: unknown, options: any) =>
            new Promise((_resolve, reject) => {
                reached();
                options!.signal!.addEventListener("abort", () => reject(options!.signal!.reason), { once: true });
            }),
    ]);
    const submission = await chat.root.submit({ type: "input", content: "busy" }, BACKGROUND_CONTEXT);
    await reachedPromise;
    const inspection = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const status = await submission.status(BACKGROUND_CONTEXT);
    const expected = {
        submissions: inspection.submissions.map((record: any) => ({
            idMatches: record.id === submission.id,
            conversationIsRoot: record.conversationId === chat.root.id,
            type: record.type,
            status: record.status,
        })),
        status: { type: status.type, status: status.status },
        tasks: inspection.tasks.map((task: any) => ({ kind: task.record.kind, state: task.state.kind })),
    };
    await chat.root.abort(BACKGROUND_CONTEXT);
    await submission.wait(BACKGROUND_CONTEXT);
    await closeChat(chat);
    return {
        name: "inspect lists the placed submission and the generation task that owns it",
        source: "packages/durable/src/harness/harness.ts:Harness.inspect;packages/durable/src/harness/submissions.ts:Submissions.status",
        inputs: { generation: "unanswered", inspection: "while busy" },
        expected,
    };
}

export async function taskGraph(): Promise<Scenario> {
    const childGate = deferred();
    const Child = defineTask<null, Step, null>({
        name: "audit.graph-child",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                await childGate.promise;
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: null } }),
                    context,
                );
            },
        },
        abort: async () => {},
    });
    type ParentState = { phase: "spawn" } | { phase: "wait" };
    const Parent = defineTask<null, ParentState, null>({
        name: "audit.graph-parent",
        version: 1,
        initial: () => ({ phase: "spawn" }),
        phases: {
            spawn: async (task: any, runtime: any, context: any) => {
                let child: any;
                await runtime.commit(async (tx: any) => {
                    child = await tx.createTask(Child, null, { ownership: { kind: "task", taskId: task.id } });
                    await tx.createConversation({ ownership: { kind: "task", taskId: task.id } });
                    return {
                        status: "waiting",
                        checkpoint: { phase: "wait" } as const,
                        on: [child],
                        policy: "allSettled" as const,
                    };
                }, context);
            },
            wait: async (_task: any, runtime: any, context: any) =>
                runtime.commit(() => ({ status: "terminal", outcome: { status: "completed", result: null } }), context),
        },
        abort: async () => {},
    });
    const chat = await openChat(new MemoryStorage());
    chat.registry.install(defineExtension({ name: "graph-tasks", tasks: [Parent, Child] }));
    const graph = await chat.harness.taskGraph(BACKGROUND_CONTEXT);
    const before = graph.value;
    const parent = await chat.root.commit(
        (tx: any) => tx.createTask(Parent, null, { ownership: { kind: "conversation" } }),
        BACKGROUND_CONTEXT,
    );
    chat.harness.resume();
    let live: any;
    await waitUntil(() => {
        live = graph.value;
        const nodes = Object.values(live.tasks) as any[];
        return (
            nodes.some((node) => node.kind === "audit.graph-parent" && node.state.status === "waiting") &&
            nodes.some((node) => node.kind === "audit.graph-child" && node.state.status === "running")
        );
    }, "parent waiting with a running child");
    const parentNode = live.tasks[String(parent)];
    const childNode = Object.values(live.tasks).find((node: any) => node.kind === "audit.graph-child") as any;
    const expected = {
        before: before.tasks,
        running: {
            parent: {
                status: parentNode.state.status,
                onOwnsChild: parentNode.state.on?.includes(childNode.id),
                conversations: parentNode.conversations.length,
            },
            child: { status: childNode.state.status, ownerIsParent: childNode.owner === parent },
        },
        after: null as unknown,
    };
    childGate.resolve();
    await chat.harness.waitForTask(parent, BACKGROUND_CONTEXT);
    await chat.harness.waitForIdle(BACKGROUND_CONTEXT);
    expected.after = graph.value.tasks;
    graph.dispose();
    await closeChat(chat);
    return {
        name: "the task graph follows owner edges, owned conversations, waits and completion to empty",
        source: "packages/durable/src/harness/task-graph.ts:TaskGraphView;packages/durable/src/harness/harness.ts:Harness.taskGraph",
        inputs: { parent: "creates child and owned conversation, waits allSettled", child: "gated", observation: "state" },
        expected,
    };
}

export async function conversationView(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    chat.faux.setResponses([answer("hello")]);
    const state = await chat.root.viewState(BACKGROUND_CONTEXT);
    const initial = state.value;
    state.dispose();
    const watch = await chat.root.watch(BACKGROUND_CONTEXT);
    const frames: any[] = [];
    watch.start(async (value: any, ops: any) => frames.push({ value, ops }));
    await (await chat.root.submit({ type: "input", content: "hi" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    await new Promise((resolve) => setTimeout(resolve, 0));
    await watch.stop();
    const fresh = await chat.root.viewState(BACKGROUND_CONTEXT);
    const freshValue = fresh.value;
    fresh.dispose();
    const expected = {
        initial: {
            conversation: initial.conversation,
            entries: initial.entries.length,
            docs: Object.keys(initial.docs).sort(),
            live: initial.docs["pi.live"],
            inbox: initial.docs["pi.inbox"],
        },
        frames: frames.length,
        firstFrameOps: frames[0]?.ops.map((op: any) => op[0]),
        final: {
            entries: freshValue.entries.map((entry: any) => [entry.kind, entry.model?.[0]?.content ?? null]),
            docs: Object.keys(freshValue.docs).sort(),
            live: freshValue.docs["pi.live"],
            inbox: freshValue.docs["pi.inbox"],
        },
    };
    await closeChat(chat);
    return {
        name: "conversation watch publishes delta frames whose final view equals a fresh hydration",
        source: "packages/durable/src/harness/view.ts:ConversationViews;packages/durable/src/harness/harness.ts:ConversationImpl.watch",
        inputs: { turn: ["hi", "hello"], observation: "viewState + watch + fresh" },
        expected,
    };
}

export async function eventStream(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    chat.registry.install(
        defineExtension({
            name: "event-tool",
            tools: [
                defineTool({
                    name: "print",
                    description: "Prints",
                    parameters: Type.Object({ n: Type.Number() }),
                    execute: async (_args: any, api: any) => {
                        api.output("one\n");
                        api.output("two\n");
                        return {};
                    },
                }),
            ],
        }),
    );
    chat.faux.setResponses([toolCall("print", { n: 1 }, "c1"), answer("done")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches: any[][] = [];
    stream.start(async (events: any) => batches.push([...events]));
    await (await chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    await new Promise((resolve) => setTimeout(resolve, 0));
    const events = batches.flat();
    const types = events.map((event: any) => event.type);
    const start = events.find((event: any) => event.type === "tool_execution_start");
    const end = events.find((event: any) => event.type === "tool_execution_end");
    const expected = {
        snapshot: {
            type: stream.snapshot.type,
            entries: stream.snapshot.entries.length,
            tools: stream.snapshot.tools.map((tool: any) => tool.name),
            inbox: stream.snapshot.inbox,
        },
        distinctTypes: types.filter((type: string, index: number) => type !== types[index - 1]),
        counts: {
            run_start: types.filter((type: string) => type === "run_start").length,
            run_end: types.filter((type: string) => type === "run_end").length,
            turn_start: types.filter((type: string) => type === "turn_start").length,
            turn_end: types.filter((type: string) => type === "turn_end").length,
            submission: types.filter((type: string) => type === "submission").length,
        },
        toolStart: start === undefined ? null : { toolCallId: start.toolCallId, toolName: start.toolName, args: start.args },
        toolEnd:
            end === undefined
                ? null
                : {
                      toolCallId: end.toolCallId,
                      toolName: end.toolName,
                      entryKind: end.entry?.kind ?? null,
                  },
        endFollowedByResultMessage: (() => {
            const at = events.indexOf(end);
            return at < 0 ? null : events.slice(at + 1, at + 3).map((event: any) => event.type);
        })(),
    };
    await stream.stop();
    await closeChat(chat);
    return {
        name: "the agent event stream reports run, turn, message, tool and submission lifecycle in order",
        source: "packages/durable/src/harness/events.ts:watchEvents,AgentEvent;packages/durable/src/harness/harness.ts:ConversationImpl.id",
        inputs: { turn: ["go", "print tool then done"], tool: "print" },
        expected,
    };
}

export async function contextAtEntry(): Promise<Scenario> {
    const chat = await openChat(new MemoryStorage());
    chat.faux.setResponses([answer("a1"), answer("a2")]);
    await (await chat.root.submit({ type: "input", content: "u1" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    await (await chat.root.submit({ type: "input", content: "u2" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const first = entries[0];
    const view = await chat.root.context(BACKGROUND_CONTEXT, { at: first.id });
    const fork = await chat.root.fork(first.id, { ownership: { kind: "ownerless" } }, BACKGROUND_CONTEXT);
    const expected = {
        atEntry: {
            entries: transcript(view.entries),
            messages: view.messages.map((message: any) => message.content),
            head: view.head ?? null,
        },
        forkStartsWith: transcript(await entriesOf(fork)),
        rootHasBothTurns: (await entriesOf(chat.root)).length === entries.length,
    };
    await closeChat(chat);
    return {
        name: "context with an entry cursor returns the same committed view a fork would start from",
        source: "packages/durable/src/harness/harness.ts:ConversationImpl.context;packages/durable/src/harness/context.ts:readContext",
        inputs: { turns: ["u1", "u2"], at: "first entry" },
        expected,
    };
}

export async function allObservabilityScenarios(): Promise<Scenario[]> {
    return [
        await inspectStates(),
        await inspectSubmissions(),
        await taskGraph(),
        await conversationView(),
        await eventStream(),
        await contextAtEntry(),
    ];
}
