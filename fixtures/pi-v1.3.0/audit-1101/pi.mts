/**
 * Imports the frozen pi v1.3.0 modules from the checkout named by `PI_CHECKOUT`.
 *
 * The capture lives in the Pike repository, but the measured source is a separate private checkout.
 * Resolving through `PI_CHECKOUT` keeps the capture independent of any package files in the Pike tree
 * and makes the manifest's revision guard authoritative.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT ?? "/tmp/cch-1050-pi-frozen";

function load(relativePath: string): Promise<Record<string, unknown>> {
    return import(pathToFileURL(path.join(checkout, relativePath)).href) as Promise<Record<string, unknown>>;
}

const mcp = await load("packages/mcp/src/index.ts");
const testing = await load("packages/mcp/src/testing/index.ts");
const tools = await load("packages/coding-agent/src/extensions/mcp/tools.ts");

export const McpClient = mcp.McpClient;
export const createInMemoryTransportPair = testing.createInMemoryTransportPair;
export const isJsonRpcRequest = mcp.isJsonRpcRequest;
export const convertMcpResult = tools.convertMcpResult;
