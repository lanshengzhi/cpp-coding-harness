/**
 * `audit-1173` scenarios: the durable public agent event stream (`watchEvents`) and its lifecycle
 * ordering. Every expectation is produced by the frozen pi v1.3.0 public caller; nothing here reads
 * private scheduler or storage state.
 */

import {
    AgentDoc,
    BACKGROUND_CONTEXT,
    ConversationBusy,
    MemoryStorage,
    Type,
    defineExtension,
    defineTask,
    defineTool,
    fauxAssistantMessage,
    fauxToolCall,
    watchEvents,
} from "./pi.mts";
import {
    answer,
    textOf,
    assert,
    batchTypes,
    closeChat,
    collapseBatchTypes,
    collect,
    deferred,
    drain,
    entriesOf,
    flatten,
    openChat,
    settleSubmission,
    snapshotSummary,
    summarize,
    toolCall,
    waitUntil,
    type Scenario,
} from "./support.mts";

const STREAM_SOURCE =
    "packages/durable/src/harness/events.ts:watchEvents,AgentEvent;packages/durable/src/harness/harness.ts:ConversationImpl.id";
const VIEW_SOURCE = "packages/durable/src/harness/view.ts:ConversationViews;packages/durable/src/session/observation.ts:CommittedWatch";

/**
 * Fold each run of consecutive `tool_execution_update` events into one comparable progress summary:
 * the accumulated running output and the last details. Frozen progress commits are paced and
 * coalesced; the C++ writer commits each report, so the update partition is not the comparison
 * unit, the reported progress is.
 */
function collapseUpdates(events: readonly any[]): any[] {
    const collapsed: any[] = [];
    let run: { toolCallId: string; toolName: string; hasTaskId: boolean; text: string; details?: any } | undefined;
    const flush = () => {
        if (run === undefined) return;
        collapsed.push({
            type: "tool_execution_update",
            toolCallId: run.toolCallId,
            toolName: run.toolName,
            hasTaskId: run.hasTaskId,
            outputText: run.text,
            ...(run.details === undefined ? {} : { details: run.details }),
        });
        run = undefined;
    };
    for (const event of events) {
        if (event.type !== "tool_execution_update") {
            flush();
            collapsed.push(summarize(event));
            continue;
        }
        if (run === undefined || run.toolCallId !== event.toolCallId) {
            flush();
            run = { toolCallId: event.toolCallId, toolName: event.toolName, hasTaskId: event.taskId !== undefined, text: "" };
        }
        const output = event.output;
        if (output !== undefined) {
            if ("set" in output) run.text = output.set;
            else {
                if (output.trimStart !== undefined) run.text = run.text.slice(output.trimStart);
                if (output.append !== undefined) run.text += output.append;
            }
        }
        if (event.details !== undefined) run.details = event.details;
    }
    flush();
    return collapsed;
}

/** A tool that reports two output chunks, each followed by a details commit, then returns. */
function printingTool() {
    return defineTool({
        name: "print",
        description: "Prints",
        parameters: Type.Object({ n: Type.Number() }),
        execute: async (_args: any, api: any, context: any) => {
            api.output("one\n");
            await api.details({ step: 1 }, context);
            api.output("two\n");
            await api.details({ step: 2 }, context);
            return {};
        },
    });
}

/**
 * A tool that reports one chunk (and optionally its details), holds until
 * `gate`, then returns. The cancellation scenario reports output only: a
 * details report that the frozen progress pacing publishes after the abort is
 * not part of the compared progress.
 */
function gatedPrintTool(gate: Promise<void>, withDetails = true) {
    return defineTool({
        name: "print",
        description: "Prints",
        parameters: Type.Object({ n: Type.Number() }),
        execute: async (_args: any, api: any, context: any) => {
            api.output("one\n");
            if (withDetails) await api.details({ step: 1 }, context);
            await gate;
            api.output("two\n");
            return {};
        },
    });
}

/** A tool that throws. */
function failingTool() {
    return defineTool({
        name: "print",
        description: "Prints",
        parameters: Type.Object({ n: Type.Number() }),
        execute: async () => {
            throw new Error("boom");
        },
    });
}

/**
 * One model/tool round: message start/end, usage, tool start/update/end with its result entry, the
 * turn handover, and the settled submission. Batch boundaries are per commit.
 */
