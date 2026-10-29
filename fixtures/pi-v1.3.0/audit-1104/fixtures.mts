/**
 * Local fixtures and host doubles for the `audit-1104` management-during-connection capture.
 *
 * The servers are the same shapes audit-1052 used: a scripted in-memory MCP server that can delay
 * or never answer `initialize`, and an OAuth-protected MCP server that implements the frozen pi
 * v1.3.0 discovery/registration/authorize/token surface. The extension host doubles record the
 * registered command, the session event handlers and the UI notifications; the headless manager
 * host runs the real frozen `McpManagerView` against a fake terminal (no PTY), feeding raw key
 * sequences exactly like pi's own manager-view tests.
 */

import { createHash } from "node:crypto";
import { createServer as createHttpServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import {
    FileAuthStorageBackend,
    KeybindingsManager,
    McpManagerView,
    McpOAuthCredentialStore,
    createInMemoryTransportPair,
    initTheme,
    isJsonRpcRequest,
    LATEST_PROTOCOL_VERSION,
    setKeybindings,
    theme,
} from "./pi.mts";

async function readBody(request: IncomingMessage): Promise<string> {
    const chunks: Buffer[] = [];
    for await (const chunk of request) chunks.push(Buffer.from(chunk));
    return Buffer.concat(chunks).toString("utf8");
}

function sendJson(response: ServerResponse, status: number, body: unknown, headers: Record<string, string> = {}): void {
    response.writeHead(status, { "content-type": "application/json", ...headers }).end(JSON.stringify(body));
}

// ── Scripted MCP servers ────────────────────────────────────────────────────

export interface InMemoryMcpServerOptions {
    initializeDelayMs?: number;
    neverInitialize?: boolean;
}

/** One scripted MCP server over pi's public in-memory transport; it records every request method. */
export async function startInMemoryMcpServer(options: InMemoryMcpServerOptions = {}) {
    const methods: string[] = [];
    const pair = createInMemoryTransportPair();
    await pair.server.start();
    pair.server.onMessage((message: unknown) => {
        if (!isJsonRpcRequest(message)) return;
        methods.push(message.method);
        if (options.neverInitialize && message.method === "initialize") return;
        const respond = () => {
            let result: unknown = {};
            if (message.method === "initialize") {
                result = {
                    protocolVersion: LATEST_PROTOCOL_VERSION,
                    capabilities: { tools: {} },
                    serverInfo: { name: "scripted", version: "1.0.0" },
                };
            } else if (message.method === "tools/list") {
                result = { tools: [{ name: "echo", inputSchema: { type: "object", properties: {} } }] };
            }
            void pair.server.send({ jsonrpc: "2.0", id: message.id, result });
        };
        const delay = options.initializeDelayMs ?? 0;
        if (message.method === "initialize" && delay > 0) setTimeout(respond, delay);
        else queueMicrotask(respond);
    });
    return { methods, transport: pair.client, close: () => pair.client.close() };
}

/** The OAuth-protected MCP server and authorization server for the sign-in scenarios. */
export async function startOAuthMcpFixture() {
    const mcpRequests: Array<{ method: string; token: string | undefined; status: number }> = [];
    const tokenRequests: URLSearchParams[] = [];
    const challenges = new Map<string, string>();
    const validTokens = new Set<string>();
    let issued = 0;
    let origin = "";

    const issueTokens = () => {
        issued += 1;
        const tokens = { access_token: `access-${issued}`, refresh_token: `refresh-${issued}` };
        validTokens.add(tokens.access_token);
        return { ...tokens, token_type: "Bearer", expires_in: 3600 };
    };

    const handleMcp = async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
        const token = request.headers.authorization?.replace(/^Bearer /, "");
        if (request.method === "DELETE") {
            response.writeHead(200).end();
            return;
        }
        if (request.method === "GET") {
            response.writeHead(405).end();
            return;
        }
        const message = JSON.parse(await readBody(request)) as { id?: number; method: string };
        const status = token && validTokens.has(token) ? 200 : 401;
        mcpRequests.push({ method: message.method, token, status });
        if (status === 401) {
            response
                .writeHead(401, {
                    "www-authenticate": `Bearer resource_metadata="${origin}/.well-known/oauth-protected-resource/mcp"`,
                })
                .end();
            return;
        }
        if (message.id === undefined) {
            response.writeHead(202).end();
            return;
        }
        const result =
            message.method === "initialize"
                ? {
                      protocolVersion: LATEST_PROTOCOL_VERSION,
                      capabilities: { tools: {} },
                      serverInfo: { name: "fixture", version: "1.0.0" },
                  }
                : message.method === "tools/list"
                  ? { tools: [{ name: "echo", inputSchema: { type: "object", properties: {} } }] }
                  : {};
        sendJson(response, 200, { jsonrpc: "2.0", id: message.id, result }, { "mcp-session-id": "session-1" });
    };

    const handle = async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
        const url = new URL(request.url ?? "/", origin);
        switch (url.pathname) {
            case "/mcp":
                return handleMcp(request, response);
            case "/.well-known/oauth-protected-resource/mcp":
                return sendJson(response, 200, { resource: `${origin}/mcp`, authorization_servers: [origin] });
            case "/.well-known/oauth-authorization-server":
                return sendJson(response, 200, {
                    issuer: origin,
                    authorization_endpoint: `${origin}/authorize`,
                    token_endpoint: `${origin}/token`,
                    registration_endpoint: `${origin}/register`,
                    response_types_supported: ["code"],
                    code_challenge_methods_supported: ["S256"],
                    token_endpoint_auth_methods_supported: ["none"],
                });
            case "/register": {
                const metadata = JSON.parse(await readBody(request)) as Record<string, unknown>;
                return sendJson(response, 201, { ...metadata, client_id: "client-1" });
            }
            case "/authorize": {
                const code = `code-${challenges.size + 1}`;
                challenges.set(code, url.searchParams.get("code_challenge") ?? "");
                const redirect = new URL(url.searchParams.get("redirect_uri") ?? "");
                redirect.searchParams.set("code", code);
                redirect.searchParams.set("state", url.searchParams.get("state") ?? "");
                response.writeHead(302, { location: redirect.href }).end();
                return;
            }
            case "/token": {
                const params = new URLSearchParams(await readBody(request));
                tokenRequests.push(params);
                if (params.get("grant_type") === "authorization_code") {
                    const challenge = challenges.get(params.get("code") ?? "");
                    const verifier = createHash("sha256")
                        .update(params.get("code_verifier") ?? "")
                        .digest("base64url");
                    if (!challenge || challenge !== verifier) {
                        return sendJson(response, 400, { error: "invalid_grant" });
                    }
                    challenges.delete(params.get("code") ?? "");
                    return sendJson(response, 200, issueTokens());
                }
                return sendJson(response, 400, { error: "invalid_grant" });
            }
            default:
                response.writeHead(404).end();
        }
    };

    const server: Server = createHttpServer((request, response) => {
        void handle(request, response).catch((error: unknown) => {
            response.writeHead(500).end(String(error));
        });
    });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (!address || typeof address === "string") throw new Error("OAuth MCP fixture did not bind to TCP");
    origin = `http://127.0.0.1:${address.port}`;

    return {
        url: `${origin}/mcp`,
        origin,
        mcpRequests,
        tokenRequests,
        /**
         * What a browser would end up at after the user approves: performs the authorize step and
         * returns the redirect URL the user would paste back (the loopback callback itself is never
         * contacted, matching a browser running on another machine).
         */
        approve(authorizationUrl: string): string {
            const url = new URL(authorizationUrl);
            const redirect = new URL(url.searchParams.get("redirect_uri") ?? "");
            const code = `code-${challenges.size + 1}`;
            challenges.set(code, url.searchParams.get("code_challenge") ?? "");
            redirect.searchParams.set("code", code);
            redirect.searchParams.set("state", url.searchParams.get("state") ?? "");
            return redirect.href;
        },
        seedAccessToken(token: string): void {
            validTokens.add(token);
        },
        close(): Promise<void> {
            return new Promise((resolve) => {
                server.closeAllConnections();
                server.close(() => resolve());
            });
        },
    };
}

