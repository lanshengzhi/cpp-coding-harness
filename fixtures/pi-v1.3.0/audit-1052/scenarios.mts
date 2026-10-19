/**
 * Scenarios for the `audit-1052` MCP capture. Each function drives frozen pi v1.3.0 through a public
 * seam and returns the observed values; `capture-mcp.mts` turns them into the manifest scenarios.
 * Fixtures are custom because the OAuth lifecycle needs a server that can stall one endpoint while
 * the real client completes, cancels, or closes the surrounding request.
 */

import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import {
    McpAuthRequiredError,
    McpClient,
    StreamableHttpTransport,
    createInMemoryTransportPair,
    isJsonRpcRequest,
    McpOAuthProvider,
    authorizeMcp,
    OAuthIssuerMismatchError,
    loadMcpConfig,
    InMemoryAuthStorageBackend,
    createMcpAuthProvider,
    McpOAuthCredentialStore,
    McpSignInCancelledError,
    signInMcpServer,
    convertMcpResult,
    createMcpExtension,
    McpServerConnection,
    type McpServerEntry,
} from "./pi.mts";
import {
    createSelfSignedCertificate,
    startInMemoryMcpServer,
    startMcpHttpFixture,
    startOAuthMcpFixture,
} from "./fixtures.mts";

export interface Scenario {
    name: string;
    source: string;
    expected: unknown;
}

const PI_VERSION = "1.3.0";
const checkoutRequire = createRequire(path.join(process.env.PI_CHECKOUT ?? "/tmp/cch-1052-pi-frozen", "package.json"));
const undici = checkoutRequire("undici") as {
    Agent: new (options: { connect: { ca: Buffer } }) => { close(): Promise<void> };
    fetch: typeof fetch;
};

export function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

function errorRecord(error: unknown): Record<string, unknown> {
    if (!(error instanceof Error)) return { name: "NonError", message: String(error) };
    const cause = error.cause instanceof Error ? error.cause : undefined;
    const code =
        cause && typeof (cause as NodeJS.ErrnoException).code === "string"
            ? (cause as NodeJS.ErrnoException).code
            : undefined;
    return {
        name: error.name,
        message: error.message.replace(/https?:\/\/127\.0\.0\.1:\d+/g, "<loopback>"),
        ...(code ? { causeCode: code } : {}),
    };
}

function rpcResult(id: number, result: unknown): unknown {
    return { jsonrpc: "2.0", id, result };
}

async function startScriptedClient(
    handler: (message: { id: number; method: string; params?: unknown }) => unknown,
): Promise<{ client: McpClient; close: () => Promise<void> }> {
    const pair = createInMemoryTransportPair();
    await pair.server.start();
    pair.server.onMessage((message: unknown) => {
        if (!isJsonRpcRequest(message)) return;
        if (message.method === "initialize") {
            void pair.server.send(
                rpcResult(message.id, {
                    protocolVersion: "2025-11-25",
                    capabilities: { tools: {} },
                    serverInfo: { name: "scripted", version: "1.0.0" },
                }),
            );
            return;
        }
        const response = handler(message as { id: number; method: string; params?: unknown });
        if (response !== undefined) void pair.server.send(response);
    });
    const client = new McpClient({ name: "pi", version: PI_VERSION, roots: [] });
    await client.connect(pair.client);
    return { client, close: () => client.close() };
}

// ---------------------------------------------------------------------------------------------------
// structuredContent: array, primitive, null and error results; object-only fixtures are not enough.
// ---------------------------------------------------------------------------------------------------

export async function structuredArray(): Promise<Scenario> {
    const { client, close } = await startScriptedClient((message) =>
        message.method === "tools/call"
            ? rpcResult(message.id, { content: [], structuredContent: [1, { x: true }, null] })
            : rpcResult(message.id, { tools: [] }),
    );
    try {
        const result = await client.callTool("array");
        assert(Array.isArray(result.structuredContent), "structuredContent must remain an array");
        assert(result.content.length === 0, "content defaults to an empty array");
        return {
            name: "client preserves an array structuredContent when content is absent",
            source: "packages/mcp/src/client.ts:McpClient.callTool,validateCallToolResult",
            expected: {
                structuredContent: result.structuredContent,
                content: result.content,
                isError: result.isError ?? null,
            },
        };
    } finally {
        await close();
    }
}

export async function structuredPrimitive(): Promise<Scenario> {
    const { client, close } = await startScriptedClient((message) =>
        message.method === "tools/call"
            ? rpcResult(message.id, { content: [], structuredContent: 7 })
            : rpcResult(message.id, { tools: [] }),
    );
    try {
        const result = await client.callTool("primitive");
        assert(result.structuredContent === 7, "structuredContent must remain the primitive 7");
        return {
            name: "client preserves a primitive structuredContent",
            source: "packages/mcp/src/client.ts:McpClient.callTool,validateCallToolResult",
            expected: { structuredContent: result.structuredContent, content: result.content, isError: result.isError ?? null },
        };
    } finally {
        await close();
    }
}

