/**
 * Frozen pi v1.3.0 modules for the `audit-1057` Chord dependency capture.
 *
 * The capture lives in the Pike repository, but the measured code is the private checkout named by
 * `PI_CHECKOUT`. Loading by absolute file URL keeps every import inside that checkout, so the
 * manifest revision guard is the authority for what was executed. The recorded package name is
 * `@earendil-works/chord`; there is no `pi-chord` package in the frozen release.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT;
if (checkout === undefined || checkout === "") throw new Error("PI_CHECKOUT must name the private pi v1.3.0 checkout");

async function load(relativePath: string): Promise<Record<string, any>> {
    return (await import(pathToFileURL(path.join(checkout!, relativePath)).href)) as Record<string, any>;
}

const durable = await load("packages/durable/src/index.ts");
const context = await load("packages/chord/src/context/index.ts");
const delta = await load("packages/chord/src/delta/index.ts");
const state = await load("packages/chord/src/index.ts");
const output = await load("packages/chord/src/output.ts");

export const createSession = durable.createSession;
export const MemoryStorage = durable.MemoryStorage;
export const defineDoc = durable.defineDoc;

export const BACKGROUND_CONTEXT = context.BACKGROUND_CONTEXT;
export const TODO_CONTEXT = context.TODO_CONTEXT;
export const createContextKey = context.createContextKey;
export const withContextValue = context.withContextValue;
export const withAbortSignal = context.withAbortSignal;
export const withoutAbortSignal = context.withoutAbortSignal;
export const withCancel = context.withCancel;
export const awaitWithContext = context.awaitWithContext;

export const track = delta.track;
export const apply = delta.apply;
export const applyImmutable = delta.applyImmutable;
export const applyImmutableBatches = delta.applyImmutableBatches;
export const assertValidOp = delta.assertValidOp;
export const isBase = delta.isBase;
export const isReplace = delta.isReplace;
export const overlap = delta.overlap;

export const replicatedState = state.replicatedState;
export const copyJson = state.copyJson;
export const isJsonValue = state.isJsonValue;

export const boundOutput = output.boundOutput;
export const OutputBuffer = output.OutputBuffer;
export const Progress = output.Progress;
export const PROGRESS_BYTES_PER_SECOND = output.PROGRESS_BYTES_PER_SECOND;
export const characterEnd = output.characterEnd;
export const utf8ByteLength = output.utf8ByteLength;

/** One frozen observation: the comparison unit a later C++ test must reproduce. */
export interface Scenario {
    name: string;
    source: string;
    inputs: unknown;
    expected: unknown;
    normalizations?: string[];
}