// ── Extension host doubles ──────────────────────────────────────────────────

export interface CommandDefinition {
    description: string;
    handler(args: string, ctx: unknown): Promise<void>;
}

export interface ApiStub {
    tools: Array<Record<string, unknown>>;
    active: string[];
    notifications: Array<{ message: string; level?: string }>;
    handlers: Map<string, Function>;
    commands: Map<string, CommandDefinition>;
    api: Record<string, unknown>;
}

/** pi's ExtensionAPI with the surface the MCP extension uses, recording every call. */
export function apiStub(): ApiStub {
    const result: ApiStub = { tools: [], active: [], notifications: [], handlers: new Map(), commands: new Map(), api: {} };
    result.api = {
        getMcpServers: () => [],
        registerToolRenderer: () => {},
        registerTool: (tool: Record<string, unknown>) => result.tools.push(tool),
        getAllTools: () => result.tools,
        getActiveTools: () => result.active,
        setActiveTools: (names: string[]) => {
            result.active.splice(0, result.active.length, ...names);
        },
        on: (event: string, handler: Function) => result.handlers.set(event, handler),
        registerCommand: (name: string, definition: CommandDefinition) => void result.commands.set(name, definition),
        registerMessageRenderer: () => {},
        events: { emit: () => {} },
    };
    return result;
}