export async function structuredNull(): Promise<Scenario> {
    const { client, close } = await startScriptedClient((message) =>
        message.method === "tools/call"
            ? rpcResult(message.id, { content: [], structuredContent: null })
            : rpcResult(message.id, { tools: [] }),
    );
    try {
        const result = await client.callTool("null");
        assert(result.structuredContent === null, "structuredContent must remain null");
        return {
            name: "client preserves a null structuredContent",
            source: "packages/mcp/src/client.ts:McpClient.callTool,validateCallToolResult",
            expected: { structuredContent: result.structuredContent, content: result.content, isError: result.isError ?? null },
        };
    } finally {
        await close();
    }
}

export async function structuredErrorResult(): Promise<Scenario> {
    const { client, close } = await startScriptedClient((message) =>
        message.method === "tools/call"
            ? rpcResult(message.id, {
                  content: [{ type: "text", text: "tool failed" }],
                  isError: true,
                  _meta: { traceId: "t-1" },
              })
            : rpcResult(message.id, { tools: [] }),
    );
    try {
        const result = await client.callTool("error");
        assert(result.isError === true, "isError must be preserved");
        assert(result.content.length === 1, "content must be preserved");
        const converted = await convertMcpResult("fixture", "error", result);
        assert(converted.isError === true, "convertMcpResult must mark the error result");
        return {
            name: "client and tool conversion preserve an error result",
            source:
                "packages/mcp/src/client.ts:McpClient.callTool; packages/coding-agent/src/extensions/mcp/tools.ts:convertMcpResult",
            expected: {
                client: { isError: result.isError, content: result.content, meta: result._meta },
                converted: {
                    content: converted.content,
                    isError: converted.isError,
                    structuredContent: converted.structuredContent,
                },
            },
        };
    } finally {
        await close();
    }
}

export async function structuredConversion(): Promise<Scenario> {
    const { client, close } = await startScriptedClient((message) =>
        message.method === "tools/call"
            ? rpcResult(message.id, {
                  content: [],
                  structuredContent: [{ id: 1 }, { id: 2 }],
              })
            : rpcResult(message.id, { tools: [] }),
    );
    try {
        const result = await client.callTool("array");
        const converted = await convertMcpResult("fixture", "array", result);
        assert(converted.content.length === 1 && converted.content[0]?.type === "text", "structured content becomes text");
        return {
            name: "tool conversion serializes structuredContent into model text",
            source: "packages/coding-agent/src/extensions/mcp/tools.ts:convertMcpResult,createMcpResultSchema",
            expected: {
                content: converted.content,
                structuredContent: converted.structuredContent,
                isError: converted.isError ?? null,
            },
        };
    } finally {
        await close();
    }
}

// ---------------------------------------------------------------------------------------------------
// OAuth lifecycle: cancellation during refresh never dispatches the cancelled call; close never waits.
// ---------------------------------------------------------------------------------------------------

function oauthEntry(url: string): McpServerEntry {
    return { name: "server", config: { url, oauth: {} }, source: "/tmp/audit-1052/mcp.json" };
}

async function seededOAuth(
    serverUrl: string,
    options: { expireAt?: number; clientInformation?: boolean } = {},
): Promise<{ backend: InMemoryAuthStorageBackend; store: McpOAuthCredentialStore; provider: ReturnType<typeof createMcpAuthProvider> }> {
    const backend = new InMemoryAuthStorageBackend();
    const store = new McpOAuthCredentialStore(backend);
    await store.forServer("server", serverUrl).save({
        serverUrl,
        ...(options.clientInformation === false ? {} : { clientInformation: { client_id: "client-1" } }),
        tokens: {
            access_token: "access-0",
            token_type: "Bearer",
            refresh_token: "refresh-0",
            ...(options.expireAt === undefined ? {} : { expires_in: 3600 }),
        },
        ...(options.expireAt === undefined ? {} : { tokensExpireAt: options.expireAt }),
    });
    const provider = createMcpAuthProvider({
        serverUrl,
        store: store.forServer("server", serverUrl),
        settings: () => ({}),
        onChallenge: () => {},
    });
    return { backend, store, provider };
}

async function storedToken(backend: InMemoryAuthStorageBackend, serverUrl: string): Promise<string | undefined> {
    const store = new McpOAuthCredentialStore(backend);
    return (await store.forServer("server", serverUrl).load())?.tokens?.access_token;
}

