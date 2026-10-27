/**
 * Frozen pi v1.3.0 public modules for the `audit-1087` durable registry/hooks capture.
 *
 * The capture lives in the Pike repository, but the measured code is the private checkout named by
 * `PI_CHECKOUT`. Loading by absolute file URL keeps every import inside that checkout, so the
 * manifest revision guard is the authority for what was executed.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT;
if (checkout === undefined || checkout === "") throw new Error("PI_CHECKOUT must name the private pi v1.3.0 checkout");

async function load(relativePath: string): Promise<Record<string, any>> {
    return (await import(pathToFileURL(path.join(checkout!, relativePath)).href)) as Record<string, any>;
}

const durable = await load("packages/durable/src/index.ts");
const ai = await load("packages/ai/src/index.ts");
const chordContext = await load("packages/chord/src/context/index.ts");

export const Harness = durable.Harness;
export const MemoryStorage = durable.MemoryStorage;
export const createRegistry = durable.createRegistry;
export const defineExtension = durable.defineExtension;
export const defineTool = durable.defineTool;
export const hook = durable.hook;
export const section = durable.section;
export const GenerationTask = durable.GenerationTask;
export const ToolTask = durable.ToolTask;

export const createModels = ai.createModels;
export const fauxProvider = ai.fauxProvider;
export const fauxAssistantMessage = ai.fauxAssistantMessage;
export const fauxText = ai.fauxText;
export const fauxToolCall = ai.fauxToolCall;
export const Type = ai.Type;
export const BACKGROUND_CONTEXT = chordContext.BACKGROUND_CONTEXT;

/** One frozen observation: the comparison unit a later C++ test must reproduce. */
export interface Scenario {
    name: string;
    source: string;
    inputs: Record<string, unknown>;
    expected: unknown;
    normalizations?: string[];
}