export async function normalRound(): Promise<Scenario> {
    const chat = await openChat();
    chat.registry.install(defineExtension({ name: "event-tool", tools: [printingTool()] }));
    chat.faux.setResponses([toolCall("print", { n: 1 }, "c1"), answer("done")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches = collect(stream);
    const attached = snapshotSummary(stream.snapshot);
    const submission = chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT);
    const settled = await settleSubmission(submission);
    await drain();
    const events = flatten(batches).map(summarize);
    const types = batchTypes(batches);
    const liveTypes = types.flat();
    const toolStart = events.find((event: any) => event.type === "tool_execution_start");
    const toolEnd = events.find((event: any) => event.type === "tool_execution_end");
    const toolEndIndex = events.findIndex((event: any) => event.type === "tool_execution_end");
    const afterToolEnd = events.slice(toolEndIndex + 1, toolEndIndex + 3).map((event: any) => event.type);
    const rawEvents = flatten(batches);
    const runStart = rawEvents.find((event: any) => event.type === "run_start");
    const runEnd = rawEvents.find((event: any) => event.type === "run_end");
    const placed = rawEvents.find((event: any) => event.type === "submission" && event.record.status === "placed");
    const ended = rawEvents.find((event: any) => event.type === "submission" && event.record.status === "done");
    const toolEndRaw = rawEvents.find((event: any) => event.type === "tool_execution_end");
    const resultEntryEnd = rawEvents.filter((event: any) => event.type === "message_end").find(
        (event: any) => event.entry.kind === "pi.tool-result",
    );
    const entries = await entriesOf(chat.root);
    const end = await stream.stop();
    await closeChat(chat);
    return {
        name: "a model/tool round reports message, tool, usage and settlement lifecycle in committed order",
        source: STREAM_SOURCE,
        inputs: { turn: ["go", "print tool then done"], tool: "print", observation: "batches + flattened events" },
        expected: {
            snapshot: attached,
            batches: types,
            events,
            counts: {
                run_start: liveTypes.filter((type: string) => type === "run_start").length,
                run_end: liveTypes.filter((type: string) => type === "run_end").length,
                turn_start: liveTypes.filter((type: string) => type === "turn_start").length,
                turn_end: liveTypes.filter((type: string) => type === "turn_end").length,
                submission: liveTypes.filter((type: string) => type === "submission").length,
                usage_changed: liveTypes.filter((type: string) => type === "usage_changed").length,
                entry_appended: liveTypes.filter((type: string) => type === "entry_appended").length,
            },
            toolStart,
            toolEnd,
            afterToolEnd,
            collapsedEvents: collapseUpdates(rawEvents),
            linkage: {
                placedSubmissionIsRunInput: placed !== undefined && runStart !== undefined &&
                    runStart.inputs.length === 1 && runStart.inputs[0] === placed.record.id,
                runEndInputsMatchRunStart: runEnd !== undefined && runStart !== undefined &&
                    JSON.stringify(runEnd.inputs) === JSON.stringify(runStart.inputs),
                settledSubmissionIsRunInput: ended !== undefined && placed !== undefined &&
                    ended.record.id === placed.record.id,
                toolEndEntryIsResultMessage: toolEndRaw?.entry !== undefined &&
                    resultEntryEnd !== undefined && toolEndRaw.entry.id === resultEntryEnd.entry.id,
            },
            settled,
            entryKinds: entries.map((entry: any) => entry.kind),
            stop: end,
        },
    };
}

/**
 * A tool held by a gate: the slot stays running, a subscriber attached during the hold sees the
 * committed state and receives no past batches, and both subscribers see the remaining commits.
 */
export async function heldTool(): Promise<Scenario> {
    const gate = deferred();
    const chat = await openChat();
    chat.registry.install(defineExtension({ name: "event-tool", tools: [gatedPrintTool(gate.promise)] }));
    chat.faux.setResponses([toolCall("print", { n: 1 }, "c1"), answer("done")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches = collect(stream);
    const submission = chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT);
    await waitUntil(
        () => flatten(batches).some((event: any) => event.type === "tool_execution_update"),
        "the held tool reports its committed progress",
    );
    const during = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const heldSnapshot = snapshotSummary(during.snapshot);
    const lateBatches = collect(during);
    await drain();
    const lateDuringHold = batchTypes(lateBatches);
    const heldBatches = batchTypes(batches);
    const inspection = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const heldTasks = inspection.tasks.map((task: any) => ({
        kind: task.record.kind,
        state: task.state.kind,
    }));
    gate.resolve();
    const settled = await settleSubmission(submission);
    await waitUntil(() => batches.length > heldBatches.length, "the released hold commits further batches");
    await drain();
    const afterRelease = batchTypes(batches);
    await stream.stop();
    const lateEnd = await during.stop();
    await closeChat(chat);
    return {
        name: "a tool held by a gate keeps its slot running and a later subscriber sees committed state only",
        source: STREAM_SOURCE,
        inputs: { tool: "print (gated after one output chunk)", subscribers: "before submit and during the hold" },
        expected: {
            heldSnapshot,
            heldTasks,
            heldBatches,
            heldBatchesCollapsed: collapseBatchTypes(heldBatches),
            lateDuringHold,
            afterRelease,
            afterReleaseCollapsed: collapseBatchTypes(afterRelease),
            settled,
            lateEnd,
        },
    };
}

/**
 * A tool held at `completing` by live owned work: its generation stays waiting, so the turn stays
 * open until the held child settles; the held work is the discriminator, not a timer.
 */
export async function heldToolCompleting(): Promise<Scenario> {
    const Child = defineTask({
        name: "audit.hold-child",
        version: 1,
        initial: () => ({ phase: "run" }),
        phases: {
            run: async (_task: any, runtime: any, context: any) => {
                await runtime.commit(
                    () => ({ status: "terminal", outcome: { status: "completed", result: null } }),
                    context,
                );
            },
        },
        abort: async () => {},
    });
    const spawn = defineTool({
        name: "spawn",
        description: "Spawns a held task",
        parameters: Type.Object({}),
        execute: async (_args: any, api: any, context: any) => {
            await api.createTask(Child, null, { ownership: { kind: "task", taskId: api.taskId } }, context);
            return {};
        },
    });
    const chat = await openChat();
    // The child's definition is installed only after the hold is observed, so the child stays
    // blocked and cannot settle: the tool's completed outcome is held at `completing`.
    const tasks = defineExtension({ name: "hold-tasks", tasks: [Child] });
    chat.registry.install(defineExtension({ name: "event-tool", tools: [spawn] }));
    chat.faux.setResponses([toolCall("spawn", {}, "c1"), answer("done")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches = collect(stream);
    const submission = chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT);
    await waitUntil(
        () => flatten(batches).some((event: any) => event.type === "tool_execution_end"),
        "the tool's result commits",
    );
    await drain();
    const beforeRelease = batchTypes(batches);
    const held = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const heldTasks = held.tasks.map((task: any) => ({
        kind: task.record.kind,
        state: task.state.kind,
    }));
    chat.registry.install(tasks);
    const settled = await settleSubmission(submission);
    await waitUntil(() => batches.length > beforeRelease.length, "the released hold commits further batches");
    await drain();
    const afterRelease = batchTypes(batches);
    const events = flatten(batches).map(summarize);
    const after = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const afterTasks = after.tasks.map((task: any) => ({
        kind: task.record.kind,
        state: task.state.kind,
    }));
    await stream.stop();
    await closeChat(chat);
    return {
        name: "a tool held at completing by live owned work keeps its turn open until that work settles",
        source: STREAM_SOURCE,
        inputs: { tool: "spawn (creates one blocked owned task and returns)", release: "install the child definition" },
        expected: {
            beforeRelease,
            heldTasks,
            settled,
            afterRelease,
            afterTasks,
            turnEndedWhileHeld: beforeRelease.flat().includes("turn_end"),
            turnEnds: events.filter((event: any) => event.type === "turn_end").length,
            turnStarts: events.filter((event: any) => event.type === "turn_start").length,
            events,
        },
    };
}

/**
 * Cancelling the run while its tool is held: the input settles unanswered/aborted, the tool ends
 * with an aborted result entry, and the same Session serves a later submission.
 */
export async function cancellation(): Promise<Scenario> {
    const gate = deferred();
    const chat = await openChat();
    chat.registry.install(defineExtension({ name: "event-tool", tools: [gatedPrintTool(gate.promise)] }));
    chat.faux.setResponses([toolCall("print", { n: 1 }, "c1"), answer("done"), answer("after abort")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches = collect(stream);
    const submission = chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT);
    await waitUntil(
        () => flatten(batches).some((event: any) => event.type === "tool_execution_update"),
        "the tool reports its committed progress",
    );
    await chat.root.abort(BACKGROUND_CONTEXT);
    gate.resolve();
    const settled = await settleSubmission(submission);
    await drain();
    const cancelledBatches = batchTypes(batches);
    const events = flatten(batches).map(summarize);
    // A details report the frozen pacing publishes only after the abort is not
    // part of the compared progress.
    const collapsedEvents = collapseUpdates(flatten(batches)).map((event: any) => {
        const copy: any = { ...event };
        delete copy.details;
        return copy;
    });
    const toolEnd = events.find((event: any) => event.type === "tool_execution_end");
    const abortedInspect = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const abortedTasks = abortedInspect.tasks.map((task: any) => ({
        kind: task.record.kind,
        state: task.state.kind,
    }));
    const secondSettled = await settleSubmission(
        chat.root.submit({ type: "input", content: "again" }, BACKGROUND_CONTEXT),
    );
    const entries = await entriesOf(chat.root);
    await drain();
    const afterSecond = batchTypes(batches);
    await stream.stop();
    await closeChat(chat);
    return {
        name: "cancelling a run during its tool settles the input unanswered and ends the tool with a result",
        source: STREAM_SOURCE,
        inputs: { tool: "print (gated)", abort: "Conversation.abort during the hold" },
        expected: {
            cancelledBatches,
            events,
            collapsedEvents,
            toolEnd,
            toolEndBeforeSettlement: cancelledBatches.flat().indexOf("tool_execution_end") <
                cancelledBatches.flat().indexOf("run_end"),
            settled,
            abortedTasks,
            secondSettled,
            afterSecond,
            entryKinds: entries.map((entry: any) => entry.kind),
        },
    };
}

/** A retryable provider error: retry start and end around the recovered answer. */
export async function retryPath(): Promise<Scenario> {
    const chat = await openChat({
        settings: { retry: { enabled: true, maxRetries: 1, baseDelayMs: 1, maxAgentDelayMs: 50 } },
    });
    chat.faux.setResponses([
        fauxAssistantMessage([], { stopReason: "error", errorMessage: "rate limit exceeded" }),
        answer("recovered"),
    ]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches = collect(stream);
    const settled = await settleSubmission(chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT));
    await drain();
    const events = flatten(batches).map(summarize);
    await stream.stop();
    await closeChat(chat);
    return {
        name: "a retryable provider error reports retry start and end around the recovered answer",
        source: STREAM_SOURCE,
        inputs: { responses: ["rate limit exceeded", "recovered"], retry: { maxRetries: 1, baseDelayMs: 1 } },
        expected: {
            batches: batchTypes(batches),
            events,
            settled,
            retryStartBeforeRetryEnd:
                events.findIndex((event: any) => event.type === "auto_retry_start") <
                events.findIndex((event: any) => event.type === "auto_retry_end"),
        },
    };
}

/** A terminal provider error: the input settles unanswered and the turn still ends. */
export async function modelFailure(): Promise<Scenario> {
    const chat = await openChat();
    chat.faux.setResponses([
        fauxAssistantMessage([], { stopReason: "error", errorMessage: "provider exploded" }),
        answer("after failure"),
    ]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches = collect(stream);
    const settled = await settleSubmission(chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT));
    await drain();
    const failedBatches = batchTypes(batches);
    const events = flatten(batches).map(summarize);
    const afterInspection = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const afterTasks = afterInspection.tasks.map((task: any) => ({
        kind: task.record.kind,
        state: task.state.kind,
    }));
    const secondSettled = await settleSubmission(
        chat.root.submit({ type: "input", content: "again" }, BACKGROUND_CONTEXT),
    );
    await drain();
    const afterSecond = batchTypes(batches);
    await stream.stop();
    await closeChat(chat);
    return {
        name: "a terminal provider error settles the input unanswered and ends the turn without a remaining task",
        source: STREAM_SOURCE,
        inputs: { responses: ["provider error"] },
        expected: {
            failedBatches,
            events,
            settled,
            afterTasks,
            secondSettled,
            afterSecond,
        },
    };
}

/** A tool that throws: its result lands as an error entry and the round continues. */
export async function toolFailure(): Promise<Scenario> {
    const chat = await openChat();
    chat.registry.install(defineExtension({ name: "event-tool", tools: [failingTool()] }));
    chat.faux.setResponses([toolCall("print", { n: 1 }, "c1"), answer("done")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches = collect(stream);
    const settled = await settleSubmission(chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT));
    await drain();
    const events = flatten(batches).map(summarize);
    const entries = await entriesOf(chat.root);
    const result = entries.find((entry: any) => entry.kind === "pi.tool-result");
    const resultMessage = result?.model?.[0];
    await stream.stop();
    await closeChat(chat);
    return {
        name: "a tool that throws settles an error result on the stream and the round continues",
        source: STREAM_SOURCE,
        inputs: { tool: "print throws Error(boom)" },
        expected: {
            batches: batchTypes(batches),
            events,
            settled,
            toolEnd: events.find((event: any) => event.type === "tool_execution_end"),
            result: resultMessage === undefined
                ? null
                : {
                      toolCallId: resultMessage.toolCallId,
                      toolName: resultMessage.toolName,
                      isError: resultMessage.isError,
                      contentTypes: resultMessage.content.map((block: any) => block.type),
                      text: resultMessage.content.map((block: any) => block.text ?? "").join(""),
                  },
            entryKinds: entries.map((entry: any) => entry.kind),
        },
    };
}

/**
 * Stopping a started stream: no later deliveries, the running work still settles, the acquisition
 * snapshot stays, and a second start is rejected. Stopping an unstarted stream drops its buffer.
 */
export async function subscriberStop(): Promise<Scenario> {
    const gate = deferred();
    const chat = await openChat();
    chat.registry.install(defineExtension({ name: "event-tool", tools: [gatedPrintTool(gate.promise)] }));
    chat.faux.setResponses([toolCall("print", { n: 1 }, "c1"), answer("done")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches = collect(stream);
    const submission = chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT);
    await waitUntil(
        () => flatten(batches).some((event: any) => event.type === "tool_execution_update"),
        "the tool reports its committed progress before the stop",
    );
    const deliveredBeforeStop = batches.length;
    const end = await stream.stop();
    const cancelledReason = (await stream.stop()).reason;
    gate.resolve();
    const settled = await settleSubmission(submission);
    await drain();
    const deliveredAfterStop = batches.length;
    const entries = await entriesOf(chat.root);
    let restartRejected = false;
    try {
        stream.start(async () => {});
    } catch {
        restartRejected = true;
    }

    // An unstarted stream drops its buffered frames when stopped and cannot start afterwards.
    const unstarted = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    await chat.root.commit(async (tx: any) => {
        await tx.appendEntry(chat.root.id, {
            kind: "pi.user",
            model: [{ role: "user", content: "buffered", timestamp: 0 }],
        });
    }, BACKGROUND_CONTEXT);
    const unstartedEnd = await unstarted.stop();
    let unstartedStartRejected = false;
    let unstartedBatches = 0;
    try {
        unstarted.start(async () => {
            unstartedBatches += 1;
        });
    } catch {
        unstartedStartRejected = true;
    }
    await drain();
    await closeChat(chat);
    return {
        name: "stopping a started stream ends its deliveries and leaves persistent work running",
        source: STREAM_SOURCE,
        inputs: { tool: "print (gated)", stop: "during the hold", second: "stop an unstarted stream" },
        expected: {
            deliveredBeforeStop,
            deliveredAfterStop,
            end,
            cancelledReason,
            settled,
            entryKinds: entries.map((entry: any) => entry.kind),
            restartRejected,
            unstartedEnd,
            unstartedStartRejected,
            unstartedBatches,
            stopGain: deliveredAfterStop - deliveredBeforeStop,
        },
    };
}

/** An unstarted stream buffers every committed batch and replays them on start. */
export async function subscriptionTiming(): Promise<Scenario> {
    const chat = await openChat();
    chat.faux.setResponses([answer("done")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const attached = snapshotSummary(stream.snapshot);
    const settled = await settleSubmission(chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT));
    const delayed = collect(stream);
    await drain();
    const delayedBatches = batchTypes(delayed);
    const afterStart = snapshotSummary(stream.snapshot);
    await stream.stop();

    const late = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const lateSnapshot = snapshotSummary(late.snapshot);
    const lateBatches = collect(late);
    await drain();
    const lateDelivered = batchTypes(lateBatches);
    await late.stop();
    await closeChat(chat);
    return {
        name: "an unstarted stream buffers committed batches and replays them when it starts",
        source: STREAM_SOURCE,
        inputs: { attach: "before submit", start: "after the run settles", second: "attach after completion" },
        expected: {
            attached,
            settled,
            delayedBatches,
            afterStart,
            lateSnapshot,
            lateDelivered,
        },
    };
}

/** More than one hundred undelivered batches collapse into one snapshot replacement. */
export async function bufferingOverflow(): Promise<Scenario> {
    const chat = await openChat();
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    for (let index = 0; index < 101; index++) {
        await chat.root.commit(async (tx: any) => {
            await tx.appendEntry(chat.root.id, {
                kind: "pi.user",
                model: [{ role: "user", content: `note ${index}`, timestamp: 0 }],
            });
        }, BACKGROUND_CONTEXT);
    }
    const batches = collect(stream);
    await drain();
    const delivered = batches.map((batch) => batch.map((event) => event.type));
    const replacement = batches[0]?.[0];
    const replacementSummary =
        replacement === undefined
            ? null
            : {
                  type: replacement.type,
                  entries: replacement.entries.length,
                  lastText: replacement.entries.at(-1)?.model?.[0]?.content ?? null,
              };
    await stream.stop();

    // A short buffer replays every batch in order.
    const short = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    for (let index = 0; index < 3; index++) {
        await chat.root.commit(async (tx: any) => {
            await tx.appendEntry(chat.root.id, {
                kind: "pi.user",
                model: [{ role: "user", content: `short ${index}`, timestamp: 0 }],
            });
        }, BACKGROUND_CONTEXT);
    }
    const shortBatches = collect(short);
    await drain();
    const shortDelivered = shortBatches.map((batch) => batch.map((event) => event.type));
    const shortEntries = shortBatches.flat().flatMap((event: any) => {
        const message = event.entry?.model?.[0];
        return message === undefined ? [] : [typeof message.content === "string" ? message.content : textOf(message) ?? ""];
    });
    await short.stop();
    await closeChat(chat);
    return {
        name: "more than one hundred undelivered batches collapse into one snapshot replacement",
        source: STREAM_SOURCE,
        inputs: { commits: 101, shortCommits: 3 },
        expected: {
            delivered,
            replacement: replacementSummary,
            shortDelivered,
            shortEntries,
        },
    };
}

/** Closing the Harness ends a live stream with `session_closed`. */
export async function closeTerminates(): Promise<Scenario> {
    const gate = deferred();
    const chat = await openChat();
    chat.registry.install(defineExtension({ name: "event-tool", tools: [gatedPrintTool(gate.promise)] }));
    chat.faux.setResponses([toolCall("print", { n: 1 }, "c1"), answer("done")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const batches = collect(stream);
    chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT);
    await waitUntil(
        () => flatten(batches).some((event: any) => event.type === "tool_execution_update"),
        "the tool reports its committed progress before the close",
    );
    const beforeClose = batches.length;
    const closed = stream.closed;
    await chat.harness.close(BACKGROUND_CONTEXT);
    gate.resolve();
    const end = await closed;
    await drain();
    const afterClose = batches.length;
    const stopAfterClose = JSON.stringify(await stream.stop());
    return {
        name: "closing the Harness ends a live event stream with session_closed",
        source: STREAM_SOURCE,
        inputs: { tool: "print (gated)", close: "Harness.close during the hold" },
        expected: {
            end,
            beforeClose,
            afterClose,
            stopAfterClose,
        },
    };
}

/** A listener failure ends its stream; the run and other subscribers keep going. */
export async function listenerFailure(): Promise<Scenario> {
    const chat = await openChat();
    chat.faux.setResponses([answer("done"), answer("second")]);
    const stream = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const attempts: string[][] = [];
    stream.start(async (events: any) => {
        attempts.push(events.map((event: any) => event.type));
        throw new Error("listener exploded");
    });
    const settled = await settleSubmission(chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT));
    await drain();
    const end = await stream.closed;
    const entries = await entriesOf(chat.root);
    let restartRejected = false;
    try {
        stream.start(async () => {});
    } catch {
        restartRejected = true;
    }
    // A second subscriber is independent of the failed one.
    const second = await watchEvents(chat.harness, chat.root.id, BACKGROUND_CONTEXT);
    const secondBatches = collect(second);
    const secondSettled = await settleSubmission(
        chat.root.submit({ type: "input", content: "again" }, BACKGROUND_CONTEXT),
    );
    await drain();
    const secondEnd = await second.stop();
    await closeChat(chat);
    return {
        name: "a listener failure ends its stream and leaves the run and other subscribers running",
        source: STREAM_SOURCE,
        inputs: { listener: "throws on the first batch", second: "another subscriber after the failure" },
        expected: {
            attempts,
            end: { reason: end.reason },
            endError: String(end.error ?? ""),
            settled,
            entryKinds: entries.map((entry: any) => entry.kind),
            restartRejected,
            secondBatches: batchTypes(secondBatches),
            secondSettled,
            secondEnd,
            secondAttempts: secondBatches.length,
        },
    };
}

/** An aborted attachment context cancels the stream, never the run. */
export async function stopTokenCancellation(): Promise<Scenario> {
    const chat = await openChat();
    chat.faux.setResponses([answer("done")]);
    const controller = new AbortController();
    const context = { ...BACKGROUND_CONTEXT, abortSignal: controller.signal };
    const stream = await watchEvents(chat.harness, chat.root.id, context);
    const batches = collect(stream);
    const submission = chat.root.submit({ type: "input", content: "go" }, BACKGROUND_CONTEXT);
    await waitUntil(
        () => batches.flat().some((event: any) => event.type === "run_end"),
        "the stream delivered the run's whole lifecycle",
    );
    controller.abort(new Error("subscriber cancelled"));
    const end = await stream.closed;
    const atAbort = batches.length;
    const settled = await settleSubmission(submission);
    await drain();
    const afterRun = batches.length;
    const entries = await entriesOf(chat.root);
    const preAborted = new AbortController();
    preAborted.abort(new Error("pre-aborted"));
    let attachError = "none";
    try {
        await watchEvents(chat.harness, chat.root.id, { ...BACKGROUND_CONTEXT, abortSignal: preAborted.signal });
    } catch (error) {
        attachError = String(error);
    }
    await closeChat(chat);
    return {
        name: "an aborted attachment context cancels its stream and never the run",
        source: STREAM_SOURCE,
        inputs: { attach: "before submit", abort: "after the first batch", second: "attach with an aborted context" },
        expected: {
            end: { reason: end.reason },
            endError: String(end.error ?? ""),
            atAbort,
            afterRun,
            settled,
            entryKinds: entries.map((entry: any) => entry.kind),
            attachError,
        },
    };
}

export async function allEventScenarios(): Promise<Scenario[]> {
    const ordered: [string, () => Promise<Scenario>][] = [
        ["normalRound", normalRound],
        ["heldTool", heldTool],
        ["heldToolCompleting", heldToolCompleting],
        ["cancellation", cancellation],
        ["retryPath", retryPath],
        ["modelFailure", modelFailure],
        ["toolFailure", toolFailure],
        ["subscriberStop", subscriberStop],
        ["subscriptionTiming", subscriptionTiming],
        ["bufferingOverflow", bufferingOverflow],
        ["closeTerminates", closeTerminates],
        ["listenerFailure", listenerFailure],
        ["stopTokenCancellation", stopTokenCancellation],
    ];
    const scenarios: Scenario[] = [];
    for (const [name, capture] of ordered) {
        console.error(`START ${name}`);
        scenarios.push(await capture());
        console.error(`DONE ${name}`);
    }
    return scenarios;
}

export { AgentDoc, ConversationBusy, MemoryStorage, fauxToolCall };
