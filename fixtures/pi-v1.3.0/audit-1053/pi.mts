/**
 * Imports the frozen pi v1.3.0 modules from the private checkout named by `PI_CHECKOUT`.
 *
 * The capture lives in the Pike repository, but the measured source is a separate private checkout.
 * Resolving through `PI_CHECKOUT` keeps the capture independent of any package files in the Pike tree
 * and makes the manifest's revision guard authoritative. `capture-1053.mts` re-executes the script
 * under the checkout's own tsx and sets `NODE_OPTIONS=--conditions=source` so workspace package
 * specifiers select frozen source rather than a built `dist`.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT ?? "/tmp/cch-1053-pi-frozen";

function load(relativePath: string): Promise<Record<string, unknown>> {
    return import(pathToFileURL(path.join(checkout, relativePath)).href) as Promise<Record<string, unknown>>;
}

const codemode = await load("packages/codemode/src/index.ts");
const declarations = await load("packages/codemode/src/declarations.ts");
const source = await load("packages/codemode/src/source.ts");
const prelude = await load("packages/codemode/src/runtime/prelude-source.ts");
const applicationTool = await load("packages/coding-agent/src/extensions/codemode/tool.ts");
const applicationIndex = await load("packages/coding-agent/src/extensions/codemode/index.ts");

/** Public sandbox entry points (worker, inline, remote) and result/error types at runtime. */
export const CodemodeSandbox = codemode.CodemodeSandbox;
export const CodemodeSandboxBase = codemode.CodemodeSandboxBase;
export const InlineCodemodeSandbox = codemode.InlineCodemodeSandbox;
export const RemoteCodemodeSandbox = codemode.RemoteCodemodeSandbox;
export const serveCodemodeRemote = codemode.serveCodemodeRemote;
export const loadQuickJSWasm = codemode.loadQuickJSWasm;
export const DEFAULT_INTERRUPT_BUDGET = codemode.DEFAULT_INTERRUPT_BUDGET;
export const MAX_OUTPUT_CHARS = codemode.MAX_OUTPUT_CHARS;
export const MAX_OUTPUT_ITEMS = codemode.MAX_OUTPUT_ITEMS;
export const MAX_STORE_TOTAL_CHARS = codemode.MAX_STORE_TOTAL_CHARS;
export const MAX_STORE_VALUE_CHARS = codemode.MAX_STORE_VALUE_CHARS;

/** Declaration/public-source exports. */
export const DEFAULT_INPUT_SCHEMA_MAX_CHARS = declarations.DEFAULT_INPUT_SCHEMA_MAX_CHARS;
export const renderDeclarations = declarations.renderDeclarations;
export const renderToolOutputType = declarations.renderToolOutputType;
export const renderToolSample = declarations.renderToolSample;
export const renderToolSignature = declarations.renderToolSignature;
export const schemaToType = declarations.schemaToType;
export const toCodemodeIdentifier = declarations.toCodemodeIdentifier;
export const CODEMODE_OPTIONS_PREFIX = source.CODEMODE_OPTIONS_PREFIX;
export const CODEMODE_SOURCE_GRAMMAR = source.CODEMODE_SOURCE_GRAMMAR;
export const CodemodeSourceError = source.CodemodeSourceError;
export const parseCodemodeSource = source.parseCodemodeSource;

/** The prelude's output/store limits and the public prelude text (for the guest-boundary row). */
export const PRELUDE_SOURCE = prelude.PRELUDE_SOURCE;

/** Application integration: description/catalog, tool definition, and extension registration. */
export const CODEMODE_TOOL_NAME = applicationTool.CODEMODE_TOOL_NAME;
export const CODEMODE_STORE_ENTRY_TYPE = applicationTool.CODEMODE_STORE_ENTRY_TYPE;
export const DEFAULT_CODEMODE_INLINE_BUDGET = applicationTool.DEFAULT_CODEMODE_INLINE_BUDGET;
export const codemodeToolSystemPromptContribution = applicationTool.codemodeToolSystemPromptContribution;
export const codemodeSchema = applicationTool.codemodeSchema;
export const createCodemodeDescription = applicationTool.createCodemodeDescription;
export const createCodemodeToolDefinition = applicationTool.createCodemodeToolDefinition;
export const createCodemodeTool = applicationTool.createCodemodeTool;
export const getCodemodeCallableTools = applicationTool.getCodemodeCallableTools;
export const toCodemodeDeclaration = applicationTool.toCodemodeDeclaration;
export const createCodemodeExtension = applicationIndex.createCodemodeExtension;

/** The public layout of one script result; imported values are untyped through this loader. */
export interface CodemodeResultShape {
    ok: boolean;
    value?: unknown;
    output: unknown[];
    calls: unknown[];
    storeWrites?: { set: Record<string, unknown>; delete: string[] };
    error?: { kind: string; name?: string; message: string; stack?: string };
    during?: { seq: number; issued: number };
}
