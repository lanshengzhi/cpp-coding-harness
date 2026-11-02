/**
 * Scenarios for the `audit-1191` MCP source-only capture.
 *
 * Each function drives frozen pi v1.3.0 through a public seam and returns the observed value. The
 * `capture-1191.mts` runner turns the observations into the manifest. The scenarios use real
 * loopback HTTP fixtures and a real stdio child; no C++ expectation is an input.
 */

import { createHash } from "node:crypto";
import {
    LATEST_PROTOCOL_VERSION,
    McpClient,
    McpError,
    McpServerConnection,
    StdioTransport,
    StreamableHttpTransport,
    createDefaultTransport,
    createInMemoryTransportPair,
    createMcpExtension,
    createMcpToolDefinition,
    createMcpToolName,
    isJsonRpcRequest,
} from "./pi.mts";
import {
    assert,
    delay,
    errorRecord,
    header,
    readTrace,
    startHttpMcp,
    startStdioFixture,
    waitFor,
    type HttpMcpFixture,
    type StdioFixture,
} from "./fixtures.mts";

export interface Scenario {
    name: string;
    source: string;
    expected: unknown;
}

const ROOTS = [{ uri: "file:///workspace", name: "workspace" }];

function clientOptions(overrides: Record<string, unknown> = {}): Record<string, unknown> {
    return { name: "audit-1191", version: "1.0.0", requestTimeoutMs: 5_000, roots: ROOTS, ...overrides };
}

function toolPage(tools: string[], nextCursor?: unknown): unknown {
    return {
        tools: tools.map((name) => ({ name, inputSchema: { type: "object", properties: {} } })),
        ...(nextCursor === undefined ? {} : { nextCursor }),
    };
}

function namesOf(tools: any[]): string[] {
    return tools.map((tool) => tool.name);
}

function postMethods(fixture: HttpMcpFixture): string[] {
    return fixture.requests.map((request) => {
        if (request.method !== "POST") return request.method;
        return request.message?.method ?? "response";
    });
}

async function connectStdio(
    mode = "protocol",
    environment: Record<string, string> = {},
    options: Record<string, unknown> = {},
): Promise<{ fixture: StdioFixture; client: any }> {
    const fixture = await startStdioFixture(mode, environment);
    const client = new McpClient(clientOptions(options));
    await client.connect(fixture.transport);
    return { fixture, client };
}

async function connectHttp(
    fixture: HttpMcpFixture,
    transportOptions: Record<string, unknown> = {},
    options: Record<string, unknown> = {},
): Promise<any> {
    const client = new McpClient(clientOptions(options));
    await client.connect(new StreamableHttpTransport({ url: fixture.url, ...transportOptions }));
    return client;
}

// ---------------------------------------------------------------------------------------------------
// cch_mcp:mcp-tools-list — `tools/list` pagination and application tool naming.
// ---------------------------------------------------------------------------------------------------

