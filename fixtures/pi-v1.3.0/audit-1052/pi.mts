/**
 * Imports the frozen pi v1.3.0 modules from the checkout named by `PI_CHECKOUT`.
 *
 * The capture lives in the Pike repository, but the measured source is a separate private checkout.
 * Resolving through `PI_CHECKOUT` keeps the capture independent of any package files in the Pike tree
 * and makes the manifest's revision guard authoritative.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT ?? "/tmp/cch-1052-pi-frozen";

function load(relativePath: string): Promise<Record<string, unknown>> {
    return import(pathToFileURL(path.join(checkout, relativePath)).href) as Promise<Record<string, unknown>>;
}

const mcp = await load("packages/mcp/src/index.ts");
const testing = await load("packages/mcp/src/testing/index.ts");
const oauth = await load("packages/mcp/src/oauth/index.ts");
const config = await load("packages/coding-agent/src/extensions/mcp/config.ts");
const authStorage = await load("packages/coding-agent/src/core/auth-storage.ts");
const codingAgentOAuth = await load("packages/coding-agent/src/extensions/mcp/oauth.ts");
const tools = await load("packages/coding-agent/src/extensions/mcp/tools.ts");
const extension = await load("packages/coding-agent/src/extensions/mcp/index.ts");
const runtime = await load("packages/coding-agent/src/extensions/mcp/runtime.ts");

export const McpClient = mcp.McpClient;
export const StreamableHttpTransport = mcp.StreamableHttpTransport;
export const McpAuthRequiredError = mcp.McpAuthRequiredError;
export const isJsonRpcRequest = mcp.isJsonRpcRequest;
export const LATEST_PROTOCOL_VERSION = mcp.LATEST_PROTOCOL_VERSION;
export const createInMemoryTransportPair = testing.createInMemoryTransportPair;
export const McpOAuthProvider = oauth.McpOAuthProvider;
export const authorizeMcp = oauth.authorizeMcp;
export const OAuthIssuerMismatchError = oauth.OAuthIssuerMismatchError;
export const loadMcpConfig = config.loadMcpConfig;
export const InMemoryAuthStorageBackend = authStorage.InMemoryAuthStorageBackend;
export const createMcpAuthProvider = codingAgentOAuth.createMcpAuthProvider;
export const McpOAuthCredentialStore = codingAgentOAuth.McpOAuthCredentialStore;
export const McpSignInCancelledError = codingAgentOAuth.McpSignInCancelledError;
export const signInMcpServer = codingAgentOAuth.signInMcpServer;
export const convertMcpResult = tools.convertMcpResult;
export const createMcpExtension = extension.createMcpExtension;
export const McpServerConnection = runtime.McpServerConnection;

/** The part of pi's server entry this capture reads; the full type lives in the frozen checkout. */
export interface McpServerEntry {
    name: string;
    config: Record<string, unknown> & { url?: string; exposure?: string };
    source: string;
    scope?: "global" | "project" | "extension";
    override?: string;
}
