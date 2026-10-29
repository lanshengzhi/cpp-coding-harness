/**
 * Imports the frozen pi v1.3.0 modules from the checkout named by `PI_CHECKOUT`.
 *
 * The capture lives in the Pike repository, but the measured source is a separate private checkout.
 * Resolving through `PI_CHECKOUT` keeps the capture independent of any package files in the Pike tree
 * and makes the manifest's revision guard authoritative. The management-UI scenarios additionally
 * import the interactive theme and keybindings so the real `McpManagerView` runs against a headless
 * terminal double, exactly like pi's own `mcp-manager-view.test.ts`.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT ?? "/tmp/cch-1104-pi-frozen";

function load(relativePath: string): Promise<Record<string, unknown>> {
    return import(pathToFileURL(path.join(checkout, relativePath)).href) as Promise<Record<string, unknown>>;
}

const mcp = await load("packages/mcp/src/index.ts");
const testing = await load("packages/mcp/src/testing/index.ts");
const config = await load("packages/coding-agent/src/extensions/mcp/config.ts");
const authStorage = await load("packages/coding-agent/src/core/auth-storage.ts");
const codingAgentOAuth = await load("packages/coding-agent/src/extensions/mcp/oauth.ts");
const extension = await load("packages/coding-agent/src/extensions/mcp/index.ts");
const runtime = await load("packages/coding-agent/src/extensions/mcp/runtime.ts");
const managerUi = await load("packages/coding-agent/src/extensions/mcp/ui.ts");
const keybindings = await load("packages/coding-agent/src/core/keybindings.ts");
const themeModule = await load("packages/coding-agent/src/modes/interactive/theme/theme.ts");
const tui = await load("packages/tui/src/index.ts");

export const LATEST_PROTOCOL_VERSION = mcp.LATEST_PROTOCOL_VERSION as string;
export const McpClient = mcp.McpClient as new (
    info: { name: string; version: string },
    options: { maxRetries: number },
) => {
    connect(transport: unknown): Promise<void>;
    close(): Promise<void>;
    listTools(): unknown[];
};
export const isJsonRpcRequest = mcp.isJsonRpcRequest as (message: unknown) => message is {
    id?: number;
    method: string;
    params?: unknown;
};
export const createInMemoryTransportPair = testing.createInMemoryTransportPair as () => {
    client: { close(): void };
    server: {
        start(): Promise<void>;
        onMessage(handler: (message: unknown) => void): void;
        send(message: unknown): Promise<void>;
    };
};
export const createMcpExtension = extension.createMcpExtension as (options?: Record<string, unknown>) => (api: unknown) => void;
export const createDefaultTransport = runtime.createDefaultTransport as (
    entry: McpServerEntry,
    cwd: string,
    authProvider: unknown,
) => unknown;
export const loadMcpConfig = config.loadMcpConfig as unknown;
export const FileAuthStorageBackend = authStorage.FileAuthStorageBackend as new (path: string) => unknown;
export const McpOAuthCredentialStore = codingAgentOAuth.McpOAuthCredentialStore as new (
    backend?: unknown,
    lockDir?: string,
) => {
    tokens(name: string, serverUrl: string): { access: string; refresh: string } | undefined;
    forServer(name: string, serverUrl: string): {
        save(state: {
            serverUrl: string;
            tokens?: { access_token: string; refresh_token: string; token_type?: string; expires_in?: number };
        }): Promise<void>;
    };
};
const runtimeLazy = await load("packages/coding-agent/src/extensions/mcp/runtime.lazy.ts");
export const createMcpAuthProvider = codingAgentOAuth.createMcpAuthProvider as (options: {
    serverUrl: string;
    store: unknown;
    settings: () => Record<string, unknown>;
    onChallenge: (challenge: unknown) => void;
}) => unknown;
export const signInMcpServer = runtime.signInMcpServer as (options: {
    serverUrl: string | URL;
    store: unknown;
    settings: Record<string, unknown>;
    challenge?: { resourceMetadataUrl?: string; scope?: string };
    prompt: {
        showAuthorizationUrl(url: URL): void;
        promptForRedirectUrl(signal?: AbortSignal): Promise<string | undefined>;
    };
    signal?: AbortSignal;
}) => Promise<void>;

export const loadMcpRuntime = runtimeLazy.loadMcpRuntime as () => Promise<{
    McpClient: new (info: { name: string; version: string }, options: { maxRetries: number }) => {
        connect(transport: unknown): Promise<void>;
        close(): Promise<void>;
        listTools(): unknown[];
    };
    createAuthProvider: (serverUrl: string | URL, store: unknown) => unknown;
    signInMcpServer(options: {
        serverUrl: string | URL;
        store: unknown;
        settings: Record<string, unknown>;
        challenge?: { resourceMetadataUrl?: string; scope?: string };
        prompt: {
            showAuthorizationUrl(url: URL): void;
            promptForRedirectUrl(signal?: AbortSignal): Promise<string | undefined>;
        };
        signal?: AbortSignal;
    }): Promise<void>;
    McpSignInCancelledError: new (...args: unknown[]) => Error;
}>;

export const McpManagerView = managerUi.McpManagerView as new (
    tui: unknown,
    theme: unknown,
    keybindings: unknown,
) => {
    handleInput(data: string): void;
    render(width: number): string[];
};
export const KeybindingsManager = keybindings.KeybindingsManager as new () => unknown;
export const initTheme = themeModule.initTheme as (name: string) => void;
export const theme = themeModule.theme as unknown;
export const setKeybindings = tui.setKeybindings as (manager: unknown) => void;

/** The part of pi's server entry this capture reads; the full type lives in the frozen checkout. */
export interface McpServerEntry {
    name: string;
    config: Record<string, unknown> & { url?: string; exposure?: string };
    source: string;
    scope?: "global" | "project" | "extension";
    override?: string;
}