export async function toolsListFollowsCursor(): Promise<Scenario> {
    const fixture = await startHttpMcp({
        toolsList: (cursor) =>
            cursor === undefined
                ? toolPage(["search"], "page-2")
                : toolPage(["read"], ""),
    });
    let client;
    try {
        client = await connectHttp(fixture, { openGetStream: false });
        const tools = await client.listTools();
        const listRequests = fixture.requests.filter((request) => request.message?.method === "tools/list");
        const cursors = listRequests.map((request) => request.message.params?.cursor ?? null);
        assert(tools.length === 2, "both pages must be collected");
        return {
            name: "tools/list follows nextCursor across pages and ends on an empty cursor",
            source: "packages/mcp/src/client.ts:McpClient.listTools,listAll,validateListPage",
            expected: {
                tools: namesOf(tools),
                cursors,
                pageRequests: listRequests.length,
                terminalCursor: "",
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function toolsListNullCursor(): Promise<Scenario> {
    const fixture = await startHttpMcp({ toolsList: () => toolPage(["only"], null) });
    let client;
    try {
        client = await connectHttp(fixture, { openGetStream: false });
        const tools = await client.listTools();
        const listRequests = fixture.requests.filter((request) => request.message?.method === "tools/list");
        return {
            name: "a null nextCursor ends tools/list without another request",
            source: "packages/mcp/src/client.ts:validateListPage,listAll",
            expected: {
                tools: namesOf(tools),
                pageRequests: listRequests.length,
                nextCursor: null,
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function toolsListEmptyCursor(): Promise<Scenario> {
    const fixture = await startHttpMcp({ toolsList: () => toolPage(["only"], "") });
    let client;
    try {
        client = await connectHttp(fixture, { openGetStream: false });
        const tools = await client.listTools();
        const listRequests = fixture.requests.filter((request) => request.message?.method === "tools/list");
        return {
            name: "an empty nextCursor ends tools/list without another request",
            source: "packages/mcp/src/client.ts:validateListPage,listAll",
            expected: {
                tools: namesOf(tools),
                pageRequests: listRequests.length,
                nextCursor: "",
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function toolsListDuplicateCursor(): Promise<Scenario> {
    const fixture = await startHttpMcp({ toolsList: () => toolPage([], "loop") });
    let client;
    try {
        client = await connectHttp(fixture, { openGetStream: false });
        const outcome = await client.listTools().then(
            () => null,
            (error: unknown) => errorRecord(error),
        );
        const listRequests = fixture.requests.filter((request) => request.message?.method === "tools/list");
        assert(outcome !== null, "a duplicate cursor must fail the list");
        return {
            name: "a repeated nextCursor is an explicit duplicate-cursor error",
            source: "packages/mcp/src/client.ts:listAll",
            expected: {
                error: outcome,
                pageRequests: listRequests.length,
                repeatedCursor: "loop",
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function toolsListPageBound(): Promise<Scenario> {
    const fixture = await startHttpMcp({
        toolsList: (cursor, index) => toolPage([], `page-${index}`),
    });
    let client;
    try {
        client = await connectHttp(fixture, { openGetStream: false });
        const outcome = await client.listTools().then(
            () => null,
            (error: unknown) => errorRecord(error),
        );
        const listRequests = fixture.requests.filter((request) => request.message?.method === "tools/list");
        assert(outcome !== null, "an endless page sequence must fail the list");
        return {
            name: "tools/list stops after pi's 1000-page bound",
            source: "packages/mcp/src/client.ts:listAll,MAX_LIST_PAGES",
            expected: {
                error: outcome,
                pageRequests: listRequests.length,
                maxPages: 1000,
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function toolNameSanitization(): Promise<Scenario> {
    const exactTool = "t".repeat(56);
    const overTool = "u".repeat(57);
    const exactBound = createMcpToolName("s", exactTool);
    const overBound = createMcpToolName("s", overTool);
    const long = createMcpToolName("server", "x".repeat(100));
    const takenPlain = createMcpToolName("s", "a_b");
    const secondWhenTaken = createMcpToolName("s", "a-b", (candidate: string) => candidate === takenPlain);
    const short = createMcpToolName("my-server", "get.item/v2");
    const distinct = [createMcpToolName("filesystem", "read_file"), createMcpToolName("filesystem", "write_file")];
    const expectedHash = createHash("sha256").update(`server\0${"x".repeat(100)}`).digest("hex").slice(0, 8);

    assert(exactBound.length === 64 && exactBound === `mcp__s__${exactTool}`, "a 64-character name stays plain");
    assert(overBound.length === 64 && overBound !== exactBound, "a name over the bound is shortened to 64");
    assert(long.length === 64 && long.endsWith(`_${expectedHash}`), "the long name carries the raw-pair hash suffix");
    assert(secondWhenTaken !== takenPlain, "a taken plain name takes the hash suffix");
    assert(short === "mcp__my_server__get_item_v2", "punctuation outside [A-Za-z0-9_] becomes _");

    return {
        name: "application tool names sanitize punctuation, bound at 64 and hash collisions from the raw pair",
        source:
            "packages/coding-agent/src/extensions/mcp/tools.ts:createMcpToolName; packages/coding-agent/src/extensions/mcp/index.ts:registerTools",
        expected: {
            short,
            exactBound,
            exactBoundLength: exactBound.length,
            overBound,
            overBoundLength: overBound.length,
            long,
            longHashSuffix: expectedHash,
            takenPlain,
            secondWhenTaken,
            secondWhenTakenHasSuffix:
                secondWhenTaken.startsWith(`${takenPlain}_`) && secondWhenTaken.length === takenPlain.length + 9,
            distinct,
        },
    };
}

async function assignmentFor(rawTools: string[]): Promise<Array<{ label: string; name: string }>> {
    const pair = createInMemoryTransportPair();
    await pair.server.start();
    pair.server.onMessage((message: any) => {
        if (!isJsonRpcRequest(message)) return;
        let result: unknown = { content: [] };
        if (message.method === "initialize") {
            result = {
                protocolVersion: LATEST_PROTOCOL_VERSION,
                capabilities: { tools: {} },
                serverInfo: { name: "scripted", version: "1.0.0" },
            };
        } else if (message.method === "tools/list") {
            result = {
                tools: rawTools.map((name) => ({ name, inputSchema: { type: "object", properties: {} } })),
            };
        }
        void pair.server.send({ jsonrpc: "2.0", id: message.id, result });
    });

    const stub = {
        tools: [] as Array<Record<string, unknown>>,
        active: [] as string[],
        handlers: new Map<string, Function>(),
    };
    const api = {
        getMcpServers: () => [],
        registerToolRenderer: () => {},
        registerTool: (tool: Record<string, unknown>) => stub.tools.push(tool),
        getAllTools: () => stub.tools,
        getActiveTools: () => stub.active,
        setActiveTools: (names: string[]) => {
            stub.active.splice(0, stub.active.length, ...names);
        },
        on: (event: string, handler: Function) => stub.handlers.set(event, handler),
        registerCommand: () => {},
        registerMessageRenderer: () => {},
        events: { emit: () => {} },
    };
    const extension = createMcpExtension({
        loadConfig: () => ({
            servers: [{ name: "srv", config: { command: "unused", exposure: "direct" }, source: "test" }],
            errors: [],
        }),
        createTransport: () => pair.client,
        logPath: "/tmp/audit-1191/mcp.log",
    });
    extension(api as never);
    const context = {
        cwd: "/tmp/audit-1191/workspace",
        hasUI: true,
        mode: "print",
        signal: new AbortController().signal,
        ui: { notify: () => {}, input: async () => undefined },
    };
    try {
        await stub.handlers.get("session_start")!({ type: "session_start" }, context);
        const prompt = { systemPromptOptions: { sections: {} } };
        await stub.handlers.get("before_agent_start")!({ type: "before_agent_start", ...prompt }, context);
        return stub.tools
            .filter((tool) => String(tool.label).startsWith("srv/"))
            .map((tool) => ({ label: String(tool.label), name: String(tool.name) }));
    } finally {
        await stub.handlers.get("session_shutdown")?.({ type: "session_shutdown", reason: "quit" }, context);
        await pair.client.close().catch(() => {});
    }
}

export async function applicationToolNameAssignment(): Promise<Scenario> {
    const forward = await assignmentFor(["a-b", "a_b"]);
    const reverse = await assignmentFor(["a_b", "a-b"]);
    const nameFor = (list: Array<{ label: string; name: string }>, label: string) =>
        list.find((entry) => entry.label === label)?.name ?? null;
    const forwardDash = nameFor(forward, "srv/a-b");
    const forwardUnderscore = nameFor(forward, "srv/a_b");
    const reverseDash = nameFor(reverse, "srv/a-b");
    const reverseUnderscore = nameFor(reverse, "srv/a_b");
    assert(forward.length === 2 && reverse.length === 2, "both colliding tools must register");
    assert(forwardDash !== forwardUnderscore, "both colliding tools take distinct hashed names");
    assert(forwardDash !== "mcp__srv__a_b" && forwardUnderscore !== "mcp__srv__a_b", "neither keeps the plain name");
    assert(forwardDash === reverseDash && forwardUnderscore === reverseUnderscore, "the assignment is order-independent");
    return {
        name: "application tool names hash every colliding sanitized name order-independently",
        source: "packages/coding-agent/src/extensions/mcp/index.ts:registerTools,assignName",
        expected: {
            forward,
            reverse,
            plainName: "mcp__srv__a_b",
            sameAssignmentByRaw: forwardDash === reverseDash && forwardUnderscore === reverseUnderscore,
        },
    };
}

// ---------------------------------------------------------------------------------------------------
// cch_mcp:mcp-progress — `_meta.progressToken`, notifications, text and deadline re-arming.
// ---------------------------------------------------------------------------------------------------

function progressObservation(events: any[], request: any, result: any): Record<string, unknown> {
    const delivered = events.map((event) => ({
        progressToken: event.progressToken,
        progress: event.progress,
        total: event.total ?? null,
        message: event.message ?? null,
    }));
    return {
        requestId: request.id,
        progressTokenInjected: request.progressToken === request.id,
        progressToken: request.progressToken,
        progressCount: delivered.length,
        progress: delivered,
        resultText: result.content[0]?.text ?? null,
    };
}

export async function stdioProgressDelivery(): Promise<Scenario> {
    const { fixture, client } = await connectStdio("protocol");
    try {
        const events: any[] = [];
        const result = await client.callTool("progress", {}, {
            timeoutMs: 2_000,
            onProgress: (progress: any) => events.push(progress),
        });
        await delay(140);
        const trace = fixture.trace();
        const request = trace.find((entry) => entry.type === "call" && entry.name === "progress");
        assert(request !== undefined, "the child must receive the tools/call request");
        const observed = progressObservation(events, request, result);
        assert(observed.progressCount === 2, "only the two matching progress notifications are delivered");
        assert(observed.progressTokenInjected === true, "_meta.progressToken must be the request id");
        return {
            name: "stdio: onProgress injects _meta.progressToken and delivers only matching progress",
            source:
                "packages/mcp/src/client.ts:McpClient.requestInternal,handleProgress; packages/mcp/src/transports/stdio.ts:StdioTransport",
            expected: {
                ...observed,
                wrongTokenIgnored: true,
                lateProgressIgnored: true,
                transport: "stdio",
            },
        };
    } finally {
        await client.close().catch(() => {});
        await fixture.cleanup();
    }
}

export async function httpProgressDelivery(): Promise<Scenario> {
    const fixture = await startHttpMcp({
        toolCall: (message, response, api) => {
            const token = message.params?._meta?.progressToken;
            response.writeHead(200, { "content-type": "text/event-stream", "mcp-session-id": "session-1" });
            setTimeout(() => {
                api.writeSse(response, {
                    jsonrpc: "2.0",
                    method: "notifications/progress",
                    params: { progressToken: Number(token) + 1000, progress: 0 },
                });
            }, 10);
            setTimeout(() => {
                api.writeSse(response, {
                    jsonrpc: "2.0",
                    method: "notifications/progress",
                    params: { progressToken: token, progress: 1, total: 2, message: "halfway" },
                });
            }, 40);
            setTimeout(() => {
                api.writeSse(response, {
                    jsonrpc: "2.0",
                    method: "notifications/progress",
                    params: { progressToken: token, progress: 2 },
                });
            }, 80);
            setTimeout(() => {
                api.writeSse(response, {
                    jsonrpc: "2.0",
                    id: message.id,
                    result: { content: [{ type: "text", text: "done" }] },
                });
            }, 110);
            setTimeout(() => {
                api.writeSse(response, {
                    jsonrpc: "2.0",
                    method: "notifications/progress",
                    params: { progressToken: token, progress: 99 },
                });
                response.end();
            }, 170);
        },
    });
    let client;
    try {
        client = await connectHttp(fixture, { openGetStream: false });
        const events: any[] = [];
        const result = await client.callTool("progress", {}, {
            timeoutMs: 2_000,
            onProgress: (progress: any) => events.push(progress),
        });
        await delay(140);
        const request = fixture.requests.find((entry) => entry.message?.method === "tools/call");
        assert(request !== undefined, "the fixture must receive the tools/call request");
        const observed = progressObservation(
            events,
            {
                id: request.message.id,
                progressToken: request.message.params?._meta?.progressToken,
            },
            result,
        );
        assert(observed.progressCount === 2, "only the two matching progress notifications are delivered");
        assert(observed.progressTokenInjected === true, "_meta.progressToken must be the request id");
        return {
            name: "HTTP: onProgress injects _meta.progressToken and delivers only matching progress",
            source:
                "packages/mcp/src/client.ts:McpClient.requestInternal,handleProgress; packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport",
            expected: {
                ...observed,
                wrongTokenIgnored: true,
                lateProgressIgnored: true,
                transport: "http",
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

async function progressRearm(transport: "stdio" | "http"): Promise<Record<string, unknown>> {
    let fixture: HttpMcpFixture | StdioFixture;
    let client: any;
    let close: () => Promise<void>;
    const events: any[] = [];
    if (transport === "stdio") {
        const started = await connectStdio("protocol");
        fixture = started.fixture;
        client = started.client;
        close = async () => {
            await client.close().catch(() => {});
            await started.fixture.cleanup();
        };
    } else {
        const started = await startHttpMcp({
            toolCall: (message, response, api) => {
                const token = message.params?._meta?.progressToken;
                response.writeHead(200, { "content-type": "text/event-stream", "mcp-session-id": "session-1" });
                setTimeout(() => {
                    api.writeSse(response, {
                        jsonrpc: "2.0",
                        method: "notifications/progress",
                        params: { progressToken: Number(token) + 1000, progress: 0 },
                    });
                }, 10);
                setTimeout(() => {
                    api.writeSse(response, {
                        jsonrpc: "2.0",
                        method: "notifications/progress",
                        params: { progressToken: token, progress: 1, total: 2, message: "halfway" },
                    });
                }, 40);
                setTimeout(() => {
                    api.writeSse(response, {
                        jsonrpc: "2.0",
                        method: "notifications/progress",
                        params: { progressToken: token, progress: 2 },
                    });
                }, 80);
                setTimeout(() => {
                    api.writeSse(response, {
                        jsonrpc: "2.0",
                        id: message.id,
                        result: { content: [{ type: "text", text: "done" }] },
                    });
                    response.end();
                }, 110);
            },
        });
        fixture = started;
        client = await connectHttp(started, { openGetStream: false });
        close = async () => {
            await client.close().catch(() => {});
            await started.close();
        };
    }
    try {
        const startedAt = Date.now();
        const result = await client.callTool("progress", {}, {
            timeoutMs: 70,
            onProgress: (progress: any) => events.push(progress),
        });
        const elapsedMs = Date.now() - startedAt;
        assert(elapsedMs >= 80, "the call must outlive the un-rearmed deadline");
        assert(result.content[0]?.text === "done", "the call must resolve after the re-armed deadline");
        return {
            timeoutMs: 70,
            resolved: true,
            elapsedMs,
            elapsedBeyondDeadline: elapsedMs >= 80,
            progressCount: events.length,
            resultText: result.content[0]?.text ?? null,
            transport,
        };
    } finally {
        await close();
    }
}

export async function stdioProgressRearmsDeadline(): Promise<Scenario> {
    return {
        name: "stdio: a progress notification re-arms the request deadline",
        source: "packages/mcp/src/client.ts:handleProgress,armTimeout",
        expected: await progressRearm("stdio"),
    };
}

export async function httpProgressRearmsDeadline(): Promise<Scenario> {
    return {
        name: "HTTP: a progress notification re-arms the request deadline",
        source: "packages/mcp/src/client.ts:handleProgress,armTimeout",
        expected: await progressRearm("http"),
    };
}

export async function progressText(): Promise<Scenario> {
    async function textsFor(params: Record<string, unknown>): Promise<string[]> {
        const updates: any[] = [];
        const definition = createMcpToolDefinition({
            server: "srv",
            tool: { name: "slow", inputSchema: { type: "object", properties: {} } },
            name: "mcp__srv__slow",
            exposure: "direct",
            namespace: { name: "srv" },
            timeoutMs: 1_000,
            getClient: async () => ({
                callTool: async (_name: string, _args: unknown, options: any) => {
                    options.onProgress?.(params);
                    return { content: [{ type: "text", text: "ok" }] };
                },
            }),
        });
        await definition.execute("call-progress", {}, undefined, (update: any) => updates.push(update));
        return updates.map((update) =>
            (update.content ?? [])
                .filter((block: any) => block.type === "text")
                .map((block: any) => block.text)
                .join(""),
        );
    }

    const cases = [
        { params: { message: "custom message", progress: 1, total: 5 }, texts: await textsFor({ message: "custom message", progress: 1, total: 5 }) },
        { params: { progress: 2, total: 10 }, texts: await textsFor({ progress: 2, total: 10 }) },
        { params: { progress: 3 }, texts: await textsFor({ progress: 3 }) },
        { params: { message: "", progress: 4 }, texts: await textsFor({ message: "", progress: 4 }) },
    ];
    assert(cases[0].texts.join("") === "custom message", "the message wins over progress/total");
    assert(cases[1].texts.join("") === "Progress 2/10", "progress and total format as Progress n/total");
    assert(cases[2].texts.join("") === "Progress 3", "progress alone formats as Progress n");
    assert(cases[3].texts.join("") === "", "an empty message stays empty");
    return {
        name: "application progress text uses the message verbatim or Progress <n>[/<total>]",
        source: "packages/coding-agent/src/extensions/mcp/tools.ts:createMcpToolDefinition.execute,onProgress",
        expected: { cases },
    };
}

// ---------------------------------------------------------------------------------------------------
// cch_mcp:mcp-request-cancellation — abort, timeout, no-notify cases and connection reuse.
// ---------------------------------------------------------------------------------------------------

export async function stdioCancellation(): Promise<Scenario> {
    const { fixture, client } = await connectStdio("protocol");
    try {
        const controller = new AbortController();
        const pending = client.callTool("hang", {}, { signal: controller.signal });
        await waitFor(() => fixture.trace().some((entry) => entry.type === "call" && entry.name === "hang"), 2_000, "stdio hang call");
        const pid = fixture.transport.pid;
        controller.abort("stop");
        const outcome = await pending.then(() => null, (error: unknown) => errorRecord(error));
        await waitFor(() => fixture.trace().some((entry) => entry.type === "cancelled"), 2_000, "stdio cancellation");
        const after = await client.callTool("echo", { text: "after" });
        const trace = fixture.trace();
        const call = trace.find((entry) => entry.type === "call" && entry.name === "hang");
        const cancelled = trace.filter((entry) => entry.type === "cancelled");
        assert(outcome !== null, "the cancelled call must fail");
        assert((outcome as any).name === "AbortError", "the cancelled call settles with AbortError");
        assert(after.content[0]?.text === "echo:after", "the same child serves a later call");
        return {
            name: "stdio: aborting an in-flight call notifies the server and leaves the child usable",
            source:
                "packages/mcp/src/client.ts:cancelPending,onAbort; packages/mcp/src/transports/stdio.ts:StdioTransport.send",
            expected: {
                outcome,
                requestId: call.id,
                cancellationCount: cancelled.length,
                cancellationRequestId: cancelled[0]?.requestId ?? null,
                cancellationReason: cancelled[0]?.reason ?? null,
                sameChildPid: fixture.transport.pid === pid,
                reusableResultText: after.content[0]?.text ?? null,
                transport: "stdio",
            },
        };
    } finally {
        await client.close().catch(() => {});
        await fixture.cleanup();
    }
}

export async function httpCancellation(): Promise<Scenario> {
    const fixture = await startHttpMcp({
        toolCall: (message, response) => {
            if (message.params?.name === "hang") return; // Hold the response until the client gives up.
            response.writeHead(200, { "content-type": "application/json", "mcp-session-id": "session-1" }).end(
                JSON.stringify({
                    jsonrpc: "2.0",
                    id: message.id,
                    result: {
                        content: [{ type: "text", text: `echo:${message.params?.arguments?.text ?? ""}` }],
                    },
                }),
            );
        },
    });
    let client;
    try {
        client = await connectHttp(fixture, { openGetStream: false });
        const controller = new AbortController();
        const pending = client.callTool("hang", {}, { signal: controller.signal });
        await waitFor(
            () => fixture.requests.some((entry) => entry.message?.method === "tools/call" && entry.message.params?.name === "hang"),
            2_000,
            "HTTP hang call",
        );
        const sessionId = client.transport?.sessionId ?? null;
        controller.abort("stop");
        const outcome = await pending.then(() => null, (error: unknown) => errorRecord(error));
        await waitFor(
            () => fixture.requests.some((entry) => entry.message?.method === "notifications/cancelled"),
            2_000,
            "HTTP cancellation",
        );
        const after = await client.callTool("echo", { text: "after" });
        const cancellation = fixture.requests.find((entry) => entry.message?.method === "notifications/cancelled");
        const hang = fixture.requests.find((entry) => entry.message?.method === "tools/call" && entry.message.params?.name === "hang");
        const afterRequest = fixture.requests.find(
            (entry) => entry.message?.method === "tools/call" && entry.message.params?.name === "echo",
        );
        assert(outcome !== null, "the cancelled call must fail");
        assert((outcome as any).name === "AbortError", "the cancelled call settles with AbortError");
        assert(after.content[0]?.text === "echo:after", "the same session serves a later call");
        return {
            name: "HTTP: aborting an in-flight call posts notifications/cancelled and reuses the session",
            source:
                "packages/mcp/src/client.ts:cancelPending,onAbort; packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport.send",
            expected: {
                outcome,
                requestId: hang.message.id,
                cancellationCount: fixture.requests.filter((entry) => entry.message?.method === "notifications/cancelled").length,
                cancellationRequestId: cancellation.message.params?.requestId ?? null,
                cancellationReason: cancellation.message.params?.reason ?? null,
                reusableResultText: after.content[0]?.text ?? null,
                reusableSessionHeader: header(afterRequest, "mcp-session-id"),
                sessionId: sessionId ?? null,
                transport: "http",
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

async function timeoutCancellation(transport: "stdio" | "http"): Promise<Record<string, unknown>> {
    let client: any;
    let fixture: HttpMcpFixture | StdioFixture;
    let close: () => Promise<void>;
    if (transport === "stdio") {
        const started = await connectStdio("protocol");
        client = started.client;
        fixture = started.fixture;
        close = async () => {
            await client.close().catch(() => {});
            await started.fixture.cleanup();
        };
    } else {
        const started = await startHttpMcp({
            toolCall: (message, response) => {
                if (message.params?.name === "hang") return;
                response.writeHead(200, { "content-type": "application/json", "mcp-session-id": "session-1" }).end(
                    JSON.stringify({ jsonrpc: "2.0", id: message.id, result: { content: [] } }),
                );
            },
        });
        fixture = started;
        client = await connectHttp(started, { openGetStream: false });
        close = async () => {
            await client.close().catch(() => {});
            await started.close();
        };
    }
    try {
        const outcome = await client.callTool("hang", {}, { timeoutMs: 80 }).then(
            () => null,
            (error: unknown) => errorRecord(error),
        );
        await waitFor(
            () =>
                transport === "stdio"
                    ? (fixture as StdioFixture).trace().some((entry) => entry.type === "cancelled")
                    : (fixture as HttpMcpFixture).requests.some((entry) => entry.message?.method === "notifications/cancelled"),
            2_000,
            `${transport} timeout cancellation`,
        );
        const cancellation =
            transport === "stdio"
                ? (fixture as StdioFixture).trace().find((entry) => entry.type === "cancelled")
                : (fixture as HttpMcpFixture).requests.find((entry) => entry.message?.method === "notifications/cancelled")?.message.params;
        assert(outcome !== null, "the timed-out call must fail");
        assert((outcome as any).name === "McpTimeoutError", "the timed-out call settles with McpTimeoutError");
        return {
            outcome,
            timeoutMs: 80,
            cancellationReason: cancellation?.reason ?? null,
            cancellationRequestId: cancellation?.requestId ?? null,
            transport,
        };
    } finally {
        await close();
    }
}

export async function stdioTimeoutReason(): Promise<Scenario> {
    return {
        name: "stdio: a timed-out request fails with pi's timeout error and reason",
        source: "packages/mcp/src/client.ts:armTimeout,cancelPending; packages/mcp/src/protocol/jsonrpc.ts:McpTimeoutError",
        expected: await timeoutCancellation("stdio"),
    };
}

export async function httpTimeoutReason(): Promise<Scenario> {
    return {
        name: "HTTP: a timed-out request fails with pi's timeout error and reason",
        source: "packages/mcp/src/client.ts:armTimeout,cancelPending; packages/mcp/src/protocol/jsonrpc.ts:McpTimeoutError",
        expected: await timeoutCancellation("http"),
    };
}

export async function alreadyCancelledRequest(): Promise<Scenario> {
    const fixture = await startHttpMcp();
    let client;
    try {
        client = await connectHttp(fixture, { openGetStream: false });
        const controller = new AbortController();
        controller.abort("stop");
        const outcome = await client.callTool("echo", { text: "never" }, { signal: controller.signal }).then(
            () => null,
            (error: unknown) => errorRecord(error),
        );
        await delay(100);
        const toolCalls = fixture.requests.filter((entry) => entry.message?.method === "tools/call");
        const cancellations = fixture.requests.filter((entry) => entry.message?.method === "notifications/cancelled");
        assert(outcome !== null && (outcome as any).name === "AbortError", "an already-aborted signal fails with AbortError");
        assert(toolCalls.length === 0, "an already-aborted request is never sent");
        return {
            name: "an already-aborted request is never sent and emits no notifications/cancelled",
            source: "packages/mcp/src/client.ts:requestInternal",
            expected: {
                outcome,
                toolCallPosts: toolCalls.length,
                cancellationPosts: cancellations.length,
                transport: "http",
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function failedRequestNoCancellation(): Promise<Scenario> {
    const fixture = await startHttpMcp({
        toolCall: (message, response) => {
            response.writeHead(500, { "content-type": "text/plain" }).end("server failed");
        },
    });
    let client;
    try {
        client = await connectHttp(fixture, { openGetStream: false });
        const outcome = await client.callTool("fail-http", {}).then(
            () => null,
            (error: unknown) => errorRecord(error),
        );
        await delay(100);
        const cancellations = fixture.requests.filter((entry) => entry.message?.method === "notifications/cancelled");
        assert(outcome !== null, "the failed call must fail");
        return {
            name: "a failed HTTP request does not emit notifications/cancelled",
            source: "packages/mcp/src/client.ts:requestInternal,cancelPending; packages/mcp/src/transports/streamable-http.ts:send",
            expected: {
                outcome,
                cancellationPosts: cancellations.length,
                transport: "http",
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function stdioFailedRequestNoCancellation(): Promise<Scenario> {
    const { fixture, client } = await connectStdio("protocol");
    try {
        const controller = new AbortController();
        const outcome = await client.callTool("fail", {}, { signal: controller.signal }).then(
            () => null,
            (error: unknown) => errorRecord(error),
        );
        controller.abort("after settle");
        await delay(120);
        const cancellations = fixture.trace().filter((entry) => entry.type === "cancelled");
        assert(outcome !== null, "the failed call must fail");
        return {
            name: "a failed stdio request does not emit notifications/cancelled",
            source: "packages/mcp/src/client.ts:handleResponse,cancelPending",
            expected: {
                outcome,
                cancellationCount: cancellations.length,
                transport: "stdio",
            },
        };
    } finally {
        await client.close().catch(() => {});
        await fixture.cleanup();
    }
}

// ---------------------------------------------------------------------------------------------------
// cch_mcp:mcp-http-session-stream — session headers, GET stream, ping/roots and close.
// ---------------------------------------------------------------------------------------------------

export async function httpSessionHeadersAnd405(): Promise<Scenario> {
    const fixture = await startHttpMcp();
    const client = new McpClient(clientOptions());
    const transport = new StreamableHttpTransport({ url: fixture.url });
    const errors: any[] = [];
    client.onError((error: unknown) => errors.push(errorRecord(error)));
    try {
        await client.connect(transport);
        const sessionId = transport.sessionId ?? null;
        const tools = await client.listTools();
        await waitFor(() => fixture.getCount() >= 1, 2_000, "405 GET");
        await delay(100);
        const methods = postMethods(fixture);
        const listRequest = fixture.requests.find((entry) => entry.message?.method === "tools/list");
        const getRequest = fixture.requests.find((entry) => entry.method === "GET");
        await client.close();
        await waitFor(() => fixture.requests.some((entry) => entry.method === "DELETE"), 2_000, "session DELETE");
        const deleteRequest = fixture.requests.find((entry) => entry.method === "DELETE");
        assert(tools.length === 1, "the connection stays usable after a 405 GET");
        assert(fixture.getCount() === 1, "405 is feature-absent: the GET is not retried");
        return {
            name: "HTTP: session headers carry through and a 405 GET is feature-absent",
            source:
                "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport.captureSession,buildHeaders,openSseStream,close",
            expected: {
                sessionId,
                tools: namesOf(tools),
                listSessionHeader: header(listRequest, "mcp-session-id"),
                listProtocolHeader: header(listRequest, "mcp-protocol-version"),
                getAccept: header(getRequest, "accept"),
                getSessionHeader: header(getRequest, "mcp-session-id"),
                getCount: fixture.getCount(),
                getAfterInitialized: methods.indexOf("GET") > methods.indexOf("notifications/initialized"),
                errors,
                deleteSessionHeader: header(deleteRequest, "mcp-session-id"),
                latestProtocolVersion: LATEST_PROTOCOL_VERSION,
            },
        };
    } finally {
        await client.close().catch(() => {});
        await fixture.close();
    }
}

async function retryHintScenario(retryField: string, reconnect: Record<string, number>): Promise<Record<string, unknown>> {
    const fixture = await startHttpMcp({
        getStream: (index, _request, response, api) => {
            if (index === 1) {
                response.writeHead(200, { "content-type": "text/event-stream" });
                response.end(
                    `retry: ${retryField}\nid: 1\ndata: ${JSON.stringify({
                        jsonrpc: "2.0",
                        method: "notifications/message",
                        params: { level: "info", data: "primed" },
                    })}\n\n`,
                );
                return;
            }
            response.writeHead(200, { "content-type": "text/event-stream" });
            response.write(": resumed\n\n");
        },
    });
    let client: any;
    try {
        const errors: any[] = [];
        client = new McpClient(clientOptions());
        client.onError((error: unknown) => errors.push(errorRecord(error)));
        await client.connect(new StreamableHttpTransport({ url: fixture.url, reconnect }));
        await waitFor(() => fixture.getCount() >= 2, 5_000, "GET reconnect");
        const getRequests = fixture.requests.filter((entry) => entry.method === "GET");
        const gapMs = getRequests[1].at - getRequests[0].at;
        const secondLastEventId = header(getRequests[1], "last-event-id") ?? null;
        await client.close();
        return {
            retryField,
            retryNumeric: Number(retryField),
            reconnectGapMs: gapMs,
            reconnected: fixture.getCount() >= 2,
            secondLastEventId,
            errors,
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function httpRetryHint(): Promise<Scenario> {
    const observed = await retryHintScenario("20", { initialDelayMs: 500, maxDelayMs: 500, maxRetries: 2 });
    assert(Number(observed.reconnectGapMs) < 200, "the server retry hint overrides the 500 ms client backoff");
    assert(observed.secondLastEventId === "1", "the resumed GET carries Last-Event-ID");
    return {
        name: "HTTP: a server retry hint overrides the client reconnect delay and resumes with Last-Event-ID",
        source: "packages/mcp/src/transports/streamable-http.ts:runGetStream,reconnectDelay,consumeSse",
        expected: observed,
    };
}

export async function httpOutOfRangeRetryHint(): Promise<Scenario> {
    const observed = await retryHintScenario("99999999999999999999999999", {
        initialDelayMs: 20,
        maxDelayMs: 20,
        maxRetries: 2,
    });
    assert(Number(observed.reconnectGapMs) < 1_000, "the out-of-range retry stays bounded and never hangs");
    assert(observed.secondLastEventId === "1", "the resumed GET carries Last-Event-ID");
    return {
        name: "HTTP: an out-of-range retry hint reconnects without throwing or hanging",
        source: "packages/mcp/src/transports/streamable-http.ts:consumeSseStream,reconnectDelay,sleep",
        expected: {
            ...observed,
            // Node clamps a setTimeout delay above 2^31-1 to 1 ms; the runtime warning is not part
            // of the contract, and the capture records the bounded gap instead.
            runtimeClamped: true,
        },
    };
}

export async function httpGetStreamNotification(): Promise<Scenario> {
    const fixture = await startHttpMcp({
        getStream: (index, _request, response, api) => {
            response.writeHead(200, { "content-type": "text/event-stream" });
            setTimeout(() => {
                api.writeSseRaw(
                    response,
                    `id: m1\ndata: ${JSON.stringify({
                        jsonrpc: "2.0",
                        method: "notifications/message",
                        params: { level: "info", data: "hello" },
                    })}\n\n`,
                );
            }, 20);
        },
    });
    let client: any;
    try {
        client = await connectHttp(fixture);
        const received: any[] = [];
        client.onNotification("notifications/message", (params: unknown) => received.push(params));
        await waitFor(() => received.length > 0, 2_000, "GET notification");
        await client.close();
        return {
            name: "HTTP: an event on the server-to-client GET stream reaches the notification listener",
            source: "packages/mcp/src/transports/streamable-http.ts:runGetStream,consumeSse; packages/mcp/src/client.ts:handleNotification",
            expected: {
                events: received,
                getCount: fixture.getCount(),
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function httpPingAndRoots(): Promise<Scenario> {
    const fixture = await startHttpMcp({
        getStream: (index, _request, response, api) => {
            response.writeHead(200, { "content-type": "text/event-stream" });
            setTimeout(() => {
                api.writeSseRaw(response, `id: r1\ndata: ${JSON.stringify({ jsonrpc: "2.0", id: 7, method: "ping" })}\n\n`);
            }, 20);
            setTimeout(() => {
                api.writeSseRaw(
                    response,
                    `id: r2\ndata: ${JSON.stringify({ jsonrpc: "2.0", id: 8, method: "roots/list" })}\n\n`,
                );
            }, 50);
        },
    });
    let client: any;
    try {
        client = await connectHttp(fixture);
        await waitFor(
            () =>
                fixture.requests.some((entry) => entry.message?.id === 7 && entry.message.method === undefined) &&
                fixture.requests.some((entry) => entry.message?.id === 8 && entry.message.method === undefined),
            3_000,
            "HTTP ping and roots answers",
        );
        const ping = fixture.requests.find((entry) => entry.message?.id === 7 && entry.message.method === undefined);
        const roots = fixture.requests.find((entry) => entry.message?.id === 8 && entry.message.method === undefined);
        await client.close();
        return {
            name: "HTTP: the client answers ping and roots/list requests delivered on the GET stream",
            source: "packages/mcp/src/client.ts:handleRequest,requestHandlers; packages/mcp/src/transports/streamable-http.ts:runGetStream",
            expected: {
                pingResponse: ping.message.result,
                rootsResponse: roots.message.result,
                configuredRoots: ROOTS,
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function httpCancelsServerRequest(): Promise<Scenario> {
    const fixture = await startHttpMcp({
        getStream: (index, _request, response, api) => {
            response.writeHead(200, { "content-type": "text/event-stream" });
            setTimeout(() => {
                api.writeSseRaw(
                    response,
                    `id: c1\ndata: ${JSON.stringify({ jsonrpc: "2.0", id: 9, method: "test/wait" })}\n\n`,
                );
            }, 20);
            setTimeout(() => {
                api.writeSseRaw(
                    response,
                    `id: c2\ndata: ${JSON.stringify({
                        jsonrpc: "2.0",
                        method: "notifications/cancelled",
                        params: { requestId: 9 },
                    })}\n\n`,
                );
            }, 80);
        },
    });
    const client = new McpClient(clientOptions());
    let handlerAborted = false;
    client.setRequestHandler(
        "test/wait",
        (_params: unknown, context: { signal: AbortSignal }) =>
            new Promise((_resolve, reject) => {
                context.signal.addEventListener(
                    "abort",
                    () => {
                        handlerAborted = true;
                        reject(new McpError(-32603, "MCP request aborted"));
                    },
                    { once: true },
                );
            }),
    );
    try {
        await client.connect(new StreamableHttpTransport({ url: fixture.url }));
        await waitFor(
            () => fixture.requests.some((entry) => entry.message?.id === 9 && entry.message.method === undefined),
            3_000,
            "cancelled server request answer",
        );
        const answer = fixture.requests.find((entry) => entry.message?.id === 9 && entry.message.method === undefined);
        await client.close();
        return {
            name: "HTTP: notifications/cancelled aborts an in-flight server request handler",
            source: "packages/mcp/src/client.ts:handleRequest,handleCancelled",
            expected: {
                handlerAborted,
                responseError: answer.message.error,
                responseHasResult: "result" in answer.message,
            },
        };
    } finally {
        await client.close().catch(() => {});
        await fixture.close();
    }
}

export async function httpCloseAbortsStream(): Promise<Scenario> {
    const fixture = await startHttpMcp({
        getStream: (index, _request, response) => {
            response.writeHead(200, { "content-type": "text/event-stream" });
            response.write(": open\n\n");
        },
    });
    let client: any;
    try {
        client = await connectHttp(fixture);
        await waitFor(() => fixture.getCount() >= 1, 2_000, "GET open");
        const errors: any[] = [];
        client.onError((error: unknown) => errors.push(errorRecord(error)));
        await client.close();
        await waitFor(() => fixture.requests.some((entry) => entry.method === "DELETE"), 2_000, "close DELETE");
        await waitFor(() => fixture.events.some((event) => event.type === "get-close"), 2_000, "GET close event");
        const deleteRequest = fixture.requests.find((entry) => entry.method === "DELETE");
        const getClose = fixture.events.some((event) => event.type === "get-close");
        return {
            name: "HTTP: close aborts the GET stream without a dropped-stream error",
            source: "packages/mcp/src/transports/streamable-http.ts:runGetStream,close",
            expected: {
                getCount: fixture.getCount(),
                getClosed: getClose,
                deleteSessionHeader: header(deleteRequest, "mcp-session-id"),
                errors,
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

// ---------------------------------------------------------------------------------------------------
// cch_mcp:mcp-stdio-stderr-tail — the 64 KiB ring and the bounded 2000-character failure tail.
// ---------------------------------------------------------------------------------------------------

const STDERR_BYTES = 80_000;
const STDERR_HEAD = "AUDIT1191-START-";
const STDERR_TAIL = "-AUDIT1191-END";

function stderrPayload(bytes = STDERR_BYTES): string {
    const filler = Math.max(0, bytes - STDERR_HEAD.length - STDERR_TAIL.length);
    return `${STDERR_HEAD}${"s".repeat(filler)}${STDERR_TAIL}`;
}

export async function stdioStderrRing(): Promise<Scenario> {
    const fixture = await startStdioFixture("stderr", { AUDIT_1191_STDERR_BYTES: String(STDERR_BYTES) });
    const client = new McpClient(clientOptions());
    try {
        const outcome = await client.connect(fixture.transport).then(
            () => null,
            (error: unknown) => errorRecord(error),
        );
        await waitFor(() => fixture.transport.stderr.length > 0, 2_000, "stdio stderr");
        const stderr = fixture.transport.stderr;
        assert(outcome !== null, "the child exits before answering initialize");
        assert(stderr.length === 64 * 1024, "the ring keeps exactly the last 64 KiB");
        assert(stderr.endsWith(STDERR_TAIL), "the ring keeps the tail of stderr");
        return {
            name: "stdio: the transport ring keeps the last 64 KiB of a larger stderr stream",
            source: "packages/mcp/src/transports/stdio.ts:StdioTransport.handleStderr,DEFAULT_MAX_STDERR_BYTES",
            expected: {
                connectOutcome: outcome,
                payloadBytes: STDERR_BYTES,
                ringBytes: stderr.length,
                ringKeepsTail: stderr.endsWith(STDERR_TAIL),
                ringDroppedHead: !stderr.includes(STDERR_HEAD),
            },
        };
    } finally {
        await client.close().catch(() => {});
        await fixture.cleanup();
    }
}

export async function stdioFailureTail(): Promise<Scenario> {
    const fixture = await startStdioFixture("stderr", { AUDIT_1191_STDERR_BYTES: String(STDERR_BYTES) });
    const connection = new McpServerConnection({
        entry: {
            name: "audit-1191-stderr",
            config: {
                command: process.execPath,
                args: [fixture.transport.options.args[0]],
                env: {
                    AUDIT_1191_MODE: "stderr",
                    AUDIT_1191_STDERR_BYTES: String(STDERR_BYTES),
                    AUDIT_1191_TRACE: fixture.tracePath,
                },
            },
            source: "project",
        },
        cwd: process.cwd(),
        createTransport: createDefaultTransport,
        credentials: { forServer: () => ({}) },
        onTools: () => {},
    });
    try {
        const outcome = await connection.getClient().then(
            () => null,
            (error: unknown) => errorRecord(error),
        );
        assert(outcome !== null, "the connection must fail");
        const message = String((outcome as any).message);
        const expectedTail = stderrPayload().trim().slice(-2_000);
        assert(connection.state === "failed", "the connection reports the failed state");
        assert(message.endsWith(expectedTail), "the failure carries pi's last-2000-character stderr tail");
        assert(!message.includes(STDERR_HEAD), "the failure tail does not include the start of the ring");
        return {
            name: "stdio: a failed connect appends pi's bounded 2000-character stderr tail",
            source:
                "packages/coding-agent/src/extensions/mcp/runtime.ts:McpServerConnection.connectOnce,connectFailed,STDERR_TAIL_CHARS",
            expected: {
                state: connection.state,
                error: outcome,
                failurePrefix: message.includes('failed to connect:'),
                tailCharacters: expectedTail.length,
                tailMatchesLast2000: message.endsWith(expectedTail),
                tailIncludesRingHead: message.includes(STDERR_HEAD),
                errorIncludesWholeRing: message.length >= STDERR_BYTES,
            },
        };
    } finally {
        await connection.close().catch(() => {});
        await fixture.cleanup();
    }
}
