/**
 * Imports the frozen pi v1.3.0 durable modules from the checkout named by `PI_CHECKOUT`.
 *
 * The capture lives in the Pike repository, but the measured source is a separate private
 * checkout. Resolving through `PI_CHECKOUT` keeps the capture independent of any package files
 * in the Pike tree and makes the manifest's revision guard authoritative. This module is loaded
 * only after `capture-1063.mts` verified the checkout revision and that its tracked source is
 * clean.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT ?? "";

if (checkout === "") {
    throw new Error("PI_CHECKOUT must name the private frozen pi v1.3.0 checkout");
}

function load(relativePath: string): Promise<Record<string, unknown>> {
    return import(pathToFileURL(path.join(checkout, relativePath)).href) as Promise<Record<string, unknown>>;
}

const durable = await load("packages/durable/src/index.ts");
const chordContext = await load("packages/chord/src/context/index.ts");
const chordDelta = await load("packages/chord/src/delta/index.ts");

export const createSession = durable.createSession as (...args: unknown[]) => any;
export const defineDoc = durable.defineDoc as (definition: unknown) => any;
export const defineEntry = durable.defineEntry as (kind: string) => any;
export const MemoryStorage = durable.MemoryStorage as new () => any;
export const BACKGROUND_CONTEXT = chordContext.BACKGROUND_CONTEXT as Record<string, unknown>;
export const applyImmutable = chordDelta.applyImmutable as (target: unknown, ops: readonly unknown[]) => any;

/** The durable package version this capture measures; the manifest pins the full revision. */
export const DURABLE_PACKAGE_VERSION = "1.3.0";
