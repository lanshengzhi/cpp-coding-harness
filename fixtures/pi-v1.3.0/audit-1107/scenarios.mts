/**
 * Frozen pi-v1.3.0 Agent loop-hook scenarios for ticket #1107.
 *
 * Every observation below comes from a real public `Agent` call against the private frozen
 * checkout. The scenario records are the comparison unit: `expected` names the ordering and
 * request-visible state the C++ Agent must reproduce, while `observed` keeps the raw facts
 * (including values deliberately left out of `expected`) for provenance.
 */

export type CaptureApi = {
	Agent: any;
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

function tool(name: string, execute: (args: any, id: string) => Promise<any>) {
	return {
		name,
		label: name,
		description: `${name} tool`,
		parameters: toolSchema,
		execute: async (id: string, args: any) => execute(args, id),
	};
}

function model(id: string) {
	return {
		id,
		name: id,
		api: "openai-responses",
		provider: "openai",
		reasoning: true,
		input: ["text"],
		cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
		contextWindow: 128000,
		maxTokens: 4096,
	};
}

function messageText(message: any): string {
	const content = typeof message.content === "string" ? [{ type: "text", text: message.content }] : message.content;
	return (content ?? [])
		.filter((block: any) => block.type === "text")
		.map((block: any) => block.text)
		.join("\n");
}

function requestRoles(context: any): string[] {
	return context.messages.map((message: any) => message.role);
}

function userTexts(context: any): string[] {
	return context.messages
		.filter((message: any) => message.role === "user")
		.map((message: any) => messageText(message));
}

function lastUserText(context: any): string | undefined {
	for (let index = context.messages.length - 1; index >= 0; index -= 1) {
		if (context.messages[index].role === "user") return messageText(context.messages[index]);
	}
	return undefined;
}

/// Record one provider request and answer with the scripted assistant message for its index.
function recordingStream(pi: CaptureApi, requests: any[], handler: (index: number, context: any, options: any) => any) {
	let index = 0;
	return (_model: any, context: any, options: any) => {
		const current = index++;
		requests.push({
			model: _model?.id,
			roles: requestRoles(context),
			userTexts: userTexts(context),
			lastUser: lastUserText(context),
			reasoning: options?.reasoning ?? null,
		});
		const stream = pi.createAssistantMessageEventStream();
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

function collect(agent: any): any[] {
	const events: any[] = [];
	agent.subscribe((event: any) => {
		events.push(event);
	});
	return events;
}

function upstreamTest(path: string, testCase: string) {
	return { path, case: testCase };
}

export async function captureScenarios(pi: CaptureApi, piCheckout: string): Promise<ScenarioArtifact[]> {
	const scenarios: any[] = [];
	const sourceEndpoints = [
		"packages/agent/src/agent.ts:Agent.prompt,Agent.subscribe,Agent.state,Agent.steer,Agent.followUp",
		"packages/agent/src/agent-loop.ts:runLoop",
		"packages/agent/src/types.ts:AgentOptions,AgentTurnContext,AgentTurnDecision,AgentLoopTurnUpdate,AgentRequestUpdate",
	];

	// ── prepareRequest ────────────────────────────────────────────────────────────────────────
	{
		const steering = user("steering");
		const canonical = user("canonical projection");
		const replacementModel = model("replacement");
		const requests: any[] = [];
		const completedRoles: string[] = [];
		let observedCompletedRoles: string[] = [];
		let observedContextRoles: string[] = [];
		let prepareCalls = 0;
		const agent: any = new pi.Agent({
			streamFn: recordingStream(pi, requests, () => assistant([{ type: "text", text: "done" }])),
			prepareRequest: ({ context }: any) => {
				prepareCalls += 1;
				observedCompletedRoles = completedRoles.slice();
				observedContextRoles = requestRoles(context);
				return {
					context: { ...context, messages: [canonical] },
					model: replacementModel,
					thinkingLevel: "high",
				};
			},
		});
		agent.subscribe((event: any) => {
			if (event.type === "message_end") completedRoles.push(event.message.role);
		});
		agent.steer(steering);
		await agent.prompt("go");
		const observed = {
			prepareCalls,
			completedRolesBeforePrepare: observedCompletedRoles,
			contextRolesInPrepare: observedContextRoles,
			requests,
		};
		same(observed.prepareCalls, 1, "prepareRequest called once");
		same(observed.completedRolesBeforePrepare, ["user", "user"], "pending prompt and steering completed before prepareRequest");
		same(observed.contextRolesInPrepare, ["user", "user"], "prepareRequest sees the emitted pending messages");
		same(observed.requests.length, 1, "one provider request");
		same(observed.requests[0].roles, ["user"], "replacement context reaches the request");
		same(observed.requests[0].lastUser, "canonical projection", "replacement message text");
		same(observed.requests[0].model, "replacement", "replacement model reaches the request");
		same(observed.requests[0].reasoning, "high", "replacement thinking level reaches the request");
		scenarios.push({
			name: "prepareRequest observes the emitted pending prompt and can replace the request state",
			inputs: { prompt: "go", queuedSteering: "steering", replacementModel: "replacement", replacementThinking: "high" },
			expected: {
				prepareCalls: observed.prepareCalls,
				completedRolesBeforePrepare: observed.completedRolesBeforePrepare,
				contextRolesInPrepare: observed.contextRolesInPrepare,
				requestRoles: observed.requests[0].roles,
				requestLastUser: observed.requests[0].lastUser,
				requestModel: observed.requests[0].model,
				requestReasoning: observed.requests[0].reasoning,
			},
			normalizations: [],
			upstreamTest: upstreamTest(
				"packages/agent/test/agent-loop.test.ts",
				"prepares the initial request after pending messages and can replace request state",
			),
		});
	}

	{
		const replacementModel = model("replacement");
		const requests: any[] = [];
		let prepareCalls = 0;
		const agent: any = new pi.Agent({
			streamFn: recordingStream(pi, requests, (index) =>
				index === 0
					? assistant([toolCall("echo-1", "echo", { value: "a" })], "toolUse")
					: assistant([{ type: "text", text: "done" }]),
			),
			initialState: {
				tools: [tool("echo", async (args) => ({ content: [{ type: "text", text: args.value }], details: {} }))],
			},
			prepareRequest: () => {
				prepareCalls += 1;
				if (prepareCalls === 1) return { model: replacementModel, thinkingLevel: "high" };
				return undefined;
			},
		});
		await agent.prompt("go");
		const observed = {
			prepareCalls,
			requestCount: requests.length,
			requestModels: requests.map((request) => request.model),
			requestReasoning: requests.map((request) => request.reasoning),
		};
		same(observed.prepareCalls, 2, "prepareRequest runs before every provider request");
		same(observed.requestCount, 2, "tool turn continues to a second request");
		same(observed.requestModels, ["replacement", "replacement"], "replacement model carries into the next request");
		same(observed.requestReasoning, ["high", "high"], "replacement thinking level carries into the next request");
		scenarios.push({
			name: "prepareRequest replacement state carries into the next request of the run",
			inputs: { prompt: "go", tool: "echo", replacementModel: "replacement", replacementThinking: "high" },
			expected: observed,
			normalizations: [],
			upstreamTest: upstreamTest(
				"packages/agent/test/agent-loop.test.ts",
				"prepares the initial request after pending messages and can replace request state",
			),
		});
	}

	{
		const late = user("late steering");
		const requests: any[] = [];
		let prepareCalls = 0;
		let agentRef: any;
		const agent: any = new pi.Agent({
			streamFn: recordingStream(pi, requests, () => assistant([{ type: "text", text: "done" }])),
			prepareRequest: () => {
				prepareCalls += 1;
				if (prepareCalls === 1) agentRef.steer(late);
			},
		});
		agentRef = agent;
		await agent.prompt("go");
		const observed = {
			prepareCalls,
			requestCount: requests.length,
			lateSteeringInRequest: requests.map((request) => request.userTexts.includes("late steering")),
		};
		same(observed.prepareCalls, 2, "late steering schedules a second request");
		same(observed.requestCount, 2, "two provider requests");
		same(observed.lateSteeringInRequest, [false, true], "prepareRequest does not poll steering");
		scenarios.push({
			name: "prepareRequest does not poll the steering queue",
			inputs: { prompt: "go", lateSteeringQueuedBy: "prepareRequest" },
			expected: observed,
			normalizations: [],
			upstreamTest: upstreamTest("packages/agent/test/agent-loop.test.ts", "does not poll steering after prepareRequest"),
		});
	}

	// ── finishTurn ───────────────────────────────────────────────────────────────────────────
	{
		const ordering: string[] = [];
		const finishObservations: any[] = [];
		const agent: any = new pi.Agent({
			streamFn: recordingStream(pi, [], (index) =>
				index === 0
					? assistant([toolCall("echo-1", "echo", { value: "a" })], "toolUse")
					: assistant([{ type: "text", text: "done" }]),
			),
			initialState: {
				tools: [tool("echo", async (args) => ({ content: [{ type: "text", text: args.value }], details: {} }))],
			},
			finishTurn: (turn: any) => {
				finishObservations.push({
					toolResults: turn.toolResults.length,
					lastContextRole: turn.context.messages.at(-1)?.role,
					newMessages: turn.newMessages.length,
				});
				ordering.push("finishTurn");
				return undefined;
			},
		});
		agent.subscribe((event: any) => {
			if (event.type === "message_end") ordering.push(`message_end:${event.message.role}`);
			if (event.type === "turn_end") ordering.push(`turn_end:tools=${event.toolResults.length}`);
		});
		await agent.prompt("go");
		const firstTurnEnd = ordering.indexOf("turn_end:tools=1");
		const observed = {
			orderingBeforeFirstTurnEnd: ordering.slice(Math.max(0, firstTurnEnd - 2), firstTurnEnd + 1),
			firstFinishObservation: finishObservations[0],
			finishCalls: ordering.filter((entry) => entry === "finishTurn").length,
		};
		same(observed.finishCalls, 2, "finishTurn runs after every completed turn");
		same(
			observed.orderingBeforeFirstTurnEnd,
			["message_end:toolResult", "finishTurn", "turn_end:tools=1"],
			"finishTurn runs before turn_end",
		);
		same(
			observed.firstFinishObservation,
			{ toolResults: 1, lastContextRole: "toolResult", newMessages: 3 },
			"finishTurn sees the completed turn",
		);
		scenarios.push({
			name: "finishTurn runs after the tool-result message and before turn_end",
			inputs: { prompt: "go", tool: "echo" },
			expected: observed,
			normalizations: [],
			upstreamTest: upstreamTest(
				"packages/agent/test/agent-loop.test.ts",
				"runs finishTurn after tool-result messages and before turn_end",
			),
		});
	}

	{
		const requests: any[] = [];
		let finishCalls = 0;
		const agent: any = new pi.Agent({
			streamFn: recordingStream(pi, requests, () => assistant([{ type: "text", text: "done" }])),
			finishTurn: () => {
				finishCalls += 1;
				return finishCalls === 1 ? { action: "continue" } : undefined;
			},
		});
		await agent.prompt("go");
		const observed = {
			finishCalls,
			requestCount: requests.length,
			requestRoles: requests.map((request) => request.roles),
		};
		same(observed.finishCalls, 2, "finishTurn called after each turn");
		same(observed.requestCount, 2, "one context-only request satisfies continuation");
		same(observed.requestRoles[1], ["user", "assistant"], "continuation reuses the current context");
		scenarios.push({
			name: "finishTurn action continue makes exactly one context-only request",
			inputs: { prompt: "go", finishTurnAction: "continue" },
			expected: observed,
			normalizations: [],
			upstreamTest: upstreamTest(
				"packages/agent/test/agent-loop.test.ts",
				"makes exactly one context-only request when no natural request satisfies continuation",
			),
		});
	}

	{
		const requests: any[] = [];
		let finishCalls = 0;
		const agent: any = new pi.Agent({
			streamFn: recordingStream(pi, requests, (index) =>
				index === 0
					? assistant([toolCall("echo-1", "echo", { value: "a" })], "toolUse")
					: assistant([{ type: "text", text: "done" }]),
			),
			initialState: {
				tools: [tool("echo", async (args) => ({ content: [{ type: "text", text: args.value }], details: {} }))],
			},
			finishTurn: () => {
				finishCalls += 1;
				return finishCalls === 1 ? { action: "continue" } : undefined;
			},
		});
		await agent.prompt("go");
		const observed = { finishCalls, requestCount: requests.length };
		same(observed.finishCalls, 2, "finishTurn called after each turn");
		same(observed.requestCount, 2, "the tool-result request satisfies continuation without an extra request");
		scenarios.push({
			name: "finishTurn action continue is satisfied by the natural tool-result request",
			inputs: { prompt: "go", tool: "echo", finishTurnAction: "continue" },
			expected: observed,
			normalizations: [],
			upstreamTest: upstreamTest(
				"packages/agent/test/agent-loop.test.ts",
				"lets a natural tool-result request satisfy continuation",
			),
		});
	}

	{
		const ordering: string[] = [];
		let finishCalls = 0;
		let observedStopReason: string | null = null;
		const requests: any[] = [];
		const agent: any = new pi.Agent({
			streamFn: (_model: any, context: any, options: any) => {
				requests.push({ roles: requestRoles(context) });
				const stream = pi.createAssistantMessageEventStream();
				queueMicrotask(() => {
					stream.push({
						type: "error",
						reason: "aborted",
						error: assistant([], "aborted", "Request was aborted"),
					});
				});
				return stream;
			},
			finishTurn: ({ message }: any) => {
				finishCalls += 1;
				observedStopReason = message.stopReason;
				ordering.push("finishTurn");
				return { action: "continue" };
			},
		});
		agent.subscribe((event: any) => {
			if (event.type === "turn_end") ordering.push("turn_end");
		});
		agent.followUp(user("queued"));
		await agent.prompt("go");
		const observed = {
			finishCalls,
			observedStopReason,
			ordering,
			requestCount: requests.length,
			queuedAfterRun: agent.hasQueuedMessages(),
		};
		same(observed.finishCalls, 1, "finishTurn runs for the aborted assistant");
		same(observed.observedStopReason, "aborted", "finishTurn observes the aborted message");
		same(observed.ordering, ["finishTurn", "turn_end"], "finishTurn runs before turn_end on the hard exit");
		same(observed.requestCount, 1, "continue cannot extend an aborted terminal");
		same(observed.queuedAfterRun, true, "the hard exit does not poll the follow-up queue");
		scenarios.push({
			name: "finishTurn observes an aborted assistant before turn_end without continuing",
			inputs: { prompt: "go", providerTerminal: "aborted", finishTurnAction: "continue", queuedFollowUp: "queued" },
			expected: observed,
			normalizations: [],
			upstreamTest: upstreamTest(
				"packages/agent/test/agent-loop.test.ts",
				"runs finishTurn for a aborted assistant before turn_end without changing the hard exit",
			),
		});
	}

	// ── prepareNextTurn ──────────────────────────────────────────────────────────────────────
	{
		const requests: any[] = [];
		const ordering: string[] = [];
		let prepareCalls = 0;
		const agent: any = new pi.Agent({
			streamFn: recordingStream(pi, requests, (index) =>
				index === 0
					? assistant([toolCall("echo-1", "echo", { value: "a" })], "toolUse")
					: assistant([{ type: "text", text: "done" }]),
			),
			initialState: {
				tools: [tool("echo", async (args) => ({ content: [{ type: "text", text: args.value }], details: {} }))],
			},
			prepareNextTurnWithContext: () => {
				prepareCalls += 1;
				ordering.push("prepareNextTurn");
				return { messages: [user("prepared")] };
			},
		});
		agent.subscribe((event: any) => {
			if (event.type === "turn_start") ordering.push("turn_start");
			if (event.type === "message_start" || event.type === "message_end") {
				ordering.push(`${event.type}:${event.message.role}`);
			}
		});
		await agent.prompt("go");
		const observed = {
			prepareCalls,
			requestCount: requests.length,
			secondRequestRoles: requests[1].roles,
			secondRequestLastUser: requests[1].lastUser,
			orderingWindow: ordering.slice(ordering.indexOf("prepareNextTurn"), ordering.indexOf("prepareNextTurn") + 5),
		};
		same(observed.prepareCalls, 1, "prepareNextTurn called once");
		same(observed.requestCount, 2, "two provider requests");
		same(observed.secondRequestRoles, ["system", "user", "assistant", "toolResult", "user"], "prepared message order");
		same(observed.secondRequestLastUser, "prepared", "prepared message reaches the next request");
		same(
			observed.orderingWindow,
			["prepareNextTurn", "turn_start", "message_start:user", "message_end:user", "message_start:assistant"],
			"prepared message is emitted after turn_start and before the next response",
		);
		scenarios.push({
			name: "prepareNextTurn appended messages are emitted before the next request",
			inputs: { prompt: "go", tool: "echo", preparedMessage: "prepared" },
			expected: {
				prepareCalls: observed.prepareCalls,
				requestCount: observed.requestCount,
				secondRequestRoles: observed.secondRequestRoles,
				secondRequestLastUser: observed.secondRequestLastUser,
				orderingWindow: observed.orderingWindow,
			},
			normalizations: [],
			upstreamTest: upstreamTest(
				"packages/agent/test/agent-loop.test.ts",
				"should use prepareNextTurn snapshot before continuing",
			),
		});
	}

	{
		const requests: any[] = [];
		let prepareCalls = 0;
		let finishCalls = 0;
		const agent: any = new pi.Agent({
			streamFn: recordingStream(pi, requests, () => assistant([{ type: "text", text: "done" }])),
			finishTurn: () => {
				finishCalls += 1;
				return finishCalls === 1 ? { action: "continue" } : undefined;
			},
			prepareNextTurnWithContext: ({ context }: any) => {
				prepareCalls += 1;
				return {
					context: { ...context, messages: [user("replacement")] },
					messages: [user("prepared")],
				};
			},
		});
		await agent.prompt("go");
		const observed = {
			prepareCalls,
			requestCount: requests.length,
			secondRequestRoles: requests[1].roles,
			secondRequestUserTexts: requests[1].userTexts,
		};
		same(observed.prepareCalls, 1, "prepareNextTurn called once");
		same(observed.requestCount, 2, "two provider requests");
		same(observed.secondRequestRoles, ["user", "user"], "replacement context and appended messages both reach the request");
		same(observed.secondRequestUserTexts, ["replacement", "prepared"], "replacement messages precede appended messages");
		scenarios.push({
			name: "prepareNextTurn replacement context and appended messages both reach the next request",
			inputs: { prompt: "go", finishTurnAction: "continue", replacementMessage: "replacement", preparedMessage: "prepared" },
			expected: observed,
			normalizations: [],
			upstreamTest: upstreamTest(
				"packages/agent/test/agent-loop.test.ts",
				"should use prepareNextTurn snapshot before continuing",
			),
		});
	}

	return [{ path: "turn-hooks.json", sourceEndpoints, scenarios }];
}
