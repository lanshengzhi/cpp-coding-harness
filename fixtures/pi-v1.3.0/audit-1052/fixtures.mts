/**
 * Local fixture servers for the `audit-1052` MCP capture.
 *
 * Every server binds an ephemeral loopback port and needs no credential, provider catalog or live
 * state. The OAuth fixture implements the frozen pi v1.3.0 OAuth discovery/registration/token
 * surface alongside one MCP endpoint, so a scenario can stall one request and observe what the
 * real client does next. The MCP fixtures record tokens, JSON-RPC methods and HTTP statuses rather
 * than asserting on internals.
 */

import { createHash } from "node:crypto";
import { createServer as createHttpServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { createServer as createHttpsServer } from "node:https";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { LATEST_PROTOCOL_VERSION, createInMemoryTransportPair, isJsonRpcRequest } from "./pi.mts";

export interface StalledRequest {
    path: string;
    request: IncomingMessage;
    release(): void;
}

async function readBody(request: IncomingMessage): Promise<string> {
    const chunks: Buffer[] = [];
    for await (const chunk of request) chunks.push(Buffer.from(chunk));
    return Buffer.concat(chunks).toString("utf8");
}

function sendJson(response: ServerResponse, status: number, body: unknown, headers: Record<string, string> = {}): void {
    response.writeHead(status, { "content-type": "application/json", ...headers }).end(JSON.stringify(body));
}

/** The OAuth-protected MCP server and authorization server used by the OAuth lifecycle scenarios. */
export async function startOAuthMcpFixture(options: { iss?: string; issParameter?: boolean; cimd?: boolean } = {}) {
    const log: string[] = [];
    const mcpRequests: Array<{ method: string; token: string | undefined; status: number; id?: number }> = [];
    const tokenRequests: URLSearchParams[] = [];
    const tokenResponses: Array<{ status: number; body: unknown }> = [];
    const registrations: Record<string, unknown>[] = [];
    const authorizations: URLSearchParams[] = [];
    const stall = new Set<string>();
    const stalled: StalledRequest[] = [];
    const validTokens = new Set<string>();
    const refreshTokens = new Set<string>();
    const challenges = new Map<string, string>();
    let issued = 0;
    let origin = "";

    const waitIfStalled = async (request: IncomingMessage, pathname: string): Promise<void> => {
        if (!stall.has(pathname)) return;
        await new Promise<void>((resolve) => stalled.push({ path: pathname, request, release: resolve }));
    };

    const issueTokens = () => {
        issued += 1;
        const tokens = { access_token: `access-${issued}`, refresh_token: `refresh-${issued}` };
        validTokens.add(tokens.access_token);
        refreshTokens.add(tokens.refresh_token);
        return { ...tokens, token_type: "Bearer", expires_in: 3600 };
    };

    const handleMcp = async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
        const token = request.headers.authorization?.replace(/^Bearer /, "");
        if (request.method === "DELETE") {
            log.push(`delete ${token}`);
            response.writeHead(200).end();
            return;
        }
        if (request.method === "GET") {
            response.writeHead(405).end();
            return;
        }
        const message = JSON.parse(await readBody(request)) as { id?: number; method: string; params?: unknown };
        const status = token && validTokens.has(token) ? 200 : 401;
        mcpRequests.push({ method: message.method, token, status, ...(message.id === undefined ? {} : { id: message.id }) });
        if (status === 401) {
            log.push(`401 ${token ?? "none"}`);
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
        let result: unknown = {};
        if (message.method === "initialize") {
            result = {
                protocolVersion: LATEST_PROTOCOL_VERSION,
                capabilities: { tools: {} },
                serverInfo: { name: "fixture", version: "1.0.0" },
            };
        } else if (message.method === "tools/list") {
            result = { tools: [{ name: "echo", inputSchema: { type: "object", properties: {} } }] };
        } else if (message.method === "tools/call") {
            result = { content: [{ type: "text", text: "ok" }] };
        }
        sendJson(response, 200, { jsonrpc: "2.0", id: message.id, result }, { "mcp-session-id": "session-1" });
    };

    const handle = async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
        const url = new URL(request.url ?? "/", origin);
        switch (url.pathname) {
            case "/mcp":
                return handleMcp(request, response);
            case "/.well-known/oauth-protected-resource/mcp":
                await waitIfStalled(request, url.pathname);
                return sendJson(response, 200, {
                    resource: `${origin}/mcp`,
                    authorization_servers: [origin],
                });
            case "/.well-known/oauth-authorization-server":
                await waitIfStalled(request, url.pathname);
                return sendJson(response, 200, {
                    issuer: origin,
                    authorization_endpoint: `${origin}/authorize`,
                    token_endpoint: `${origin}/token`,
                    registration_endpoint: `${origin}/register`,
                    response_types_supported: ["code"],
                    code_challenge_methods_supported: ["S256"],
                    token_endpoint_auth_methods_supported: ["none"],
                    ...(options.cimd ? { client_id_metadata_document_supported: true } : {}),
                    ...(options.issParameter ? { authorization_response_iss_parameter_supported: true } : {}),
                });
            case "/register": {
                const metadata = JSON.parse(await readBody(request)) as Record<string, unknown>;
                await waitIfStalled(request, url.pathname);
                log.push("register");
                registrations.push(metadata);
                return sendJson(response, 201, { ...metadata, client_id: "client-1" });
            }
            case "/authorize": {
                authorizations.push(url.searchParams);
                const code = `code-${challenges.size + 1}`;
                challenges.set(code, url.searchParams.get("code_challenge") ?? "");
                const redirect = new URL(url.searchParams.get("redirect_uri") ?? "");
                redirect.searchParams.set("code", code);
                redirect.searchParams.set("state", url.searchParams.get("state") ?? "");
                const iss = options.iss ?? (options.issParameter ? origin : undefined);
                if (iss) redirect.searchParams.set("iss", iss);
                response.writeHead(302, { location: redirect.href }).end();
                return;
            }
            case "/token": {
                const params = new URLSearchParams(await readBody(request));
                tokenRequests.push(params);
                await waitIfStalled(request, url.pathname);
                if (params.get("grant_type") === "authorization_code") {
                    const challenge = challenges.get(params.get("code") ?? "");
                    const verifier = createHash("sha256")
                        .update(params.get("code_verifier") ?? "")
                        .digest("base64url");
                    if (!challenge || challenge !== verifier) {
                        tokenResponses.push({ status: 400, body: { error: "invalid_grant" } });
                        return sendJson(response, 400, { error: "invalid_grant" });
                    }
                    challenges.delete(params.get("code") ?? "");
                    log.push("token code");
                    const body = issueTokens();
                    tokenResponses.push({ status: 200, body });
                    return sendJson(response, 200, body);
                }
                const refresh = params.get("refresh_token") ?? "";
                if (!refreshTokens.delete(refresh)) {
                    tokenResponses.push({ status: 400, body: { error: "invalid_grant" } });
                    return sendJson(response, 400, { error: "invalid_grant" });
                }
                log.push("token refresh");
                const body = issueTokens();
                tokenResponses.push({ status: 200, body });
                return sendJson(response, 200, body);
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
        log,
        mcpRequests,
        tokenRequests,
        tokenResponses,
        registrations,
        authorizations,
        stall,
        stalled,
        seedAccessToken(token: string): void {
            validTokens.add(token);
        },
        seedRefreshToken(token: string): void {
            refreshTokens.add(token);
        },
        expireAccessTokens(): void {
            validTokens.clear();
        },
        /** Resolve every stalled HTTP handler after its body was read. */
        releaseAll(): void {
            for (const entry of stalled.splice(0)) entry.release();
        },
        async waitForStall(pathname: string): Promise<StalledRequest> {
            for (;;) {
                const match = stalled.find((entry) => entry.path === pathname);
                if (match) return match;
                await new Promise((resolve) => setTimeout(resolve, 5));
            }
        },
        close(): Promise<void> {
            return new Promise((resolve) => {
                server.closeAllConnections();
                server.close(() => resolve());
            });
        },
    };
}

export interface McpHttpFixtureOptions {
    requireAuth?: boolean;
    redirectTo?: string;
    tls?: { key: Buffer; cert: Buffer };
}

/** One MCP endpoint over HTTP or HTTPS, with optional auth and redirect entry points. */
export async function startMcpHttpFixture(options: McpHttpFixtureOptions = {}) {
    const log: Array<{ method: string; path: string; authorization: string | undefined; rpc: string | undefined; status: number }> = [];
    const handle = async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
        const url = new URL(request.url ?? "/", "http://fixture");
        if (request.method === "DELETE") {
            log.push({ method: "DELETE", path: url.pathname, authorization: request.headers.authorization, rpc: undefined, status: 200 });
            response.writeHead(200).end();
            return;
        }
        if (request.method === "GET") {
            response.writeHead(405).end();
            return;
        }
        const body = await readBody(request);
        const message = body ? (JSON.parse(body) as { id?: number; method?: string }) : {};
        if (url.pathname === "/redirect-same") {
            log.push({ method: "POST", path: url.pathname, authorization: request.headers.authorization, rpc: message.method, status: 307 });
            response.writeHead(307, { location: "/mcp" }).end();
            return;
        }
        if (url.pathname === "/redirect-cross" && options.redirectTo) {
            log.push({ method: "POST", path: url.pathname, authorization: request.headers.authorization, rpc: message.method, status: 307 });
            response.writeHead(307, { location: options.redirectTo }).end();
            return;
        }
        if (options.requireAuth && request.headers.authorization !== "Bearer static") {
            log.push({ method: "POST", path: url.pathname, authorization: request.headers.authorization, rpc: message.method, status: 401 });
            response.writeHead(401, { "www-authenticate": "Bearer" }).end();
            return;
        }
        if (message.id === undefined) {
            log.push({ method: "POST", path: url.pathname, authorization: request.headers.authorization, rpc: message.method, status: 202 });
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
                : {};
        log.push({ method: "POST", path: url.pathname, authorization: request.headers.authorization, rpc: message.method, status: 200 });
        sendJson(response, 200, { jsonrpc: "2.0", id: message.id, result }, { "mcp-session-id": "session-1" });
    };
    const server = options.tls
        ? createHttpsServer({ key: options.tls.key, cert: options.tls.cert }, (request, response) => {
              void handle(request, response);
          })
        : createHttpServer((request, response) => {
              void handle(request, response);
          });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (!address || typeof address === "string") throw new Error("MCP HTTP fixture did not bind to TCP");
    const scheme = options.tls ? "https" : "http";
    return {
        origin: `${scheme}://127.0.0.1:${address.port}`,
        mcpUrl: `${scheme}://127.0.0.1:${address.port}/mcp`,
        log,
        close(): Promise<void> {
            return new Promise((resolve) => {
                server.closeAllConnections();
                server.close(() => resolve());
            });
        },
    };
}

/** A self-signed loopback certificate for the HTTPS transport scenarios. */
export function createSelfSignedCertificate(): { key: Buffer; cert: Buffer } {
    const directory = mkdtempSync(path.join(tmpdir(), "audit-1052-tls-"));
    const keyPath = path.join(directory, "key.pem");
    const certPath = path.join(directory, "cert.pem");
    try {
        execFileSync("openssl", [
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            keyPath,
            "-out",
            certPath,
            "-days",
            "2",
            "-subj",
            "/CN=127.0.0.1",
            "-addext",
            "subjectAltName=IP:127.0.0.1",
        ], { stdio: "ignore" });
        return { key: readFileSync(keyPath), cert: readFileSync(certPath) };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

export interface InMemoryMcpServerOptions {
    resources?: boolean;
    resourceListError?: boolean;
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
            if (message.method === "resources/list" && options.resourceListError) {
                void pair.server.send({
                    jsonrpc: "2.0",
                    id: message.id,
                    error: { code: -32603, message: "resource boom" },
                });
                return;
            }
            let result: unknown = {};
            if (message.method === "initialize") {
                result = {
                    protocolVersion: LATEST_PROTOCOL_VERSION,
                    capabilities: { tools: {}, ...(options.resources ? { resources: {} } : {}) },
                    serverInfo: { name: "scripted", version: "1.0.0" },
                };
            } else if (message.method === "tools/list") {
                result = { tools: [{ name: "echo", inputSchema: { type: "object", properties: {} } }] };
            } else if (message.method === "resources/list") {
                result = { resources: [{ uri: "docs://intro", name: "intro", mimeType: "text/markdown" }] };
            } else if (message.method === "resources/templates/list") {
                result = { resourceTemplates: [] };
            } else if (message.method === "resources/read") {
                result = { contents: [{ uri: "docs://intro", mimeType: "text/markdown", text: "# intro" }] };
            }
            void pair.server.send({ jsonrpc: "2.0", id: message.id, result });
        };
        const delay = options.initializeDelayMs ?? 0;
        if (message.method === "initialize" && delay > 0) setTimeout(respond, delay);
        else queueMicrotask(respond);
    });
    return { methods, transport: pair.client, close: () => pair.client.close() };
}