export interface UiExtras {
    /** The headless manager host; when set, `ui.custom` runs the real McpManagerView on it. */
    manager?: HeadlessManagerHost;
}

export function extensionContext(stub: ApiStub, mode: "print" | "tui", extras: UiExtras = {}): Record<string, unknown> {
    return {
        cwd: "/tmp/audit-1104/workspace",
        hasUI: true,
        mode,
        signal: new AbortController().signal,
        ui: {
            notify: (message: string, level?: string) => void stub.notifications.push({ message, level }),
            input: async () => undefined,
            select: async () => undefined,
            custom: <T>(factory: (tui: unknown, themeValue: unknown, keybindings: unknown, done: (result: T) => void) => unknown) => {
                if (!extras.manager) throw new Error("ui.custom without a headless manager host");
                return extras.manager.run((host) => factory(host.tui, theme, new KeybindingsManager(), doneMarker<T>(host)));
            },
        },
    };
}

function doneMarker<T>(host: HeadlessManagerHost): (result: T) => void {
    return (result: T) => host.finish(result);
}

// ── Headless manager host ───────────────────────────────────────────────────

const stripAnsi = (text: string) => text.replace(/\x1b\[[0-9;]*m/g, "");

/**
 * The fake terminal the real frozen `McpManagerView` renders onto: `requestRender` is a no-op (the
 * capture reads frames on demand), and `press` feeds raw key sequences like pi's own view tests.
 */
export class HeadlessManagerHost {
    readonly tui = { requestRender: () => {} } as unknown;
    private view: McpManagerView | undefined;
    private finished: (() => void) | undefined;
    private result: unknown;

    /** Runs the `showMcpManager` factory: builds the real view and resolves when the panel closes. */
    async run<T>(factory: (host: HeadlessManagerHost) => unknown): Promise<T> {
        this.view = undefined;
        const produced = factory(this) as McpManagerView | Promise<McpManagerView>;
        this.view = produced instanceof Promise ? await produced : produced;
        await new Promise<void>((resolve) => {
            this.finished = resolve;
        });
        return this.result as T;
    }

    finish(result: unknown): void {
        this.result = result;
        const finish = this.finished;
        this.finished = undefined;
        finish?.();
    }

    /** One raw terminal input sequence: "\r" enter, "\x1b" escape, "\x1b[B" arrow down. */
    press(sequence: string): void {
        if (!this.view) throw new Error("the manager panel is not open");
        this.view.handleInput(sequence);
    }

    /** Types one string into the focused input (the sign-in redirect-URL screen). */
    type(text: string): void {
        if (!this.view) throw new Error("the manager panel is not open");
        for (const character of text) this.view.handleInput(character);
    }

    /** The current frame without styling, for observations. */
    frame(): string[] {
        if (!this.view) return [];
        return this.view.render(100).map(stripAnsi);
    }

    /** Wait until the frame has stayed unchanged for `stableMs` (no rebuilds in flight). */
    async waitForStable(stableMs = 250, timeoutMs = 10_000): Promise<void> {
        const start = Date.now();
        let last = "";
        let stableSince = Date.now();
        for (;;) {
            const frame = this.frame().join("\n");
            if (frame !== last) {
                last = frame;
                stableSince = Date.now();
            } else if (Date.now() - stableSince >= stableMs) {
                return;
            }
            if (Date.now() - start > timeoutMs) {
                throw new Error(`frame did not stabilize within ${timeoutMs}ms`);
            }
            await new Promise((resolve) => setImmediate(resolve));
        }
    }

    /** Polls the frame until `predicate` holds or the bound passes. */
    async waitFor(predicate: (frame: string[]) => boolean, what: string, boundMs = 5_000): Promise<string[]> {
        const start = Date.now();
        for (;;) {
            const frame = this.frame();
            if (predicate(frame)) return frame;
            if (Date.now() - start > boundMs) {
                throw new Error(`timed out waiting for ${what}; last frame:\n${frame.join("\n")}`);
            }
            await new Promise((resolve) => setTimeout(resolve, 10));
        }
    }
}

/** One shared credential store file, the shape `pi mcp login` in another process would write. */
export function credentialStore(path: string): McpOAuthCredentialStore {
    // The lock directory lives beside the credentials file so concurrent or killed runs cannot
    // contend on a shared lock path.
    return new McpOAuthCredentialStore(new FileAuthStorageBackend(path), `${path}.locks`);
}

export function prepareHeadlessTui(): void {
    initTheme("dark");
    setKeybindings(new KeybindingsManager());
}
