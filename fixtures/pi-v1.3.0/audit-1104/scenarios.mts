/**
 * Scenarios for the `audit-1104` management-during-connection capture. Each function drives frozen
 * pi v1.3.0 through a public seam (the `createMcpExtension` boundary with a scripted host, exactly
 * like audit-1052's application scenarios, extended with the real headless `McpManagerView` for
 * the interactive `/mcp` panel) and returns the observed values.
 *
 * These observations close the audit-1052 open limitation for the management slice: the extension
 * stub records the same registration/notification/UI surface a real Session provides, and the
 * panel loop renders through pi's real manager view; the session-level wiring on the Pike side is
 * compared through the real AgentSession/Interactive seams in the C++ suite.
 */

import { mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { createDefaultTransport, createMcpExtension } from "./pi.mts";
import type { McpServerEntry } from "./pi.mts";
import {
    apiStub,
    credentialStore,
    extensionContext,
    prepareHeadlessTui,
    startInMemoryMcpServer,
    startOAuthMcpFixture,
    type ApiStub,
    type HeadlessManagerHost,
} from "./fixtures.mts";

export interface Scenario {
    name: string;
    source: string;
    expected: unknown;
}

export function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

const bound = (elapsedMs: number, limit: number) => (elapsedMs <= limit ? `<=${limit}ms` : `>${limit}ms`);

function entryFor(name: string, exposure: "direct" | "codemode", url = "http://127.0.0.1:1/mcp"): McpServerEntry {
    return { name, config: { url, exposure }, source: "/tmp/audit-1104/mcp.json" };
}

interface RunningExtension {
    stub: ApiStub;
    start(mode: "print" | "tui", manager?: HeadlessManagerHost): Promise<Record<string, unknown>>;
    emit(event: "turn_start" | "session_shutdown", context: Record<string, unknown>): Promise<unknown>;
    command(name: string): { description: string; handler(args: string, ctx: unknown): Promise<void> };
}

/** One extension over the scripted servers, wired like audit-1052's application scenarios. */
function startExtension(options: {
    servers: McpServerEntry[];
    scriptedTransport: { name: string; transport: unknown } | undefined;
    credentialsPath: string;
    openUrl?: (url: string) => void;
}): RunningExtension {
    const stub = apiStub();
    const extension = createMcpExtension({
        loadConfig: () => ({ servers: options.servers, errors: [] }),
        createTransport: (entry: McpServerEntry, cwd: string, authProvider: unknown) =>
            options.scriptedTransport && entry.name === options.scriptedTransport.name
                ? options.scriptedTransport.transport
                : createDefaultTransport(entry, cwd, authProvider as never),
        credentials: credentialStore(options.credentialsPath),
        logPath: "/tmp/audit-1104/mcp.log",
        ...(options.openUrl ? { openUrl: options.openUrl } : {}),
    });
    extension(stub.api as never);
    return {
        stub,
        start: async (mode, manager) => {
            const context = extensionContext(stub, mode, manager ? { manager } : {});
            await stub.handlers.get("session_start")!({ type: "session_start" }, context);
            return context;
        },
        emit: async (event, context) => stub.handlers.get(event)!({ type: event }, context),
        command: (name) => {
            const definition = stub.commands.get(name);
            assert(definition, `the extension did not register ${name}`);
            return definition;
        },
    };
}

/** The menu item lines of a rendered frame: selection arrow or item indentation, without borders. */
function itemLines(frame: string[]): string[] {
    return frame
        .map((line) => line.trimEnd())
        .filter((line) => /^(→\s+| {2})\S/.test(line))
        .map((line) => line.replace(/^→\s+/, "").replace(/^ {2}/, "").trim());
}

/** The detail lines of a rendered server menu (its `State:` line). */
function detailLines(frame: string[]): string[] {
    return frame.map((line) => line.trim()).filter((line) => line.startsWith("State: "));
}

// ── Scenario 1: the interactive manager during connection ───────────────────

export async function managerUsableDuringConnection(): Promise<Scenario> {
    prepareHeadlessTui();
    const directory = mkdtempSync(path.join(tmpdir(), "audit-1104-mgr-"));
    const slow = await startInMemoryMcpServer({ neverInitialize: true });
    const auth = await startOAuthMcpFixture();
    const { HeadlessManagerHost } = await import("./fixtures.mts");
    const host = new HeadlessManagerHost();
    let authorizationUrl = "";
    const running = startExtension({
        servers: [entryFor("slow", "direct"), entryFor("auth", "direct", auth.url)],
        scriptedTransport: { name: "slow", transport: slow.transport },
        credentialsPath: path.join(directory, "mcp-auth.json"),
        openUrl: (url: string) => {
            authorizationUrl = url;
        },
    });
    try {
        await running.start("tui", host);

        // Open the `/mcp` panel while `slow` is still mid-handshake and `auth` has settled
        // into its OAuth challenge.
        const context = extensionContext(running.stub, "tui", { manager: host });
        const panel = running.command("mcp").handler("", context);
        const serversFrame = await host.waitFor(
            (frame) => frame.join("\n").includes("connecting…") && frame.join("\n").includes("needs sign-in"),
            "the servers menu showing a connecting and a needs-auth server",
        );
        const whileConnecting = itemLines(serversFrame);
        assert(
            whileConnecting.some((line) => line.includes("auth") && line.includes("needs sign-in")),
            "the servers menu must show the challenged server as needs sign-in",
        );
        assert(
            whileConnecting.some((line) => line.includes("slow") && line.includes("connecting…")),
            "the servers menu must show the unanswered server as connecting",
        );

        // The challenged server is first (attention order). Its menu offers the sign-in action.
        // Wait for the OAuth URL to resolve before entering: between `markNeedsAuth` and the
        // resolved URL the server menu has no sign-in item and the retained selection falls on
        // Exposure, which is upstream panel behavior, not part of this capture.
        await host.waitForStable();
        host.press("\r");
        const authMenu = await host.waitFor(
            (frame) => frame.join("\n").includes("Sign in"),
            "the auth server menu",
        );
        await host.waitForStable();
        const authMenuItems = itemLines(authMenu);

        // Sign in through the real manager view while `slow` is still connecting.
        const start = Date.now();
        host.press("\r");
        await host.waitFor((frame) => frame.join("\n").includes("paste the URL"), "the redirect-URL screen");
        assert(authorizationUrl !== "", "the flow must show the authorization URL");
        // The fixture plays the browser and returns the URL the user would paste back.
        const pasteUrl = auth.approve(authorizationUrl);
        host.type(pasteUrl);
        host.press("\r");
        const signedInFrame = await host.waitFor(
            (frame) => detailLines(frame).some((line) => line.includes("connected")),
            "the auth server menu showing connected",
        );
        const signInMs = Date.now() - start;
        const signedInState = detailLines(signedInFrame);

        // Back on the servers menu, `auth` is connected and `slow` is still connecting.
        host.press("\x1b");
        const serversAfter = await host.waitFor(
            (frame) => itemLines(frame).some((line) => line.includes("auth") && line.includes("connected")),
            "the servers menu showing the signed-in server",
        );
        const afterSignIn = itemLines(serversAfter);

        // The connecting server sorts first once `auth` is connected; its menu offers exposure
        // and disable while it connects (no sign-in, no reconnect).
        host.press("\r");
        const slowMenu = await host.waitFor(
            (frame) => frame.join("\n").includes("State: connecting"),
            "the slow server menu",
        );
        const slowMenuItems = itemLines(slowMenu);
        host.press("\x1b");
        // Give the manage loop a tick to present the servers menu before closing: two synchronous
        // escapes would land on the settled server menu and the second one would be lost.
        await host.waitFor((frame) => frame.join("\n").includes("MCP servers"), "the servers menu");
        host.press("\x1b");
        await panel;

        assert(
            slow.methods.length === 1 && slow.methods[0] === "initialize",
            "the unanswered server must still be waiting on its first initialize",
        );
        return {
            name: "the /mcp manager lists connecting servers and completes a sign-in while another server connects",
            source: "packages/coding-agent/src/extensions/mcp/index.ts:session_start,manage,serversMenu,serverMenu,runAction,signInWithUi",
            expected: {
                serversMenuWhileConnecting: whileConnecting,
                authServerMenuItems: authMenuItems,
                authMenuStateAfterSignIn: signedInState,
                signInMs: bound(signInMs, 5_000),
                serversMenuAfterSignIn: afterSignIn,
                slowServerMenuItems: slowMenuItems,
                slowMethods: slow.methods,
            },
        };
    } finally {
        await running.emit("session_shutdown", extensionContext(running.stub, "tui", { manager: host }));
        rmSync(directory, { recursive: true, force: true });
        slow.close();
        await auth.close();
    }
}

// ── Scenario 2: print-mode status settles connections before reporting ──────

export async function printStatusSettlesConnections(): Promise<Scenario> {
    const delayMs = 400;
    const delayed = await startInMemoryMcpServer({ initializeDelayMs: delayMs });
    const directory = mkdtempSync(path.join(tmpdir(), "audit-1104-status-"));
    const running = startExtension({
        servers: [entryFor("slow", "codemode")],
        scriptedTransport: { name: "slow", transport: delayed.transport },
        credentialsPath: path.join(directory, "mcp-auth.json"),
    });
    try {
        await running.start("print");

        // The prompt section names the connecting server before anything settled.
        const prompt = { systemPromptOptions: { sections: {} as Record<string, string> } };
        await running.stub.handlers.get("before_agent_start")!({ type: "before_agent_start", ...prompt },
            extensionContext(running.stub, "print"));
        const section = prompt.systemPromptOptions.sections.mcp_servers ?? "";

        // The plain `/mcp` status waits for the connections it reports on.
        const start = Date.now();
        await running.command("mcp").handler("", extensionContext(running.stub, "print"));
        const elapsedMs = Date.now() - start;
        assert(elapsedMs >= delayMs - 100, "print status must wait for the delayed server to settle");
        const status = running.stub.notifications.at(-1);
        assert(status?.message.includes("slow: connected"), "the settled status must report the connected server");

        await running.emit("session_shutdown", extensionContext(running.stub, "print"));
        return {
            name: "plain /mcp status waits for pending connections and then reports settled states",
            source: "packages/coding-agent/src/extensions/mcp/index.ts:registerCommand(mcp),formatStatus,pending",
            expected: {
                sectionWhileConnecting: section,
                statusWaitedForSettle: elapsedMs >= delayMs - 100,
                statusWaitedAtLeast: elapsedMs >= delayMs - 100 ? `>=${delayMs - 100}ms` : `<${delayMs - 100}ms`,
                notification: status ?? null,
            },
        };
    } finally {
        delayed.close();
        rmSync(directory, { recursive: true, force: true });
    }
}

// ── Scenario 3: an external sign-in is picked up on the next turn ───────────

export async function externalSignInPickup(): Promise<Scenario> {
    const auth = await startOAuthMcpFixture();
    const directory = mkdtempSync(path.join(tmpdir(), "audit-1104-pickup-"));
    const running = startExtension({
        servers: [entryFor("auth", "direct", auth.url)],
        scriptedTransport: undefined,
        credentialsPath: path.join(directory, "mcp-auth.json"),
    });
    try {
        await running.start("print");

        // No credentials anywhere: the server settles into needs-auth.
        await running.command("mcp").handler("", extensionContext(running.stub, "print"));
        const before = running.stub.notifications.at(-1);
        assert(before?.message.includes("needs sign-in"), "the server must start out needs-auth");

        // `pi mcp login` in another process stores credentials; nothing reconnects by itself.
        const external = credentialStore(path.join(directory, "mcp-auth.json"));
        auth.seedAccessToken("access-external");
        await external
            .forServer("auth", auth.url)
            .save({
                serverUrl: auth.url,
                tokens: { access_token: "access-external", refresh_token: "refresh-external", token_type: "Bearer" },
            });
        await running.command("mcp").handler("", extensionContext(running.stub, "print"));
        const stillNeedsAuth = running.stub.notifications.at(-1);
        assert(stillNeedsAuth?.message.includes("needs sign-in"), "a status read must not reconnect by itself");

        // The next turn picks the sign-in up: reconnect succeeds with the stored token.
        await running.emit("turn_start", extensionContext(running.stub, "print"));
        await running.command("mcp").handler("", extensionContext(running.stub, "print"));
        const after = running.stub.notifications.at(-1);
        assert(after?.message.includes("auth: connected"), "the turn start must reconnect the externally signed-in server");

        await running.emit("session_shutdown", extensionContext(running.stub, "print"));
        return {
            name: "a sign-in performed outside the session is picked up on the next turn",
            source: "packages/coding-agent/src/extensions/mcp/index.ts:onConnectionChange,tokensAtSignIn,reconnectSignedIn,turn_start",
            expected: {
                stateBeforeExternalSignIn: before?.message ?? "",
                stateAfterExternalSignInBeforeTurn: stillNeedsAuth?.message ?? "",
                stateAfterTurnStart: after?.message ?? "",
                authorizedRequests: auth.mcpRequests.filter((request) => request.status === 200).length >= 1,
            },
        };
    } finally {
        await auth.close();
        rmSync(directory, { recursive: true, force: true });
    }
}

// ── Scenario 4: session shutdown while initialize is pending ────────────────

export async function shutdownWhileConnecting(): Promise<Scenario> {
    const slow = await startInMemoryMcpServer({ neverInitialize: true });
    const directory = mkdtempSync(path.join(tmpdir(), "audit-1104-close-"));
    mkdirSync("/tmp/audit-1104/workspace", { recursive: true });
    const running = startExtension({
        servers: [entryFor("slow", "direct")],
        scriptedTransport: { name: "slow", transport: slow.transport },
        credentialsPath: path.join(directory, "mcp-auth.json"),
    });
    try {
        await running.start("print");
        await new Promise((resolve) => setTimeout(resolve, 100));
        assert(slow.methods.includes("initialize"), "the handshake must be in flight");

        const start = Date.now();
        await running.emit("session_shutdown", extensionContext(running.stub, "print"));
        const shutdownMs = Date.now() - start;
        assert(shutdownMs < 1_000, "session shutdown must not wait for the pending initialize");

        return {
            name: "closing the session while initialize is pending returns without waiting",
            source: "packages/coding-agent/src/extensions/mcp/index.ts:session_shutdown,McpServerConnection.close",
            expected: {
                shutdownMs: bound(shutdownMs, 250),
                slowMethods: slow.methods,
            },
        };
    } finally {
        slow.close();
        rmSync(directory, { recursive: true, force: true });
    }
}
