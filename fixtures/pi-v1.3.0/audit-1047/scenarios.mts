/**
 * Frozen pi-v1.3.0 Agent and tool-execution scenarios for ticket #1047.
 *
 * Every observation below comes from a real public call against the private frozen checkout:
 * `Agent`, `runToolCall`, the coding-agent `NestedToolCallRunner` and `SessionManager`. The
 * scenario records are the comparison unit: the expected value names presence/absence, ordering
 * and transitions, while `observed` keeps the raw volatile millisecond readings for provenance.
 */

export type CaptureApi = {
	Agent: any;
	runToolCall: any;
	createAssistantMessageEventStream: any;
};

export type ScenarioArtifact = {
	path: string;
	sourceEndpoints: string[];
	scenarios: any[];
};

const usage = {
	input: 0,
	output: 0,
	cacheRead: 0,
	cacheWrite: 0,
	totalTokens: 0,
	cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
};

const toolSchema: any = {
	type: "object",
	properties: { value: { type: "string" } },
	required: ["value"],
	additionalProperties: false,
};

const sleep = (milliseconds: number) => new Promise<void>((resolve) => setTimeout(resolve, milliseconds));

function expect(condition: boolean, message: string): void {
	if (!condition) throw new Error(`scenario assertion failed: ${message}`);
}

function same(actual: unknown, expected: unknown, message: string): void {
	const actualJson = JSON.stringify(actual);
	const expectedJson = JSON.stringify(expected);
	expect(actualJson === expectedJson, `${message} (actual ${actualJson}, expected ${expectedJson})`);
}

function user(text: string) {
	return { role: "user", content: text, timestamp: Date.now() };
}

function assistant(content: any[], stopReason: any = "stop", errorMessage?: string) {
	return {
		role: "assistant",
		content,
		api: "openai-responses",
		provider: "openai",
		model: "mock",
		usage,
		stopReason,
		...(errorMessage === undefined ? {} : { errorMessage }),
		timestamp: Date.now(),
	};
}

function toolCall(id: string, name: string, args: any) {
	return { type: "toolCall", id, name, arguments: args };
}

function tool(name: string, execute: (args: any, id: string) => Promise<any>, extra: any = {}) {
	return {
		name,
		label: name,
		description: `${name} tool`,
		parameters: toolSchema,
		execute: async (id: string, args: any) => execute(args, id),
		...extra,
	};
}

/// A deterministic scripted provider stream: the handler returns the final assistant message for
/// the request index it receives. This is ordinary stream injection through `StreamFn`.
function scriptedStream(pi: CaptureApi, handler: (index: number, context: any, options: any) => any) {
	let index = 0;
	return (_model: any, context: any, options: any) => {
		const stream = pi.createAssistantMessageEventStream();
		const current = index++;
		queueMicrotask(() => {
			try {
				const message = handler(current, context, options);
				stream.push({ type: "done", reason: message.stopReason, message });
			} catch (error) {
				stream.push({
					type: "error",
					reason: "error",
					error: assistant([], "error", error instanceof Error ? error.message : String(error)),
				});
			}
		});
		return stream;
	};
}

function messageText(message: any): string {
	const content = typeof message.content === "string" ? [{ type: "text", text: message.content }] : message.content;
	return (content ?? [])
		.filter((block: any) => block.type === "text")
		.map((block: any) => block.text)
		.join("\n");
}

/// Deterministic event digest. Volatile millisecond values are deliberately excluded from the
/// expected value and recorded only in `observed`.
function digestEvent(event: any): string {
	switch (event.type) {
		case "message_start":
		case "message_end": {
			const role = event.message.role;
			if (event.type === "message_end" && role === "toolResult") {
				const duration = event.message.durationMs === undefined ? "absent" : "present";
				return `${event.type}:${role}:${event.message.toolCallId}:duration=${duration}`;
			}
			if (event.type === "message_end" && role === "assistant") {
				return `${event.type}:${role}:${event.message.stopReason}`;
			}
			return `${event.type}:${role}`;
		}
		case "message_update":
			return `${event.type}:${event.assistantMessageEvent.type}`;
		case "tool_execution_start":
			return `${event.type}:${event.toolCallId}`;
		case "tool_execution_end":
			return `${event.type}:${event.toolCallId}:duration=${event.durationMs === undefined ? "absent" : "present"}:error=${event.isError}`;
		case "turn_end":
			return `${event.type}:${event.message.role}:tools=${event.toolResults.length}`;
		case "agent_end":
			return `${event.type}:messages=${event.messages.length}`;
		default:
			return event.type;
	}
}

function collect(agent: any): any[] {
	const events: any[] = [];
	agent.subscribe((event: any) => {
		events.push(event);
	});
	return events;
}

function requestRoles(context: any): string[] {
	return context.messages.map((message: any) => message.role);
}

function agentEndMessageCount(events: any[]): number | undefined {
	const agentEnd = events.findLast((event) => event.type === "agent_end");
	return agentEnd?.messages?.length;
}

function lastUserText(context: any): string | undefined {
	for (let index = context.messages.length - 1; index >= 0; index -= 1) {
		if (context.messages[index].role === "user") return messageText(context.messages[index]);
	}
	return undefined;
}

function upstreamTest(path: string, testCase: string) {
	return { path, case: testCase };
}

function resultMap(messages: any[], idKey: string, durationKey: "durationMs"): Record<string, any> {
	const map: Record<string, any> = {};
	for (const message of messages) {
		if (message.role !== "toolResult") continue;
		map[message.toolCallId] = {
			id: message[idKey] ?? message.toolCallId,
			durationPresent: message[durationKey] !== undefined,
			isError: message.isError,
			text: messageText(message),
		};
	}
	return map;
}

