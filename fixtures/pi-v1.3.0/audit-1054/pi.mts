/**
 * Imports the frozen pi v1.3.0 durable modules from the checkout named by `PI_CHECKOUT`.
 *
 * The capture lives in the Pike repository, but the measured source is a separate private
 * checkout. Resolving through `PI_CHECKOUT` keeps the capture independent of any package files
 * in the Pike tree and makes the manifest's revision guard authoritative. This module is loaded
 * only after `capture-1054.mts` verified the checkout revision and that its tracked source is
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
const jsonl = await load("packages/durable/src/storage/jsonl/storage.ts");
const jsonlNode = await load("packages/durable/src/storage/jsonl/node.ts");
const sqliteNode = await load("packages/durable/src/storage/sqlite/node.ts");
const envNode = await load("packages/durable/src/env/node.ts");
const chordContext = await load("packages/chord/src/context/index.ts");

export const createSession = durable.createSession as (...args: unknown[]) => any;
export const defineDoc = durable.defineDoc as (definition: unknown) => any;
export const defineDocFamily = durable.defineDocFamily as (definition: unknown) => any;
export const defineEntry = durable.defineEntry as (kind: string) => any;
export const defineTask = durable.defineTask as (definition: unknown) => any;
export const MemoryStorage = durable.MemoryStorage as new () => any;
export const JsonlStorage = jsonl.JsonlStorage as new () => any;
export const JsonlCorruptionError = jsonl.JsonlCorruptionError as new (...args: unknown[]) => Error;
export const JsonlStoragePoisonedError = jsonl.JsonlStoragePoisonedError as new (...args: unknown[]) => Error;
export const openNodeJsonlStorage = jsonlNode.openNodeJsonlStorage as (...args: unknown[]) => Promise<any>;
export const openNodeSqliteStorage = sqliteNode.openNodeSqliteStorage as (...args: unknown[]) => Promise<any>;
export const NodeExecutionEnv = envNode.NodeExecutionEnv as new (options: unknown) => any;
export const FileError = envNode.FileError as new (...args: unknown[]) => Error;
export const BACKGROUND_CONTEXT = chordContext.BACKGROUND_CONTEXT as Record<string, unknown>;

/** The durable package version this capture measures; the manifest pins the full revision. */
export const DURABLE_PACKAGE_VERSION = "1.3.0";