async function connectOAuth(fixture: Awaited<ReturnType<typeof startOAuthMcpFixture>>, provider: ReturnType<typeof createMcpAuthProvider>) {
    const transport = new StreamableHttpTransport({ url: fixture.url, authProvider: provider });
    const client = new McpClient({ name: "pi", version: PI_VERSION, roots: [] });
    await client.connect(transport);
    return client;
}

export async function refreshCancellation(mode: "abort" | "timeout"): Promise<Scenario> {
    const fixture = await startOAuthMcpFixture();
    fixture.seedAccessToken("access-0");
    fixture.seedRefreshToken("refresh-0");
    const { backend, provider } = await seededOAuth(fixture.url);
    let client: McpClient | undefined;
    try {
        client = await connectOAuth(fixture, provider);
        fixture.expireAccessTokens();
        fixture.stall.add("/token");
        const controller = new AbortController();
        const call = client.callTool("echo", {}, {
            ...(mode === "abort" ? { signal: controller.signal } : { timeoutMs: 250 }),
        });
        await fixture.waitForStall("/token");
        const started = Date.now();
        if (mode === "abort") controller.abort("test cancellation");
        let error: unknown;
        try {
            await call;
        } catch (caught) {
            error = caught;
        }
        const elapsedMs = Date.now() - started;
        assert(error !== undefined, "the cancelled call must reject");
        assert(
            mode === "abort"
                ? (error as Error).name === "AbortError" && (error as Error).message === "MCP request aborted"
                : (error as Error).name === "McpTimeoutError",
            "expected the documented cancellation error",
        );
        // Let the refresh answer and the cancelled request's token path finish.
        fixture.releaseAll();
        await new Promise((resolve) => setTimeout(resolve, 100));
        const afterRefresh = fixture.mcpRequests.filter((request) => request.method === "tools/call");
        const tokenRequestsAfterRelease = fixture.tokenRequests.length;
        const accessTokenAfterRelease = await storedToken(backend, fixture.url);
        assert(afterRefresh.length === 1, "the cancelled call must reach tools/call only once (the 401 attempt)");
        assert(afterRefresh[0]?.status === 401, "the only tool call is the initial 401 attempt");
        return {
            name:
                mode === "abort"
                    ? "cancelling a tool call during OAuth refresh never sends it after the refresh completes"
                    : "a tool call that times out during OAuth refresh never sends it after the refresh completes",
            source:
                "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport.authorizedFetch; packages/coding-agent/src/extensions/mcp/oauth.ts:createMcpAuthProvider",
            expected: {
                mode,
                error: errorRecord(error),
                elapsedMs: elapsedMs <= 1000 ? "<1s" : ">=1s",
                refreshRequests: tokenRequestsAfterRelease,
                refreshResponses: fixture.tokenResponses.length,
                toolCalls: afterRefresh,
                toolCallsAfterRefresh: 0,
                storedAccessTokenAfterRefresh: accessTokenAfterRelease,
            },
        };
    } finally {
        fixture.releaseAll();
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function closeWithoutRefresh(): Promise<Scenario> {
    const fixture = await startOAuthMcpFixture();
    fixture.seedAccessToken("access-0");
    fixture.seedRefreshToken("refresh-0");
    const { store, provider } = await seededOAuth(fixture.url);
    let client: McpClient | undefined;
    try {
        client = await connectOAuth(fixture, provider);
        // Connect used the plain token. Make it expire within REFRESH_SKEW_MS so the next token() refreshes.
        const current = await store.forServer("server", fixture.url).load();
        assert(current, "the seeded OAuth state must load");
        await store.forServer("server", fixture.url).save({ ...current, tokensExpireAt: Date.now() + 1_000 });
        fixture.stall.add("/token");
        const refresh = provider.token();
        await fixture.waitForStall("/token");
        const start = Date.now();
        await client.close();
        const closeMs = Date.now() - start;
        const closeReturnedWhileRefreshStalled = !fixture.tokenResponses.some((response) => response.status === 200);
        assert(closeMs < 1_000, "close must not wait for the stalled refresh");
        assert(closeReturnedWhileRefreshStalled, "refresh must still be stalled");
        assert(fixture.log.includes("delete access-0"), "DELETE must reuse the last token");
        fixture.releaseAll();
        await provider.settled();
        await refresh;
        return {
            name: "closing while a token refresh is in flight does not wait for the refresh",
            source:
                "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport.close; packages/coding-agent/src/extensions/mcp/oauth.ts:createMcpAuthProvider",
            expected: {
                closeMs: closeMs <= 250 ? "<=250ms" : ">250ms",
                closeReturnedWhileRefreshStalled,
                refreshRequests: fixture.tokenRequests.length,
                refreshCompleted: fixture.tokenResponses.some((response) => response.status === 200),
                deleteToken: fixture.log.find((line) => line.startsWith("delete "))?.slice("delete ".length),
            },
        };
    } finally {
        fixture.releaseAll();
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function issuerMismatch(): Promise<Scenario> {
    const fixture = await startOAuthMcpFixture();
    try {
        const backend = new InMemoryAuthStorageBackend();
        const store = new McpOAuthCredentialStore(backend);
        await store.forServer("server", fixture.url).save({
            serverUrl: fixture.url,
            clientInformation: { client_id: "client-1" },
        });
        let error: unknown;
        try {
            await authorizeMcp(
                new McpOAuthProvider({
                    serverUrl: fixture.url,
                    redirectUrl: "http://127.0.0.1:7777/callback",
                    clientMetadata: { client_name: "pi", redirect_uris: ["http://127.0.0.1:7777/callback"] },
                    store: store.forServer("server", fixture.url),
                    onRedirect: () => {},
                }),
                {
                    serverUrl: fixture.url,
                    authorizationCode: "code-forged",
                    iss: "https://attacker.example",
                },
            );
        } catch (caught) {
            error = caught;
        }
        assert(error instanceof OAuthIssuerMismatchError, "forged iss must be rejected as OAuthIssuerMismatchError");
        return {
            name: "authorization response from another issuer is rejected before token exchange",
            source: "packages/mcp/src/oauth/flow.ts:authorizeMcp,validateIssuer",
            expected: {
                error: errorRecord(error),
                expectedIssuer: "<loopback>",
                receivedIssuer: "https://attacker.example",
                issuerMatchesServer: false,
                tokenRequests: fixture.tokenRequests.length,
                authorizationRequests: fixture.authorizations.length,
            },
        };
    } finally {
        await fixture.close();
    }
}

export async function discoveryCancellation(): Promise<Scenario> {
    const fixture = await startOAuthMcpFixture();
    try {
        const store = new InMemoryAuthStorageBackend();
        const credentialStore = new McpOAuthCredentialStore(store);
        fixture.stall.add("/.well-known/oauth-protected-resource/mcp");
        const controller = new AbortController();
        const flow = authorizeMcp(
            new McpOAuthProvider({
                serverUrl: fixture.url,
                redirectUrl: "http://127.0.0.1:7777/callback",
                clientMetadata: { client_name: "pi", redirect_uris: ["http://127.0.0.1:7777/callback"] },
                store: credentialStore.forServer("server", fixture.url),
                onRedirect: () => {},
            }),
            { serverUrl: fixture.url, signal: controller.signal },
        );
        const stalled = await fixture.waitForStall("/.well-known/oauth-protected-resource/mcp");
        controller.abort();
        let error: unknown;
        try {
            await flow;
        } catch (caught) {
            error = caught;
        }
        assert(error !== undefined, "discovery cancellation must reject");
        await new Promise((resolve) => setTimeout(resolve, 20));
        return {
            name: "cancellation during protected-resource discovery aborts the flow",
            source: "packages/mcp/src/oauth/discovery.ts:discoverOAuthServerInfo; packages/mcp/src/oauth/flow.ts:authorizeMcp",
            expected: {
                error: errorRecord(error),
                stalledPath: stalled.path,
                requestSocketDestroyed: stalled.request.socket.destroyed,
                tokenRequests: fixture.tokenRequests.length,
                registrations: fixture.registrations.length,
            },
        };
    } finally {
        fixture.releaseAll();
        await fixture.close();
    }
}

export async function registrationCancellation(): Promise<Scenario> {
    const fixture = await startOAuthMcpFixture();
    try {
        const store = new InMemoryAuthStorageBackend();
        const credentialStore = new McpOAuthCredentialStore(store);
        fixture.stall.add("/register");
        const controller = new AbortController();
        const flow = authorizeMcp(
            new McpOAuthProvider({
                serverUrl: fixture.url,
                redirectUrl: "http://127.0.0.1:7777/callback",
                clientMetadata: { client_name: "pi", redirect_uris: ["http://127.0.0.1:7777/callback"] },
                store: credentialStore.forServer("server", fixture.url),
                onRedirect: () => {},
            }),
            { serverUrl: fixture.url, signal: controller.signal },
        );
        const stalled = await fixture.waitForStall("/register");
        controller.abort();
        let error: unknown;
        try {
            await flow;
        } catch (caught) {
            error = caught;
        }
        assert(error !== undefined, "registration cancellation must reject");
        return {
            name: "cancellation during dynamic client registration aborts the flow",
            source: "packages/mcp/src/oauth/flow.ts:authorizeMcp,registerClient",
            expected: {
                error: errorRecord(error),
                stalledPath: stalled.path,
                tokenRequests: fixture.tokenRequests.length,
                authorizations: fixture.authorizations.length,
            },
        };
    } finally {
        fixture.releaseAll();
        await fixture.close();
    }
}

export async function signInCancellation(): Promise<Scenario> {
    const fixture = await startOAuthMcpFixture();
    try {
        const backend = new InMemoryAuthStorageBackend();
        const store = new McpOAuthCredentialStore(backend);
        const controller = new AbortController();
        let shownUrl: URL | undefined;
        const signIn = signInMcpServer({
            serverUrl: fixture.url,
            store: store.forServer("server", fixture.url),
            settings: {},
            signal: controller.signal,
            prompt: {
                showAuthorizationUrl: (url) => {
                    shownUrl = url;
                },
                promptForRedirectUrl: (signal) =>
                    new Promise((resolve, reject) => {
                        signal.addEventListener("abort", () => reject(new McpSignInCancelledError()), { once: true });
                    }),
            },
        });
        // Registration and the authorization redirect happen before the prompt; wait for the prompt.
        for (let index = 0; index < 200 && !shownUrl; index++) await new Promise((resolve) => setTimeout(resolve, 5));
        assert(shownUrl, "the authorization URL must be shown before the prompt");
        controller.abort("test cancellation");
        let error: unknown;
        try {
            await signIn;
        } catch (caught) {
            error = caught;
        }
        assert(error instanceof McpSignInCancelledError, "sign-in cancellation must produce McpSignInCancelledError");
        return {
            name: "cancellation while waiting for the browser callback stops sign-in",
            source: "packages/coding-agent/src/extensions/mcp/oauth.ts:signInMcpServer,listenForCallback",
            expected: {
                error: errorRecord(error),
                authorizationUrlShown: true,
                authorizationUrl: {
                    origin: "<issuer>",
                    path: shownUrl.pathname,
                    queryKeys: [...shownUrl.searchParams.keys()],
                    redirectUri: shownUrl.searchParams.get("redirect_uri")?.replace(/127\.0\.0\.1:\d+/, "<loopback>"),
                    codeChallengeMethod: shownUrl.searchParams.get("code_challenge_method"),
                    hasState: shownUrl.searchParams.has("state"),
                },
                authorizationRequests: fixture.authorizations.length,
                registrations: fixture.registrations.length,
                tokenRequests: fixture.tokenRequests.length,
                pkceChallengePresent: Boolean(shownUrl.searchParams.get("code_challenge")),
            },
        };
    } finally {
        await fixture.close();
    }
}

// ---------------------------------------------------------------------------------------------------
// Transports: plain HTTP, HTTPS validation, same- and cross-origin redirects, request-time auth.
// ---------------------------------------------------------------------------------------------------

export function configUrlPolicy(): Scenario {
    const directory = mkdtempSync(path.join(tmpdir(), "audit-1052-config-"));
    try {
        writeFileSync(
            path.join(directory, "mcp.json"),
            JSON.stringify({
                mcpServers: {
                    plain: { url: "http://127.0.0.1:1/mcp" },
                    tls: { url: "https://127.0.0.1:1/mcp" },
                    rejected: { url: "ftp://127.0.0.1/mcp" },
                },
            }),
        );
        const loaded = loadMcpConfig({ agentDir: directory, cwd: directory, projectTrusted: false });
        assert(loaded.servers.some((server) => server.name === "plain"), "plain HTTP must be accepted");
        assert(loaded.servers.some((server) => server.name === "tls"), "HTTPS must be accepted");
        assert(loaded.errors.length === 1 && loaded.errors[0]?.includes("rejected"), "non-HTTP URL must be rejected");
        return {
            name: "application config accepts http and https but rejects another scheme",
            source: "packages/coding-agent/src/extensions/mcp/config.ts:loadMcpConfig,validateMcpServerConfig",
            expected: {
                serverNames: loaded.servers.map((server) => server.name).sort(),
                errors: loaded.errors.map((error) => error.replace(directory, "<agent-dir>")),
            },
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

async function connectHttp(url: string, options: { headers?: Record<string, string>; fetch?: typeof fetch } = {}): Promise<McpClient> {
    const transport = new StreamableHttpTransport({ url, ...options });
    const client = new McpClient({ name: "pi", version: PI_VERSION, roots: [] });
    await client.connect(transport);
    return client;
}

export async function plainHttp(): Promise<Scenario> {
    const fixture = await startMcpHttpFixture();
    let client: McpClient | undefined;
    try {
        client = await connectHttp(fixture.mcpUrl);
        return {
            name: "streamable HTTP transport connects over plain http",
            source: "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport",
            expected: {
                urlScheme: new URL(fixture.mcpUrl).protocol,
                connected: client.connectionState,
                serverName: client.serverInfo?.name,
                requests: fixture.log.map(({ method, path: requestPath, rpc, status }) => ({ method, path: requestPath, rpc, status })),
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function trustedHttps(): Promise<Scenario> {
    const certificate = createSelfSignedCertificate();
    const fixture = await startMcpHttpFixture({ tls: certificate });
    const dispatcher = new undici.Agent({ connect: { ca: certificate.cert } });
    let client: McpClient | undefined;
    try {
        const fetchWithCa = ((input: string | URL | Request, init?: RequestInit) =>
            undici.fetch(input as string, { ...(init as object), dispatcher } as never)) as typeof fetch;
        client = await connectHttp(fixture.mcpUrl, { fetch: fetchWithCa });
        return {
            name: "HTTPS transport connects when its certificate is trusted",
            source: "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport; Node fetch/undici TLS validation",
            expected: {
                connected: client.connectionState,
                serverName: client.serverInfo?.name,
                certificate: "self-signed RSA-2048, SAN IP:127.0.0.1",
                trust: "custom CA passed to undici Agent.connect.ca",
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await dispatcher.close();
        await fixture.close();
    }
}

export async function untrustedHttps(): Promise<Scenario> {
    const certificate = createSelfSignedCertificate();
    const fixture = await startMcpHttpFixture({ tls: certificate });
    let error: unknown;
    try {
        await connectHttp(fixture.mcpUrl);
    } catch (caught) {
        error = caught;
    } finally {
        await fixture.close();
    }
    assert(error !== undefined, "an untrusted certificate must fail the connection");
    return {
        name: "HTTPS transport rejects an untrusted certificate",
        source: "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport; Node fetch/undici TLS validation",
        expected: {
            error: errorRecord(error),
            tlsValidationEnforced: true,
        },
    };
}

export async function sameOriginRedirect(): Promise<Scenario> {
    const fixture = await startMcpHttpFixture({ requireAuth: true });
    let client: McpClient | undefined;
    try {
        client = await connectHttp(`${fixture.origin}/redirect-same`, { headers: { authorization: "Bearer static" } });
        const redirect = fixture.log.find((entry) => entry.path === "/redirect-same");
        const target = fixture.log.find((entry) => entry.path === "/mcp" && entry.rpc === "initialize");
        assert(redirect?.status === 307, "the fixture must answer with a 307");
        assert(target?.authorization === "Bearer static", "fetch must preserve Authorization on a same-origin redirect");
        return {
            name: "same-origin redirect is followed and keeps the request auth header",
            source: "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport; standard fetch redirect handling",
            expected: {
                redirectStatus: redirect.status,
                redirectMethod: redirect.method,
                targetPath: target.path,
                targetMethod: target.method,
                authorizationForwarded: target.authorization === "Bearer static",
                connected: client.connectionState,
            },
        };
    } finally {
        await client?.close().catch(() => {});
        await fixture.close();
    }
}

export async function crossOriginRedirect(): Promise<Scenario> {
    const target = await startMcpHttpFixture({ requireAuth: true });
    const source = await startMcpHttpFixture({ redirectTo: target.mcpUrl });
    let client: McpClient | undefined;
    let error: unknown;
    try {
        client = await connectHttp(`${source.origin}/redirect-cross`, { headers: { authorization: "Bearer static" } });
    } catch (caught) {
        error = caught;
    } finally {
        const targetRequest = target.log.find((entry) => entry.path === "/mcp");
        const sourceRequest = source.log.find((entry) => entry.path === "/redirect-cross");
        await client?.close().catch(() => {});
        await source.close();
        await target.close();
        assert(error instanceof McpAuthRequiredError, "the cross-origin target must reject the stripped auth header");
        assert(sourceRequest?.authorization === "Bearer static", "the first request carries the auth header");
        assert(targetRequest?.authorization === undefined, "fetch must strip Authorization across origins");
        return {
            name: "cross-origin redirect is followed without Authorization and is rejected",
            source: "packages/mcp/src/transports/streamable-http.ts:StreamableHttpTransport; standard fetch redirect handling",
            expected: {
                sourceRedirectStatus: sourceRequest.status,
                sourceAuthorization: sourceRequest.authorization,
                targetAuthorization: targetRequest.authorization ?? null,
                targetStatus: targetRequest.status,
                error: errorRecord(error),
            },
        };
    }
}

// ---------------------------------------------------------------------------------------------------
// Application initialization: connect lists tools only, resources stay lazy, management closes fast.
// ---------------------------------------------------------------------------------------------------

function entryFor(name: string, exposure: "direct" | "codemode"): McpServerEntry {
    return {
        name,
        config: { url: "http://127.0.0.1:1/mcp", exposure },
        source: "/tmp/audit-1052/mcp.json",
    };
}

function connectionFor(
    server: Awaited<ReturnType<typeof startInMemoryMcpServer>>,
    entry = entryFor("slow", "codemode"),
): McpServerConnection {
    return new McpServerConnection({
        entry,
        cwd: "/tmp/audit-1052/workspace",
        createTransport: () => server.transport,
        credentials: new McpOAuthCredentialStore(new InMemoryAuthStorageBackend()),
        onTools: () => {},
    });
}

export async function connectListsToolsNotResources(): Promise<Scenario> {
    const server = await startInMemoryMcpServer({ resources: true });
    const connection = connectionFor(server);
    try {
        await connection.getClient();
        return {
            name: "connecting a server lists tools but not resources",
            source: "packages/coding-agent/src/extensions/mcp/runtime.ts:McpServerConnection.connectOnce",
            expected: {
                state: connection.state,
                tools: connection.tools.map((tool) => tool.name),
                hasResources: connection.hasResources,
                methodsAtConnect: server.methods,
                resourcesListedAtConnect: server.methods.includes("resources/list"),
            },
        };
    } finally {
        await connection.close();
    }
}

export async function resourcesLazy(): Promise<Scenario> {
    const server = await startInMemoryMcpServer({ resources: true });
    const connection = connectionFor(server);
    try {
        await connection.getClient();
        const before = [...server.methods];
        const resources = await connection.allResources();
        return {
            name: "resources are listed on demand after the connection is ready",
            source: "packages/coding-agent/src/extensions/mcp/runtime.ts:McpServerConnection.allResources",
            expected: {
                before,
                after: server.methods,
                resources: resources.map((resource) => resource.uri),
                toolsStillAvailable: connection.tools.map((tool) => tool.name),
                state: connection.state,
            },
        };
    } finally {
        await connection.close();
    }
}

export async function resourcesFailure(): Promise<Scenario> {
    const server = await startInMemoryMcpServer({ resources: true, resourceListError: true });
    const connection = connectionFor(server);
    try {
        await connection.getClient();
        let error: unknown;
        try {
            await connection.allResources();
        } catch (caught) {
            error = caught;
        }
        assert(error !== undefined, "resources/list failure must reject the resource call");
        assert(connection.state === "connected", "a resource-list failure must not disconnect the server");
        return {
            name: "a resources/list failure leaves the server connected and usable for tools",
            source: "packages/coding-agent/src/extensions/mcp/runtime.ts:McpServerConnection.allResources",
            expected: {
                state: connection.state,
                error: errorRecord(error),
                tools: connection.tools.map((tool) => tool.name),
                methods: server.methods,
            },
        };
    } finally {
        await connection.close();
    }
}

export async function closeWhileConnecting(): Promise<Scenario> {
    const server = await startInMemoryMcpServer({ neverInitialize: true });
    const connection = connectionFor(server, entryFor("slow", "direct"));
    const opening = connection.getClient();
    const start = Date.now();
    await connection.close();
    const closeMs = Date.now() - start;
    let error: unknown;
    try {
        await opening;
    } catch (caught) {
        error = caught;
    }
    assert(closeMs < 1_000, "close must not wait for a pending initialize");
    return {
        name: "closing while initialize is pending returns without waiting",
        source: "packages/coding-agent/src/extensions/mcp/runtime.ts:McpServerConnection.close",
        expected: {
            closeMs: closeMs <= 250 ? "<=250ms" : ">250ms",
            state: connection.state,
            tools: connection.tools.map((tool) => tool.name),
            pendingInitializeError: errorRecord(error),
        },
    };
}

interface ApiStub {
    tools: Array<Record<string, unknown>>;
    active: string[];
    notifications: Array<{ message: string; level?: string }>;
    handlers: Map<string, Function>;
    api: Record<string, unknown>;
}

function apiStub(): ApiStub {
    const result: ApiStub = { tools: [], active: [], notifications: [], handlers: new Map(), api: {} };
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
        registerCommand: () => {},
        registerMessageRenderer: () => {},
        events: { emit: () => {} },
    };
    return result;
}

function extensionContext(stub: ApiStub): Record<string, unknown> {
    return {
        cwd: "/tmp/audit-1052/workspace",
        hasUI: true,
        mode: "print",
        signal: new AbortController().signal,
        ui: {
            notify: (message: string, level?: string) => stub.notifications.push({ message, level }),
            input: async () => undefined,
        },
    };
}

async function runApplicationPrompt(
    options: {
        exposure: "direct" | "codemode";
        server: Awaited<ReturnType<typeof startInMemoryMcpServer>>;
        startupWaitMs?: number;
        installCodemode?: boolean;
    },
): Promise<{ elapsedMs: number; stub: ApiStub; prompt: { systemPromptOptions: { sections: Record<string, string> } } }> {
    const stub = apiStub();
    if (options.installCodemode) {
        stub.tools.push({
            name: "codemode",
            description: "test codemode",
            parameters: { type: "object", properties: {} },
            reachesTools: () => true,
            execute: async () => ({ content: [] }),
        });
        stub.active.push("codemode");
    }
    const extension = createMcpExtension({
        loadConfig: () => ({
            servers: [entryFor("slow", options.exposure)],
            errors: [],
        }),
        createTransport: () => {
            return options.server.transport;
        },
        logPath: "/tmp/audit-1052/mcp.log",
        ...(options.startupWaitMs === undefined ? {} : { startupWaitMs: options.startupWaitMs }),
    });
    extension(stub.api as never);
    const context = extensionContext(stub);
    await stub.handlers.get("session_start")!({ type: "session_start" }, context);
    const prompt = { systemPromptOptions: { sections: {} as Record<string, string> } };
    const start = Date.now();
    await stub.handlers.get("before_agent_start")!({ type: "before_agent_start", ...prompt }, context);
    const elapsedMs = Date.now() - start;
    await stub.handlers.get("session_shutdown")!({ type: "session_shutdown", reason: "quit" }, context);
    return { elapsedMs, stub, prompt };
}

export async function applicationCodemodePrompt(): Promise<Scenario> {
    const server = await startInMemoryMcpServer({ neverInitialize: true });
    try {
        const result = await runApplicationPrompt({
            exposure: "codemode",
            server,
            installCodemode: true,
        });
        const section = result.prompt.systemPromptOptions.sections.mcp_servers ?? "";
        assert(result.elapsedMs < 1_000, "a codemode server must not hold up the first prompt");
        assert(section.includes("mcp__slow"), "the server section must name the connecting server");
        return {
            name: "first prompt does not wait for a codemode server still initializing",
            source: "packages/coding-agent/src/extensions/mcp/index.ts:session_start,before_agent_start,waitForDirectServers",
            expected: {
                elapsedMs: result.elapsedMs <= 250 ? "<=250ms" : ">250ms",
                startupWaitMs: 10_000,
                notification: result.stub.notifications,
                section,
                activeTools: result.stub.active,
            },
        };
    } finally {
        await server.close();
    }
}

export async function applicationDirectStartupTimeout(): Promise<Scenario> {
    const server = await startInMemoryMcpServer({ neverInitialize: true });
    try {
        const result = await runApplicationPrompt({
            exposure: "direct",
            server,
            startupWaitMs: 40,
        });
        assert(result.elapsedMs >= 40, "the first prompt must wait startupWaitMs for a direct server");
        assert(
            result.stub.notifications.some((notification) => notification.message.includes("still connecting")),
            "the timeout must be reported at the application boundary",
        );
        return {
            name: "first prompt waits startupWaitMs for a direct server and reports still connecting",
            source: "packages/coding-agent/src/extensions/mcp/index.ts:DEFAULT_STARTUP_WAIT_MS,waitForDirectServers",
            expected: {
                elapsedMs: result.elapsedMs >= 40 && result.elapsedMs < 1_000 ? "40-999ms" : String(result.elapsedMs),
                startupWaitMs: 40,
                notification: result.stub.notifications,
                section: result.prompt.systemPromptOptions.sections.mcp_servers ?? null,
                tools: result.stub.tools.map((tool) => tool.name),
            },
        };
    } finally {
        await server.close();
    }
}

export async function applicationDirectToolsAppear(): Promise<Scenario> {
    const server = await startInMemoryMcpServer({ initializeDelayMs: 30 });
    try {
        const result = await runApplicationPrompt({
            exposure: "direct",
            server,
            startupWaitMs: 500,
        });
        assert(result.elapsedMs < 500, "a direct server that connects within the wait must not use the timeout");
        const names = result.stub.tools.map((tool) => String(tool.name));
        assert(names.includes("mcp__slow__echo"), "the direct tool must be registered after connecting");
        return {
            name: "direct server tools become available when it connects within startupWaitMs",
            source: "packages/coding-agent/src/extensions/mcp/index.ts:startConnection,waitForDirectServers",
            expected: {
                elapsedMs: result.elapsedMs <= 250 ? "<=250ms" : ">250ms",
                registeredTools: names,
                activeTools: result.stub.active,
                notifications: result.stub.notifications,
                methods: server.methods,
            },
        };
    } finally {
        await server.close();
    }
}