export async function captureScenarios(pi: CaptureApi, piCheckout: string): Promise<ScenarioArtifact[]> {
	const artifacts: ScenarioArtifact[] = [];

	// ── lifecycle ──────────────────────────────────────────────────────────────────────────────
	{
		const scenarios: any[] = [];

		{
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, (index) =>
					index === 0
						? assistant([toolCall("echo-1", "echo", { value: "a" })], "toolUse")
						: assistant([{ type: "text", text: "done" }]),
				),
				initialState: {
					tools: [tool("echo", async (args) => ({ content: [{ type: "text", text: args.value }], details: {} }))],
				},
			});
			const events = collect(agent);
			await agent.prompt("go");
			const observed = {
				events: events.map(digestEvent),
				finalMessages: agent.state.messages.map((message: any) => message.role),
				toolResult: resultMap(agent.state.messages, "toolCallId", "durationMs")["echo-1"],
			};
			same(
				observed.events,
				[
					"agent_start",
					"turn_start",
					"message_start:user",
					"message_end:user",
					"message_start:assistant",
					"message_end:assistant:toolUse",
					"tool_execution_start:echo-1",
					"tool_execution_end:echo-1:duration=present:error=false",
					"message_start:toolResult",
					"message_end:toolResult:echo-1:duration=present",
					"turn_end:assistant:tools=1",
					"turn_start",
					"message_start:assistant",
					"message_end:assistant:stop",
					"turn_end:assistant:tools=0",
					"agent_end:messages=4",
				],
				"tool turn and final response lifecycle order",
			);
			expect(observed.toolResult?.text === "a", "tool result text");
			scenarios.push({
				name: "turn and event ordering for a tool turn and a final response",
				inputs: { prompt: "go", tool: "echo", executionMode: "default" },
				expected: {
					events: observed.events,
					finalMessageRoles: ["system", "user", "assistant", "toolResult", "assistant"],
					toolResultText: "a",
				},
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent-loop.test.ts", "should handle tool calls and results"),
			});
		}

		{
			const agent: any = new pi.Agent({
				streamFn: () => {
					const stream = pi.createAssistantMessageEventStream();
					const partial = assistant([], "stop");
					const text = { type: "text", text: "Hi" };
					queueMicrotask(() => {
						stream.push({ type: "start", partial });
						stream.push({ type: "text_start", contentIndex: 0, partial });
						stream.push({ type: "text_delta", contentIndex: 0, delta: "Hi", partial: assistant([text]) });
						stream.push({ type: "text_end", contentIndex: 0, content: "Hi", partial: assistant([text]) });
						stream.push({ type: "done", reason: "stop", message: assistant([text]) });
					});
					return stream;
				},
			});
			const events = collect(agent);
			await agent.prompt("go");
			const observed = {
				events: events.map(digestEvent),
				lastAssistantText: messageText(agent.state.messages.at(-1)),
				streamedUpdates: events
					.filter((event) => event.type === "message_update")
					.map((event) => event.assistantMessageEvent.type),
			};
			same(
				observed.events,
				[
					"agent_start",
					"turn_start",
					"message_start:user",
					"message_end:user",
					"message_start:assistant",
					"message_update:text_start",
					"message_update:text_delta",
					"message_update:text_end",
					"message_end:assistant:stop",
					"turn_end:assistant:tools=0",
					"agent_end:messages=2",
				],
				"ordinary stream injection lifecycle",
			);
			scenarios.push({
				name: "ordinary stream injection drives assistant updates and the final message",
				inputs: { prompt: "go", streamEvents: ["start", "text_start", "text_delta", "text_end", "done"] },
				expected: {
					events: observed.events,
					lastAssistantText: "Hi",
					streamedUpdates: ["text_start", "text_delta", "text_end"],
				},
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should subscribe to events"),
			});
		}

		{
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, () => assistant([{ type: "text", text: "ok" }])),
			});
			const order: string[] = [];
			let streamingAtAgentEnd: boolean | undefined;
			agent.subscribe(async (event: any) => {
				if (event.type !== "agent_end") return;
				streamingAtAgentEnd = agent.state.isStreaming;
				order.push("agent_end:listener_start");
				await sleep(20);
				order.push("agent_end:listener_end");
			});
			const run = agent.prompt("go").then(() => order.push("prompt:resolved"));
			await sleep(5);
			await agent.waitForIdle();
			order.push("waitForIdle:resolved");
			await run;
			const observed = {
				order,
				streamingAtAgentEnd,
				streamingAfterIdle: agent.state.isStreaming,
			};
			same(
				observed.order,
				["agent_end:listener_start", "agent_end:listener_end", "waitForIdle:resolved", "prompt:resolved"],
				"agent_end listener settlement order",
			);
			expect(observed.streamingAtAgentEnd === true, "agent is still streaming while agent_end listeners run");
			expect(observed.streamingAfterIdle === false, "agent is idle after waitForIdle");
			scenarios.push({
				name: "agent_end listeners settle before prompt and waitForIdle resolve",
				inputs: { prompt: "go", agentEndListenerDelayMs: 20 },
				expected: {
					order: observed.order,
					isStreamingDuringAgentEndListener: true,
					isStreamingAfterWaitForIdle: false,
				},
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "waitForIdle should wait for async subscribers"),
			});
		}

		{
			const agent: any = new pi.Agent({ streamFn: () => { throw new Error("unused"); } });
			const observed = {
				thinkingLevel: agent.state.thinkingLevel,
				modelId: agent.state.model.id,
				toolCount: agent.state.tools.length,
				messageCount: agent.state.messages.length,
				steeringMode: agent.steeringMode,
				followUpMode: agent.followUpMode,
				toolExecution: agent.toolExecution,
				transport: agent.transport,
				isStreaming: agent.state.isStreaming,
			};
			same(
				observed,
				{
					thinkingLevel: "off",
					modelId: "unknown",
					toolCount: 0,
					messageCount: 0,
					steeringMode: "one-at-a-time",
					followUpMode: "one-at-a-time",
					toolExecution: "parallel",
					transport: "auto",
					isStreaming: false,
				},
				"Agent default state",
			);
			scenarios.push({
				name: "Agent defaults select off thinking, parallel tools and one-at-a-time queues",
				inputs: { options: { streamFn: "unused" } },
				expected: { ...observed },
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should create an agent instance with default state"),
			});
		}

		{
			const reasoningModel = {
				id: "reasoning-mock",
				name: "reasoning-mock",
				api: "openai-responses",
				provider: "openai",
				baseUrl: "",
				reasoning: true,
				input: [],
				cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
				contextWindow: 8192,
				maxTokens: 1024,
			};
			const agent: any = new pi.Agent({
				streamFn: () => { throw new Error("unused"); },
				initialState: { model: reasoningModel },
			});
			const observed = { thinkingLevel: agent.state.thinkingLevel, modelId: agent.state.model.id };
			same(observed, { thinkingLevel: "off", modelId: "reasoning-mock" }, "default thinking level with a reasoning model");
			scenarios.push({
				name: "the default thinking level remains off for a reasoning-capable model",
				inputs: { model: { id: "reasoning-mock", reasoning: true }, explicitThinkingLevel: false },
				expected: { thinkingLevel: "off", modelId: "reasoning-mock" },
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should create an agent instance with custom initial state"),
			});
		}

		{
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, () => assistant([{ type: "text", text: "ok" }])),
				initialState: { systemPrompt: "sys" },
			});
			await agent.prompt("go");
			agent.steer(user("steer"));
			agent.followUp(user("follow"));
			agent.reset();
			const observed = {
				roles: agent.state.messages.map((message: any) => message.role),
				systemPrompt: agent.state.systemPrompt,
				hasQueuedMessages: agent.hasQueuedMessages(),
				isStreaming: agent.state.isStreaming,
			};
			same(observed, { roles: ["system"], systemPrompt: "sys", hasQueuedMessages: false, isStreaming: false }, "reset");
			scenarios.push({
				name: "reset restores the prompt baseline and clears both queues",
				inputs: { prompt: "go", queuedBeforeReset: ["steer", "follow"] },
				expected: { ...observed },
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "restores the transcript baseline when reset"),
			});
		}

		{
			const calls: any[] = [];
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, () => assistant([{ type: "text", text: "ok" }])),
				prepareRequest: (request: any) => {
					calls.push({
						roles: requestRoles(request.context),
						model: request.model.id,
						thinkingLevel: request.thinkingLevel,
					});
					return undefined;
				},
			});
			await agent.prompt("go");
			const observed = { calls };
			same(observed.calls, [{ roles: ["user"], model: "unknown", thinkingLevel: "off" }], "prepareRequest");
			scenarios.push({
				name: "prepareRequest observes the pending prompt before the provider request",
				inputs: { prompt: "go", prepareRequestReturns: "undefined" },
				expected: { calls: observed.calls },
				normalizations: [],
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"prepares the initial request after pending messages and can replace request state",
				),
			});
		}

		{
			let prepareCalls = 0;
			const requestContexts: any[] = [];
			const provider = scriptedStream(pi, (index) =>
				index === 0
					? assistant([toolCall("echo-1", "echo", { value: "a" })], "toolUse")
					: assistant([{ type: "text", text: "done" }]),
			);
			const agent: any = new pi.Agent({
				streamFn: (_model: any, context: any) => {
					requestContexts.push({ roles: requestRoles(context), lastUser: lastUserText(context) });
					return provider(_model, context, {});
				},
				initialState: {
					tools: [tool("echo", async (args) => ({ content: [{ type: "text", text: args.value }], details: {} }))],
				},
				prepareNextTurn: () => {
					prepareCalls += 1;
					return { messages: [user("prepared")] };
				},
			});
			await agent.prompt("go");
			const observed = {
				prepareCalls,
				requestContexts,
				prepareNextTurnCalls: prepareCalls,
				secondRequestRoles: requestContexts[1].roles,
				secondRequestLastUser: requestContexts[1].lastUser,
			};
			same(observed.prepareCalls, 1, "prepareNextTurn called once");
			same(observed.requestContexts[1].lastUser, "prepared", "prepareNextTurn message reaches the next request");
			same(observed.requestContexts[1].roles, ["system", "user", "assistant", "toolResult", "user"], "prepared message order");
			scenarios.push({
				name: "prepareNextTurn runs once before a continuing turn and can append messages",
				inputs: { tool: "echo", preparedMessage: "prepared" },
				expected: {
					prepareNextTurnCalls: 1,
					secondRequestRoles: observed.requestContexts[1].roles,
					secondRequestLastUser: "prepared",
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"should use prepareNextTurn snapshot before continuing",
				),
			});
		}

		{
			let finishCalls = 0;
			const requestContexts: any[] = [];
			const provider = scriptedStream(pi, (index) =>
				assistant([{ type: "text", text: index === 0 ? "first" : "second" }]),
			);
			const agent: any = new pi.Agent({
				streamFn: (_model: any, context: any) => {
					requestContexts.push({ roles: requestRoles(context) });
					return provider(_model, context, {});
				},
				finishTurn: () => {
					finishCalls += 1;
					return { action: "end" };
				},
			});
			const events = collect(agent);
			agent.followUp(user("follow"));
			await agent.prompt("go");
			const observed = {
				finishCalls,
				finishTurnCalls: finishCalls,
				requestCount: requestContexts.length,
				remaining: agent.hasQueuedMessages(),
				remainingQueued: agent.hasQueuedMessages(),
				events: events.map(digestEvent),
			};
			same(observed.finishCalls, 1, "finishTurn called once");
			same(observed.requestCount, 1, "end decision stops before another request");
			same(observed.remaining, true, "end decision does not poll the follow-up queue");
			scenarios.push({
				name: "finishTurn action end stops the run before follow-up polling",
				inputs: { finishTurnAction: "end", queuedFollowUp: "follow" },
				expected: {
					finishTurnCalls: 1,
					requestCount: 1,
					remainingQueued: true,
					events: observed.events,
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"action:end skips queue polling and next-turn preparation",
				),
			});
		}

		{
			const agent: any = new pi.Agent({
				streamFn: () => {
					const stream = pi.createAssistantMessageEventStream();
					queueMicrotask(() =>
						stream.push({
							type: "error",
							reason: "error",
							error: assistant([], "error", "provider boom"),
						}),
					);
					return stream;
				},
			});
			const events = collect(agent);
			await agent.prompt("go");
			const observed = {
				events: events.map(digestEvent),
				errorMessage: agent.state.errorMessage,
				isStreaming: agent.state.isStreaming,
			};
			same(
				observed.events,
				[
					"agent_start",
					"turn_start",
					"message_start:user",
					"message_end:user",
					"message_start:assistant",
					"message_end:assistant:error",
					"turn_end:assistant:tools=0",
					"agent_end:messages=2",
				],
				"provider error terminal lifecycle",
			);
			same(observed.errorMessage, "provider boom", "provider error state");
			same(observed.isStreaming, false, "idle after provider error");
			scenarios.push({
				name: "a provider error terminal emits one failed assistant lifecycle",
				inputs: { providerEvent: "error", errorMessage: "provider boom" },
				expected: {
					events: observed.events,
					errorMessage: "provider boom",
					isStreaming: false,
				},
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "emits full lifecycle events for thrown run failures"),
			});
		}

		{
			const agent: any = new pi.Agent({
				streamFn: (_model: any, _context: any, options: any) => {
					const stream = pi.createAssistantMessageEventStream();
					const abort = () =>
						stream.push({ type: "error", reason: "aborted", error: assistant([], "aborted", "Request was aborted") });
					if (options.signal?.aborted) abort();
					else options.signal?.addEventListener("abort", abort, { once: true });
					return stream;
				},
			});
			const run = agent.prompt("first");
			await sleep(5);
			let secondPromptError = "";
			try {
				await agent.prompt("second");
			} catch (error) {
				secondPromptError = error instanceof Error ? error.message : String(error);
			}
			agent.abort();
			await run;
			const observed = { secondPromptError };
			same(
				observed.secondPromptError,
				"Agent is already processing a prompt. Use steer() or followUp() to queue messages, or wait for completion.",
				"busy prompt error text",
			);
			scenarios.push({
				name: "a second prompt while a run is active is rejected without replacing the run",
				inputs: { firstPrompt: "first", secondPromptWhileActive: "second" },
				expected: {
					secondPromptError:
						"Agent is already processing a prompt. Use steer() or followUp() to queue messages, or wait for completion.",
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should throw when prompt() called while streaming"),
			});
		}

		artifacts.push({
			path: "lifecycle.json",
			sourceEndpoints: [
				"packages/agent/src/agent.ts:Agent.prompt,Agent.subscribe,Agent.waitForIdle,Agent.reset,Agent.state",
				"packages/agent/src/agent-loop.ts:runAgentLoop,runLoop,streamAssistantResponse",
				"packages/agent/src/types.ts:AgentEvent,AgentOptions,QueueMode",
				"packages/agent/test/agent.test.ts:Agent",
				"packages/agent/test/agent-loop.test.ts:runAgentLoop",
			],
			scenarios,
		});
	}

	// ── tool execution ────────────────────────────────────────────────────────────────────────
	{
		const scenarios: any[] = [];

		{
			const hooks = { before: 0, after: 0 };
			let startedAt: number | undefined;
			let endedAt: number | undefined;
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, (index) =>
					index === 0
						? assistant([toolCall("slow-1", "slow", { value: "a" })], "toolUse")
						: assistant([{ type: "text", text: "done" }]),
				),
				initialState: {
					tools: [
						tool("slow", async (args) => {
							await sleep(10);
							return { content: [{ type: "text", text: args.value }], details: {} };
						}),
					],
				},
				beforeToolCall: async () => {
					hooks.before += 1;
					await sleep(60);
					return undefined;
				},
				afterToolCall: async () => {
					hooks.after += 1;
					await sleep(50);
					return undefined;
				},
			});
			const events = collect(agent);
			agent.subscribe((event: any) => {
				if (event.type === "tool_execution_start") startedAt = Date.now();
				if (event.type === "tool_execution_end") endedAt = Date.now();
			});
			await agent.prompt("go");
			const end = events.find((event) => event.type === "tool_execution_end");
			const durationMs = end?.durationMs;
			const wallClockMs = endedAt! - startedAt!;
			expect(hooks.before === 1 && hooks.after === 1, "both hooks ran once");
			expect(typeof durationMs === "number", "executed call reports durationMs");
			expect(durationMs <= 40, `durationMs ${durationMs} stays near the 10ms execute delay`);
			expect(durationMs < 60, `durationMs ${durationMs} excludes the 60ms before hook`);
			expect(wallClockMs >= 90, `wall-clock start->end ${wallClockMs} covers the 110ms of hooks`);
			expect(durationMs < wallClockMs, "durationMs is not the wall-clock start->end span");
			scenarios.push({
				name: "hook and execute activity are distinguishable in duration and wall time",
				inputs: { tool: "slow", beforeHookDelayMs: 60, executeDelayMs: 10, afterHookDelayMs: 50 },
				expected: {
					hookCalls: 2,
					durationPresent: true,
					durationAtMostExecuteDelayPlusTolerance: true,
					durationLessThanBeforeHookDelay: true,
					wallClockStartToEndIncludesHookTime: true,
					durationLessThanWallClockStartToEnd: true,
				},
				normalizations: [
					"raw durationMs and wall-clock milliseconds are volatile observations; the expected value keeps presence, absence, ordering and threshold relations",
				],
				observed: {
					durationMs,
					wallClockMs,
					hooks,
					hookCalls: hooks.before + hooks.after,
					durationPresent: typeof durationMs === "number",
					durationAtMostExecuteDelayPlusTolerance: typeof durationMs === "number" && durationMs <= 40,
					durationLessThanBeforeHookDelay: typeof durationMs === "number" && durationMs < 60,
					wallClockStartToEndIncludesHookTime: wallClockMs >= 90,
					durationLessThanWallClockStartToEnd: typeof durationMs === "number" && durationMs < wallClockMs,
				},
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"records how long execute() took on the tool result, excluding hooks",
				),
			});
		}

		{
			const executed: Record<string, number> = {};
			const mark = (name: string) => {
				executed[name] = (executed[name] ?? 0) + 1;
			};
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, (index) =>
					index === 0
						? assistant(
							[
								toolCall("ok", "ok", { value: "a" }),
								toolCall("throws", "throws", { value: "b" }),
								toolCall("blocked", "ok", { value: "c" }),
								toolCall("missing", "missing", { value: "d" }),
							],
							"toolUse",
						)
						: assistant([{ type: "text", text: "done" }]),
				),
				toolExecution: "sequential",
				initialState: {
					tools: [
						tool("ok", async (args) => {
							mark("ok");
							return { content: [{ type: "text", text: args.value }], details: {} };
						}),
						tool("throws", async () => {
							mark("throws");
							throw new Error("execute boom");
						}),
					],
				},
				beforeToolCall: async ({ toolCall }: any) =>
					toolCall.id === "blocked" ? { block: true, reason: "denied" } : undefined,
			});
			const events = collect(agent);
			await agent.prompt("go");
			const results = resultMap(agent.state.messages, "toolCallId", "durationMs");
			const observed = {
				events: events.map(digestEvent),
				executed,
				durationPresent: Object.fromEntries(
					Object.entries(results).map(([id, result]: [string, any]) => [id, result.durationPresent]),
				),
				isError: Object.fromEntries(
					Object.entries(results).map(([id, result]: [string, any]) => [id, result.isError]),
				),
				text: Object.fromEntries(
					Object.entries(results).map(([id, result]: [string, any]) => [id, result.text]),
				),
			};
			same(
				observed.events,
				[
					"agent_start",
					"turn_start",
					"message_start:user",
					"message_end:user",
					"message_start:assistant",
					"message_end:assistant:toolUse",
					"tool_execution_start:ok",
					"tool_execution_end:ok:duration=present:error=false",
					"message_start:toolResult",
					"message_end:toolResult:ok:duration=present",
					"tool_execution_start:throws",
					"tool_execution_end:throws:duration=present:error=true",
					"message_start:toolResult",
					"message_end:toolResult:throws:duration=present",
					"tool_execution_start:blocked",
					"tool_execution_end:blocked:duration=absent:error=true",
					"message_start:toolResult",
					"message_end:toolResult:blocked:duration=absent",
					"tool_execution_start:missing",
					"tool_execution_end:missing:duration=absent:error=true",
					"message_start:toolResult",
					"message_end:toolResult:missing:duration=absent",
					"turn_end:assistant:tools=4",
					"turn_start",
					"message_start:assistant",
					"message_end:assistant:stop",
					"turn_end:assistant:tools=0",
					"agent_end:messages=7",
				],
				"success, failure, blocked and unknown call lifecycle",
			);
			same(observed.executed, { ok: 1, throws: 1 }, "only executed tools ran");
			same(observed.durationPresent, { ok: true, throws: true, blocked: false, missing: false }, "duration presence matrix");
			same(observed.isError, { ok: false, throws: true, blocked: true, missing: true }, "error matrix");
			same(observed.text.missing, "Tool missing not found", "unknown-tool text");
			same(observed.text.blocked, "denied", "blocked-tool text");
			scenarios.push({
				name: "executed success, thrown failure, blocked and unknown calls keep distinct duration presence",
				inputs: { toolExecution: "sequential", calls: ["ok", "throws", "blocked", "missing"] },
				expected: {
					events: observed.events,
					executeCalls: { ok: 1, throws: 1 },
					durationPresent: observed.durationPresent,
					isError: observed.isError,
					blockedText: "denied",
					unknownToolText: "Tool missing not found",
				},
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should ignore tool updates after the tool execution settles"),
			});
		}

		{
			let beforeValue: any;
			let executedValue: any;
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, (index) =>
					index === 0
						? assistant([toolCall("prep-1", "prep", { oldText: "before", newText: "after" })], "toolUse")
						: assistant([{ type: "text", text: "done" }]),
				),
				initialState: {
					tools: [
						tool(
							"prep",
							async (args) => {
								executedValue = args.value;
								return { content: [{ type: "text", text: String(args.value) }], details: { value: args.value } };
							},
							{
								prepareArguments: (args: any) => ({ value: args.oldText + args.newText }),
							},
						),
					],
				},
				beforeToolCall: async ({ args }: any) => {
					beforeValue = args.value;
					args.value = 123;
					return undefined;
				},
			});
			await agent.prompt("go");
			const result = resultMap(agent.state.messages, "toolCallId", "durationMs")["prep-1"];
			same(beforeValue, "beforeafter", "prepareArguments ran before the hook");
			same(executedValue, 123, "hook-mutated arguments are executed without revalidation");
			same(typeof executedValue, "number", "mutated argument type survives");
			const observed = {
				beforeValue,
				executedValue,
				executedValueType: typeof executedValue,
				resultText: result.text,
				durationPresent: result.durationPresent,
			};
			scenarios.push({
				name: "prepareArguments runs before validation and beforeToolCall mutations execute without revalidation",
				inputs: { tool: "prep", rawArguments: { oldText: "before", newText: "after" }, hookMutation: 123 },
				expected: {
					beforeValue: "beforeafter",
					executedValue: 123,
					executedValueType: "number",
					resultText: "123",
					durationPresent: true,
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"should execute mutated beforeToolCall args without revalidation",
				),
			});
		}

		{
			let executed = 0;
			const truncatedText =
				'Tool call "echo" was not executed: the response hit the output token limit, so its arguments may be truncated. Re-issue the tool call with complete arguments.';
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, (index) =>
					index === 0
						? assistant([toolCall("trunc", "echo", { value: "x" })], "length")
						: assistant([{ type: "text", text: "done" }]),
				),
				initialState: {
					tools: [
						tool("echo", async (args) => {
							executed += 1;
							return { content: [{ type: "text", text: args.value }], details: {} };
						}),
					],
				},
			});
			const events = collect(agent);
			await agent.prompt("go");
			const result = resultMap(agent.state.messages, "toolCallId", "durationMs")["trunc"];
			const observed = {
				executed,
				events: events.map(digestEvent),
				durationPresent: result.durationPresent,
				isError: result.isError,
				text: result.text,
			};
			expect(observed.executed === 0, "truncated tool call never executes");
			expect(observed.durationPresent === false, "truncated tool call has no duration");
			same(observed.text, truncatedText, "truncated tool call message");
			same(
				observed.events,
				[
					"agent_start",
					"turn_start",
					"message_start:user",
					"message_end:user",
					"message_start:assistant",
					"message_end:assistant:length",
					"tool_execution_start:trunc",
					"tool_execution_end:trunc:duration=absent:error=true",
					"message_start:toolResult",
					"message_end:toolResult:trunc:duration=absent",
					"turn_end:assistant:tools=1",
					"turn_start",
					"message_start:assistant",
					"message_end:assistant:stop",
					"turn_end:assistant:tools=0",
					"agent_end:messages=4",
				],
				"length-truncated lifecycle",
			);
			scenarios.push({
				name: "a call from a length-truncated message never executes and has no duration",
				inputs: { tool: "echo", stopReason: "length" },
				expected: {
					executeCalls: 0,
					durationPresent: false,
					isError: true,
					resultText: truncatedText,
					events: observed.events,
				},
				normalizations: [],
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"should not execute tool calls from a length-truncated assistant message",
				),
			});
		}

		{
			let active = 0;
			let maxActive = 0;
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, (index) =>
					index === 0
						? assistant(
							[
								toolCall("slow", "slow", { value: "a" }),
								toolCall("fast", "fast", { value: "b" }),
							],
							"toolUse",
						)
						: assistant([{ type: "text", text: "done" }]),
				),
				initialState: {
					tools: [
						tool("slow", async (args) => {
							active += 1;
							maxActive = Math.max(maxActive, active);
							await sleep(30);
							active -= 1;
							return { content: [{ type: "text", text: `slow:${args.value}` }], details: {} };
						}),
						tool("fast", async (args) => {
							active += 1;
							maxActive = Math.max(maxActive, active);
							await sleep(1);
							active -= 1;
							return { content: [{ type: "text", text: `fast:${args.value}` }], details: {} };
						}),
					],
				},
			});
			const events = collect(agent);
			await agent.prompt("go");
			const observed = {
				maxActive,
				maxConcurrent: maxActive,
				startOrder: events
					.filter((event) => event.type === "tool_execution_start")
					.map((event) => event.toolName),
				durationPresent: events
					.filter((event) => event.type === "tool_execution_end")
					.map((event) => "durationMs" in event),
				completionOrder: events.filter((event) => event.type === "tool_execution_end").map((event) => event.toolCallId),
				resultOrder: agent.state.messages
					.filter((message: any) => message.role === "toolResult")
					.map((message: any) => message.toolCallId),
				events: events.map(digestEvent),
			};
			same(observed.events.slice(6, 12), [
				"tool_execution_start:slow",
				"tool_execution_start:fast",
				"tool_execution_end:fast:duration=present:error=false",
				"tool_execution_end:slow:duration=present:error=false",
				"message_start:toolResult",
				"message_end:toolResult:slow:duration=present",
			], "parallel starts precede completion-ordered ends and source-ordered results");
			same(observed.maxActive, 2, "omitted per-tool executionMode permits concurrency");
			same(observed.completionOrder, ["fast", "slow"], "completion order");
			same(observed.resultOrder, ["slow", "fast"], "result message source order");
			scenarios.push({
				name: "parallel tool ends follow completion order while result messages keep source order",
				inputs: { calls: [{ name: "slow", delayMs: 30 }, { name: "fast", delayMs: 1 }] },
				expected: {
					startOrder: ["slow", "fast"],
					completionOrder: ["fast", "slow"],
					resultOrder: ["slow", "fast"],
					maxConcurrent: 2,
					durationPresent: [true, true],
				},
				normalizations: [
					"the completion ordering observation relies on the 30ms versus 1ms execute delays, not on exact millisecond values",
				],
				observed,
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"should emit tool_execution_end in completion order but persist tool results in source order",
				),
			});
		}

		{
			let beforeCalls = 0;
			let afterCalls = 0;
			let executeCalls = 0;
			const echo = tool("echo", async (args) => {
				executeCalls += 1;
				await sleep(5);
				return { content: [{ type: "text", text: args.value }], details: {} };
			});
			const assistantMessage = assistant([toolCall("direct-1", "echo", { value: "hi" })], "toolUse");
			const ok = await pi.runToolCall(toolCall("direct-1", "echo", { value: "hi" }), {
				tools: [echo],
				assistantMessage,
				context: { messages: [] },
				beforeToolCall: async () => {
					beforeCalls += 1;
					return undefined;
				},
				afterToolCall: async () => {
					afterCalls += 1;
					return undefined;
				},
			});
			const blocked = await pi.runToolCall(toolCall("direct-2", "echo", { value: "hi" }), {
				tools: [echo],
				assistantMessage,
				context: { messages: [] },
				beforeToolCall: async () => ({ block: true, reason: "no" }),
			});
			const observed = {
				ok: {
					durationPresent: ok.durationMs !== undefined,
					isError: ok.isError,
					beforeCalls,
					afterCalls,
					executeCalls,
					text: messageText(ok.result),
				},
				blocked: {
					durationPresent: blocked.durationMs !== undefined,
					isError: blocked.isError,
					text: messageText(blocked.result),
				},
			};
			same(observed.ok, {
				durationPresent: true,
				isError: false,
				beforeCalls: 1,
				afterCalls: 1,
				executeCalls: 1,
				text: "hi",
			}, "runToolCall success");
			same(observed.blocked, { durationPresent: false, isError: true, text: "no" }, "runToolCall blocked");
			scenarios.push({
				name: "runToolCall runs the hooks and reports execute-only duration",
				inputs: { calls: ["ok", "blocked"] },
				expected: {
					ok: observed.ok,
					blocked: observed.blocked,
				},
				normalizations: [],
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"validates, runs the hooks, and reports failures as error outcomes",
				),
			});
		}

		{
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, (index) =>
					index === 0
						? assistant([toolCall("structured-1", "echo", { value: "x" })], "toolUse")
						: assistant([{ type: "text", text: "done" }]),
				),
				initialState: {
					tools: [
						tool(
							"echo",
							async () => ({
								content: [{ type: "text", text: "ok" }],
								details: {},
								structuredContent: { value: "ok" },
							}),
							{ outputSchema: toolSchema },
						),
					],
				},
				afterToolCall: async () => ({ content: [{ type: "text", text: "replaced" }] }),
			});
			await agent.prompt("go");
			const result = resultMap(agent.state.messages, "toolCallId", "durationMs")["structured-1"];
			const message: any = agent.state.messages.find(
				(candidate: any) => candidate.role === "toolResult" && candidate.toolCallId === "structured-1",
			);
			const observed = {
				hasStructuredContent: message.structuredContent !== undefined,
				content: result.text,
				details: message.details,
				durationPresent: result.durationPresent,
			};
			same(observed, { hasStructuredContent: false, content: "replaced", details: {}, durationPresent: true }, "after hook replacement");
			scenarios.push({
				name: "afterToolCall content replacement drops structuredContent but keeps duration",
				inputs: { tool: "echo", toolStructuredContent: { value: "ok" }, replacement: "content only" },
				expected: {
					hasStructuredContent: false,
					content: "replaced",
					details: {},
					durationPresent: true,
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"lets afterToolCall replace structured content and drops it when only content is replaced",
				),
			});
		}

		{
			const { NestedToolCallRunner } = await import(
				`${piCheckout}/packages/coding-agent/src/core/nested-tool-calls.ts`
			);
			const events: any[] = [];
			const echo = tool("echo", async (args) => {
				await sleep(5);
				return { content: [{ type: "text", text: args.value }], details: {} };
			});
			const assistantMessage = assistant([toolCall("caller", "echo", { value: "a" })], "toolUse");
			const runner = new NestedToolCallRunner({
				getTools: () => [echo],
				isSequential: () => false,
				runToolCall: (call: any, _parentId: string, signal: any) =>
					pi.runToolCall(call, {
						tools: [echo],
						assistantMessage,
						context: { messages: [] },
						signal,
					}),
				emit: async (event: any) => {
					events.push(event);
				},
			});
			const outcome = await runner.execute("caller", "echo", { value: "a" });
			const record = runner.takeRecord("caller");
			const end = events.find((event) => event.type === "tool_execution_end");
			const nested = record?.calls?.calls?.[0];
			const observed = {
				eventTypes: events.map((event) => event.type),
				parentIds: events.map((event) => event.parentToolCallId),
				endDurationPresent: end?.durationMs !== undefined,
				endDurationMatchesOutcome: end?.durationMs === outcome.durationMs,
				recordStatus: nested?.status,
				recordDurationPresent: nested?.durationMs !== undefined,
				recordAtLeastOutcome: (nested?.durationMs ?? -1) >= (outcome.durationMs ?? 0),
			};
			same(observed.eventTypes, ["tool_execution_start", "tool_execution_end"], "nested event sequence");
			same(observed.parentIds, ["caller", "caller"], "parent tool call ids");
			expect(observed.endDurationPresent === true, "nested end event carries execute duration");
			expect(observed.endDurationMatchesOutcome === true, "nested end duration is the runToolCall duration");
			same(observed.recordStatus, "ok", "nested record status");
			expect(observed.recordDurationPresent === true, "nested record duration");
			scenarios.push({
				name: "nested calls forward parent ids and separate record duration from execute duration",
				inputs: { caller: "caller", tool: "echo", executeDelayMs: 5 },
				expected: {
					eventTypes: ["tool_execution_start", "tool_execution_end"],
					parentIds: ["caller", "caller"],
					endDurationPresent: true,
					endDurationMatchesOutcome: true,
					recordStatus: "ok",
					recordDurationPresent: true,
				},
				normalizations: [
					"nested record duration contains event emission and the full tool pipeline; exact milliseconds are volatile",
				],
				observed,
				upstreamTest: upstreamTest(
					"packages/coding-agent/test/nested-tool-calls.test.ts",
					"records nested calls and their usage on the model-issued result",
				),
			});
		}

		{
			const executed: Record<string, number> = { a: 0, b: 0 };
			const toolNamed = (name: "a" | "b") =>
				tool(name, async () => {
					executed[name] += 1;
					return { content: [{ type: "text", text: name }], details: {} };
				});
			const agent: any = new pi.Agent({
				streamFn: scriptedStream(pi, (index) =>
					index === 0
						? assistant([toolCall("removed-1", "a", {})], "toolUse")
						: assistant([{ type: "text", text: "done" }]),
				),
				initialState: { systemPrompt: "sys", tools: [toolNamed("a"), toolNamed("b")] },
			});
			const next = [agent.state.tools[1]];
			agent.state.tools = next;
			next.pop();
			const events = collect(agent);
			await agent.prompt("go");
			const declarations = agent.state.messages
				.filter((message: any) => message.role === "system")
				.map((message: any) => ({
					added: (message.toolsAdded ?? []).map((tool: any) => tool.name),
					removed: (message.toolsRemoved ?? []).map((tool: any) => tool.name),
				}));
			const result = resultMap(agent.state.messages, "toolCallId", "durationMs")["removed-1"];
			const observed = {
				assignedTools: agent.state.tools.map((tool: any) => tool.name),
				executed,
				declarations,
				systemDeclarations: declarations,
				totalMessages: agent.state.messages.length,
				resultText: result.text,
				durationPresent: result.durationPresent,
				events: events.map(digestEvent),
			};
			same(observed.assignedTools, ["b"], "state.tools assignment copies the replacement array");
			same(observed.executed, { a: 0, b: 0 }, "a removed tool never executes");
			same(
				observed.declarations,
				[
					{ added: ["a", "b"], removed: [] },
					{ added: [], removed: ["a"] },
				],
				"system transcript declares the tool delta",
			);
			same(observed.resultText, "Tool a not found", "removed tool is unknown to the executable set");
			same(observed.durationPresent, false, "removed tool result has no duration");
			scenarios.push({
				name: "replacing state.tools narrows the executable set and declares the system tool delta",
				inputs: { initialTools: ["a", "b"], replacementTools: ["b"], issuedCall: "a" },
				expected: {
					assignedTools: ["b"],
					executed: { a: 0, b: 0 },
					systemDeclarations: observed.declarations,
					totalMessages: 6,
					resultText: "Tool a not found",
					durationPresent: false,
					events: observed.events,
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"declares tool loadout changes and resolves calls against the executable set",
				),
			});
		}

		{
			const { mkdtempSync, rmSync } = await import("node:fs");
			const { tmpdir } = await import("node:os");
			const nodePath = await import("node:path");
			const { SessionManager } = await import(`${piCheckout}/packages/coding-agent/src/core/session-manager.ts`);
			const directory = mkdtempSync(nodePath.join(tmpdir(), "cch-1047-session-"));
			try {
				const manager = SessionManager.create(process.cwd(), directory);
				manager.appendMessage({ role: "user", content: "go", timestamp: 1_700_000_000_000 });
				manager.appendMessage({
					role: "toolResult",
					toolCallId: "persisted-1",
					toolName: "echo",
					content: [{ type: "text", text: "ok" }],
					details: {},
					durationMs: 4200,
					isError: false,
					timestamp: 1_700_000_000_001,
				});
				const file = manager.getSessionFile();
				expect(typeof file === "string", "session file was written");
				const reopened = SessionManager.open(file!, directory);
				const entry = reopened
					.getEntries()
					.find((candidate: any) => candidate.type === "message" && candidate.message.role === "toolResult");
				const observed = {
					role: entry?.message.role,
					durationMs: entry?.message.durationMs,
					isError: entry?.message.isError,
				};
				same(observed, { role: "toolResult", durationMs: 4200, isError: false }, "session record duration round trip");
				scenarios.push({
					name: "tool duration survives the public session record round trip",
					inputs: { sessionRecord: "toolResult", durationMs: 4200 },
					expected: { role: "toolResult", durationMs: 4200, isError: false },
					normalizations: [],
					observed,
					upstreamTest: upstreamTest(
						"packages/coding-agent/src/core/session-manager.ts",
						"appendMessage and SessionManager.open preserve the message record",
					),
				});
			} finally {
				rmSync(directory, { recursive: true, force: true });
			}
		}

		artifacts.push({
			path: "tool-execution.json",
			sourceEndpoints: [
				"packages/agent/src/agent-loop.ts:prepareToolCall,executePreparedToolCall,finalizeExecutedToolCall,emitToolExecutionEnd,createToolResultMessage,runToolCall",
				"packages/agent/src/types.ts:AgentToolCallOutcome,BeforeToolCallResult,AfterToolCallResult",
				"packages/ai/src/types.ts:ToolResultMessage",
				"packages/coding-agent/src/core/nested-tool-calls.ts:NestedToolCallRunner",
				"packages/coding-agent/src/core/session-manager.ts:SessionManager.appendMessage,SessionManager.open",
				"packages/agent/test/agent-loop.test.ts:tool execution",
				"packages/coding-agent/test/nested-tool-calls.test.ts:NestedToolCallRunner",
			],
			scenarios,
		});
	}


	// ── queue admission and consumption ───────────────────────────────────────────────────────
	{
		const scenarios: any[] = [];

		{
			const agent: any = new pi.Agent({ streamFn: () => { throw new Error("unused"); } });
			const steerResults: any[] = [];
			const followResults: any[] = [];
			for (let index = 0; index < 300; index += 1) steerResults.push(agent.steer(user(`s${index}`)));
			for (let index = 0; index < 300; index += 1) followResults.push(agent.followUp(user(`f${index}`)));
			const observed = {
				steeringMode: agent.steeringMode,
				followUpMode: agent.followUpMode,
				steeringEnqueued: 300,
				followUpEnqueued: 300,
				steerReturnValues: steerResults.map((value) => value === undefined ? "undefined" : String(value)),
				followUpReturnValues: followResults.map((value) => value === undefined ? "undefined" : String(value)),
				hasQueuedMessages: agent.hasQueuedMessages(),
				peekCount: agent.peekQueuedMessages().length,
				peekText: messageText(agent.peekQueuedMessages()[0]),
			};
			same(observed.steeringMode, "one-at-a-time", "default steering mode");
			same(observed.followUpMode, "one-at-a-time", "default follow-up mode");
			expect(observed.steerReturnValues.every((value: string) => value === "undefined"), "steer has no capacity result");
			expect(observed.followUpReturnValues.every((value: string) => value === "undefined"), "followUp has no capacity result");
			same(observed.hasQueuedMessages, true, "300+300 messages remain queued");
			same(observed.peekCount, 1, "one-at-a-time peek returns the oldest steering message");
			same(observed.peekText, "s0", "oldest steering message");
			scenarios.push({
				name: "queue admission is unbounded and both queues default to one-at-a-time",
				inputs: { steeringMessages: 300, followUpMessages: 300 },
				expected: {
					steeringMode: "one-at-a-time",
					followUpMode: "one-at-a-time",
					steeringEnqueued: 300,
					followUpEnqueued: 300,
					enqueueReturnsVoid: true,
					hasQueuedMessages: true,
					peekCount: 1,
					peekText: "s0",
				},
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should support steering message queue"),
			});
		}

		{
			const requests: any[] = [];
			let agent: any;
			const agentTool = tool("echo", async (args) => {
				agent.steer(user("steer1"));
				agent.steer(user("steer2"));
				return { content: [{ type: "text", text: args.value }], details: {} };
			});
			const provider = scriptedStream(pi, (index) =>
				index === 0
					? assistant([toolCall("echo-1", "echo", { value: "a" })], "toolUse")
					: assistant([{ type: "text", text: "done" }]),
			);
			agent = new pi.Agent({
				streamFn: (_model: any, context: any) => {
					requests.push({ roles: requestRoles(context), lastUser: lastUserText(context) });
					return provider(_model, context, {});
				},
				initialState: { tools: [agentTool] },
			});
			const events = collect(agent);
			await agent.prompt("go");
			const observed = {
				requests,
				requestRoles: requests.map((request: any) => request.roles),
				lastUserMessages: requests.map((request: any) => request.lastUser),
				requestCount: requests.length,
				events: events.map(digestEvent),
				remaining: agent.hasQueuedMessages(),
				remainingQueued: agent.hasQueuedMessages(),
				agentEndMessages: agentEndMessageCount(events),
			};
			same(observed.requests.map((request) => request.roles), [
				["system", "user"],
				["system", "user", "assistant", "toolResult", "user"],
				["system", "user", "assistant", "toolResult", "user", "assistant", "user"],
			], "one-at-a-time request contexts");
			same(observed.requests.map((request) => request.lastUser), ["go", "steer1", "steer2"], "one message per turn");
			same(observed.remaining, false, "both steering messages consumed");
			scenarios.push({
				name: "one-at-a-time steering injects the oldest message after the tool turn",
				inputs: { steeringMessages: ["steer1", "steer2"], tool: "echo" },
				expected: {
					requestRoles: observed.requests.map((request) => request.roles),
					lastUserMessages: ["go", "steer1", "steer2"],
					agentEndMessages: 7,
					remainingQueued: false,
					events: observed.events,
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest("packages/agent/test/agent-loop.test.ts", "should inject queued messages after all tool calls complete"),
			});
		}

		{
			const requests: any[] = [];
			let agent: any;
			const agentTool = tool("echo", async (args) => {
				agent.steer(user("steer1"));
				agent.steer(user("steer2"));
				return { content: [{ type: "text", text: args.value }], details: {} };
			});
			const provider = scriptedStream(pi, (index) =>
				index === 0
					? assistant([toolCall("echo-1", "echo", { value: "a" })], "toolUse")
					: assistant([{ type: "text", text: "done" }]),
			);
			agent = new pi.Agent({
				steeringMode: "all",
				streamFn: (_model: any, context: any) => {
					requests.push({ roles: requestRoles(context), lastUser: lastUserText(context) });
					return provider(_model, context, {});
				},
				initialState: { tools: [agentTool] },
			});
			const events = collect(agent);
			await agent.prompt("go");
			const observed = {
				requests,
				requestRoles: requests.map((request: any) => request.roles),
				requestCount: requests.length,
				events: events.map(digestEvent),
				remaining: agent.hasQueuedMessages(),
				remainingQueued: agent.hasQueuedMessages(),
				agentEndMessages: agentEndMessageCount(events),
			};
			same(observed.requests.map((request) => request.roles), [
				["system", "user"],
				["system", "user", "assistant", "toolResult", "user", "user"],
			], "all-mode request contexts");
			same(observed.remaining, false, "all steering messages consumed");
			scenarios.push({
				name: "all-mode steering injects every queued message in FIFO order in one turn",
				inputs: { steeringMode: "all", steeringMessages: ["steer1", "steer2"], tool: "echo" },
				expected: {
					requestRoles: observed.requests.map((request) => request.roles),
					requestCount: 2,
					agentEndMessages: 6,
					remainingQueued: false,
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should support steering message queue"),
			});
		}

		{
			const requestContexts: any[] = [];
			const provider = scriptedStream(pi, (index) =>
				assistant([{ type: "text", text: index === 0 ? "first" : "second" }]),
			);
			const agent: any = new pi.Agent({
				streamFn: (_model: any, context: any) => {
					requestContexts.push({ roles: requestRoles(context), lastUser: lastUserText(context) });
					return provider(_model, context, {});
				},
				initialState: { messages: [] },
			});
			const events: any[] = [];
			agent.subscribe((event: any) => events.push(event));
			agent.followUp(user("follow"));
			await agent.prompt("go");
			const observed = {
				requestRoles: requestContexts.map((request) => request.roles),
				lastUsers: requestContexts.map((request) => request.lastUser),
				lastUserMessages: requestContexts.map((request) => request.lastUser),
				requestCount: requestContexts.length,
				events: events.map(digestEvent),
				remaining: agent.hasQueuedMessages(),
				remainingQueued: agent.hasQueuedMessages(),
				agentEndMessages: agentEndMessageCount(events),
			};
			same(observed.requestRoles, [
				["user"],
				["user", "assistant", "user"],
			], "follow-up request contexts");
			same(observed.lastUsers, ["go", "follow"], "follow-up only after the first turn stops");
			same(observed.remaining, false, "follow-up consumed");
			scenarios.push({
				name: "follow-up runs only after an otherwise stopping assistant turn",
				inputs: { followUpMessage: "follow", firstAssistantText: "first" },
				expected: {
					requestRoles: observed.requestRoles,
					lastUserMessages: ["go", "follow"],
					agentEndMessages: 4,
					remainingQueued: false,
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should support follow-up message queue"),
			});
		}

		{
			const agent: any = new pi.Agent({ streamFn: () => { throw new Error("unused"); } });
			agent.steer(user("steer1"));
			agent.steer(user("steer2"));
			agent.followUp(user("follow"));
			const peekBefore = messageText(agent.peekQueuedMessages()[0]);
			agent.clearSteeringQueue();
			const peekAfterSteeringClear = messageText(agent.peekQueuedMessages()[0]);
			agent.clearAllQueues();
			const observed = {
				peekBefore,
				peekAfterSteeringClear,
				hasQueuedMessages: agent.hasQueuedMessages(),
				peekCount: agent.peekQueuedMessages().length,
			};
			same(observed, { peekBefore: "steer1", peekAfterSteeringClear: "follow", hasQueuedMessages: false, peekCount: 0 }, "peek and clear");
			scenarios.push({
				name: "peek prefers steering and clearAllQueues removes both queues",
				inputs: { steeringMessages: ["steer1", "steer2"], followUpMessage: "follow" },
				expected: { ...observed },
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "previews the next selected queued messages without consuming them"),
			});
		}

		{
			const requestContexts: any[] = [];
			const provider = scriptedStream(pi, (index) => assistant([{ type: "text", text: `r${index}` }]));
			const agent: any = new pi.Agent({
				streamFn: (_model: any, context: any) => {
					requestContexts.push({ roles: requestRoles(context), lastUser: lastUserText(context) });
					return provider(_model, context, {});
				},
				initialState: { messages: [assistant([{ type: "text", text: "tail" }])] },
			});
			agent.steer(user("steer"));
			agent.followUp(user("follow"));
			const events = collect(agent);
			await agent.continue();
			const observed = {
				requestRoles: requestContexts.map((request) => request.roles),
				lastUsers: requestContexts.map((request) => request.lastUser),
				lastUserMessages: requestContexts.map((request) => request.lastUser),
				requestCount: requestContexts.length,
				events: events.map(digestEvent),
				remaining: agent.hasQueuedMessages(),
				remainingQueued: agent.hasQueuedMessages(),
				agentEndMessages: agentEndMessageCount(events),
			};
			same(observed.lastUsers, ["steer", "follow"], "continue prioritizes steering before follow-up");
			same(observed.remaining, false, "continue consumes both queues");
			scenarios.push({
				name: "continue from an assistant tail drains steering before follow-up",
				inputs: { lastMessage: "assistant", steering: "steer", followUp: "follow" },
				expected: {
					requestRoles: observed.requestRoles,
					lastUserMessages: ["steer", "follow"],
					remainingQueued: false,
					agentEndMessages: 4,
				},
				normalizations: [],
				observed,
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "continue() should process queued follow-up messages after an assistant turn"),
			});
		}

		artifacts.push({
			path: "queue-admission.json",
			sourceEndpoints: [
				"packages/agent/src/agent.ts:PendingMessageQueue,Agent.steer,Agent.followUp,Agent.peekQueuedMessages,Agent.clearSteeringQueue,Agent.clearAllQueues,Agent.continue",
				"packages/agent/src/agent-loop.ts:runLoop,getSteeringMessages,getFollowUpMessages",
				"packages/agent/src/types.ts:QueueMode",
				"packages/agent/test/agent.test.ts:Agent queues",
				"packages/agent/test/agent-loop.test.ts:queued messages",
			],
			scenarios,
		});
	}


	// ── cancellation ──────────────────────────────────────────────────────────────────────────
	{
		const scenarios: any[] = [];

		{
			let agent: any;
			let releases: Array<() => void> = [];
			const started = new Promise<void>((resolve) => {
				releases.push(resolve);
			});
			const executed: string[] = [];
			let calls = 0;
			const provider = (index: number, context: any, options: any) => {
				const stream = pi.createAssistantMessageEventStream();
				if (index === 0) {
					queueMicrotask(() =>
						stream.push({
							type: "done",
							reason: "toolUse",
							message: assistant(
								[
									toolCall("slow", "slow", { value: "a" }),
									toolCall("never", "slow", { value: "b" }),
								],
								"toolUse",
							),
						}),
					);
					return stream;
				}
				queueMicrotask(() => {
					if (options.signal?.aborted) {
						stream.push({
							type: "error",
							reason: "aborted",
							error: assistant([], "aborted", "Request was aborted"),
						});
					} else {
						stream.push({ type: "done", reason: "stop", message: assistant([{ type: "text", text: "done" }]) });
					}
				});
				return stream;
			};
			const slowTool = tool("slow", async (args) => {
				executed.push(args.value);
				calls += 1;
				if (calls === 1) releases[0]!();
				while (!agent.signal?.aborted) await sleep(2);
				return { content: [{ type: "text", text: "returned after abort" }], details: {} };
			}, { executionMode: "sequential" });
			const requestSignals: boolean[] = [];
			agent = new pi.Agent({
				toolExecution: "sequential",
				streamFn: (_model: any, context: any, options: any) => {
					requestSignals.push(options.signal?.aborted === true);
					return provider(requestSignals.length - 1, context, options);
				},
				initialState: { tools: [slowTool] },
			});
			const events = collect(agent);
			const run = agent.prompt("go");
			await started;
			agent.abort();
			await run;
			const observed = {
				requestCount: requestSignals.length,
				secondRequestSignalAborted: requestSignals[1],
				executed,
				executeCalls: executed,
				startedToolCalls: events
					.filter((event) => event.type === "tool_execution_start")
					.map((event) => event.toolName),
				events: events.map(digestEvent),
				errorMessage: agent.state.errorMessage,
				isStreaming: agent.state.isStreaming,
			};
			same(observed.executed, ["a"], "only the first sequential call executed");
			same(observed.requestCount, 2, "the aborted run still makes one provider completion request");
			same(observed.secondRequestSignalAborted, true, "the completion request carries the aborted signal");
			same(
				observed.events,
				[
					"agent_start",
					"turn_start",
					"message_start:user",
					"message_end:user",
					"message_start:assistant",
					"message_end:assistant:toolUse",
					"tool_execution_start:slow",
					"tool_execution_end:slow:duration=present:error=false",
					"message_start:toolResult",
					"message_end:toolResult:slow:duration=present",
					"turn_end:assistant:tools=1",
					"turn_start",
					"message_start:assistant",
					"message_end:assistant:aborted",
					"turn_end:assistant:tools=0",
					"agent_end:messages=4",
				],
				"abort during sequential tool execution lifecycle",
			);
			same(observed.errorMessage, "Request was aborted", "aborted error state");
			same(observed.isStreaming, false, "idle after abort settles");
			scenarios.push({
				name: "abort during sequential execution skips the next call and settles through an aborted provider turn",
				inputs: { toolExecution: "sequential", calls: ["slow", "never"], abortDuringExecute: true },
				expected: {
					executeCalls: ["a"],
					startedToolCalls: ["slow"],
					requestCount: 2,
					secondRequestSignalAborted: true,
					events: observed.events,
					errorMessage: "Request was aborted",
					isStreaming: false,
				},
				normalizations: [
					"the exact tool durationMs is volatile; the expected value keeps its presence and the event ordering",
				],
				observed,
				upstreamTest: upstreamTest(
					"packages/agent/test/agent-loop.test.ts",
					"should force sequential execution when a tool has executionMode=sequential",
				),
			});
		}

		{
			const agent: any = new pi.Agent({
				streamFn: (_model: any, _context: any, options: any) => {
					const stream = pi.createAssistantMessageEventStream();
					const abort = () =>
						stream.push({ type: "error", reason: "aborted", error: assistant([], "aborted", "Request was aborted") });
					if (options.signal?.aborted) abort();
					else options.signal?.addEventListener("abort", abort, { once: true });
					return stream;
				},
			});
			const events = collect(agent);
			let signalWasAborted = false;
			agent.subscribe((event: any) => {
				if (event.type === "message_start" && event.message.role === "assistant") {
					signalWasAborted = agent.signal?.aborted === true;
				}
			});
			const run = agent.prompt("go");
			await sleep(10);
			agent.abort();
			agent.abort();
			await run;
			const observed = {
				events: events.map(digestEvent),
				errorMessage: agent.state.errorMessage,
				isStreaming: agent.state.isStreaming,
				signalWasAborted,
				signalAfterRun: agent.signal,
				signalPresentAfterRun: agent.signal !== undefined,
			};
			same(
				observed.events,
				[
					"agent_start",
					"turn_start",
					"message_start:user",
					"message_end:user",
					"message_start:assistant",
					"message_end:assistant:aborted",
					"turn_end:assistant:tools=0",
					"agent_end:messages=2",
				],
				"abort while streaming lifecycle",
			);
			same(observed.errorMessage, "Request was aborted", "stream abort error state");
			same(observed.isStreaming, false, "idle after stream abort");
			same(observed.signalWasAborted, true, "listeners observe the aborted signal");
			same(observed.signalAfterRun, undefined, "no active signal after settlement");
			scenarios.push({
				name: "abort while streaming produces one aborted assistant terminal and is idempotent",
				inputs: { abortDuringStream: true, abortCalls: 2 },
				expected: {
					events: observed.events,
					errorMessage: "Request was aborted",
					isStreaming: false,
					signalWasAborted: true,
					signalPresentAfterRun: false,
				},
				normalizations: [],
				observed: { ...observed, signalAfterRun: observed.signalAfterRun === undefined ? "none" : "present" },
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should handle abort controller"),
			});
		}

		{
			const agent: any = new pi.Agent({ streamFn: () => { throw new Error("unused"); } });
			const before = agent.signal;
			agent.abort();
			await agent.waitForIdle();
			const observed = {
				signalBefore: before,
				signalAfter: agent.signal,
				isStreaming: agent.state.isStreaming,
				messageCount: agent.state.messages.length,
			};
			same(observed, { signalBefore: undefined, signalAfter: undefined, isStreaming: false, messageCount: 0 }, "idle abort");
			scenarios.push({
				name: "abort and waitForIdle are no-ops while the Agent is idle",
				inputs: { abortWhileIdle: true },
				expected: { signalPresent: false, isStreaming: false, messageCount: 0 },
				normalizations: [],
				upstreamTest: upstreamTest("packages/agent/test/agent.test.ts", "should handle abort controller"),
			});
		}

		artifacts.push({
			path: "cancellation.json",
			sourceEndpoints: [
				"packages/agent/src/agent.ts:Agent.abort,Agent.signal,Agent.waitForIdle,Agent.prompt",
				"packages/agent/src/agent-loop.ts:executeToolCallsSequential,runLoop",
				"packages/agent/src/types.ts:AgentEvent,AgentTool",
				"packages/agent/test/agent.test.ts:should handle abort controller",
				"packages/agent/test/agent-loop.test.ts:sequential execution",
			],
			scenarios,
		});
	}

	return artifacts;
}
