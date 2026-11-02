/**
 * Imports the frozen pi v1.3.0 modules named by `PI_CHECKOUT`.
 *
 * The capture lives in the Pike repository, but the measured source is a separate private checkout.
 * Resolving through `PI_CHECKOUT` keeps the capture independent of any package files in the Pike
 * tree and makes the manifest's revision guard authoritative.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT ?? "/tmp/cch-1080-pi-frozen";

function load(relativePath: string): Promise<Record<string, any>> {
    return import(pathToFileURL(path.join(checkout, relativePath)).href) as Promise<Record<string, any>>;
}

const mcp = await load("packages/mcp/src/index.ts");
const testing = await load("packages/mcp/src/testing/index.ts");
const jsonrpc = await load("packages/mcp/src/protocol/jsonrpc.ts");
const tools = await load("packages/coding-agent/src/extensions/mcp/tools.ts");
const runtime = await load("packages/coding-agent/src/extensions/mcp/runtime.ts");
const extension = await load("packages/coding-agent/src/extensions/mcp/index.ts");

export const McpClient = mcp.McpClient;
export const StdioTransport = mcp.StdioTransport;
export const StreamableHttpTransport = mcp.StreamableHttpTransport;
export const LATEST_PROTOCOL_VERSION = mcp.LATEST_PROTOCOL_VERSION;
export const isJsonRpcRequest = mcp.isJsonRpcRequest;
export const createInMemoryTransportPair = testing.createInMemoryTransportPair;
export const McpError = jsonrpc.McpError;
export const McpAbortError = jsonrpc.McpAbortError;
export const McpTimeoutError = jsonrpc.McpTimeoutError;
export const createMcpToolName = tools.createMcpToolName;
export const createMcpToolDefinition = tools.createMcpToolDefinition;
export const createDefaultTransport = runtime.createDefaultTransport;
export const McpServerConnection = runtime.McpServerConnection;
export const createMcpExtension = extension.createMcpExtension;
