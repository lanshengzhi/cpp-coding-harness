/**
 * Helpers shared by the audit-1054 capture scenarios.
 *
 * Every helper stays on a public boundary of frozen pi v1.3.0: a `Storage` implementation
 * (`MemoryStorage`, `JsonlStorage`, `SqliteStorage`), the `Session` built by `createSession`, the
 * public `FileSystem` adapter a JSONL store accepts, or read-only inspection of a store file the
 * public implementation just closed. No private scheduler, Session field or SQL row is mutated to
 * produce an observation.
 */

import { createHash } from "node:crypto";
import { basename } from "node:path";
import { gzipSync } from "node:zlib";
import { mkdtemp, readFile, readdir, rm, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

export type Scenario = {
    readonly name: string;
    readonly inputs: Record<string, unknown>;
    readonly expected: Record<string, unknown>;
    readonly normalizations: readonly string[];
};

export function sha256(content: Buffer | string): string {
    return createHash("sha256").update(content).digest("hex");
}

export function flush(): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, 0));
}

export async function temporaryDirectory(prefix: string): Promise<string> {
    return mkdtemp(join(tmpdir(), prefix));
}

export async function removeDirectory(directory: string): Promise<void> {
    await rm(directory, { recursive: true, force: true });
}

export type StoreFile = {
    readonly name: string;
    readonly bytes: number;
    readonly sha256: string;
    readonly text: string;
    readonly lines: readonly unknown[];
};

/** Every JSONL file of a store directory, sorted by name, with its parsed lines. */
export async function readJsonlStore(directory: string): Promise<StoreFile[]> {
    const names = (await readdir(directory)).filter((name) => name.endsWith(".jsonl")).sort();
    const files: StoreFile[] = [];
    for (const name of names) {
        const bytes = await readFile(join(directory, name));
        const text = bytes.toString("utf8");
        const lines = text
            .split("\n")
            .filter((line) => line !== "")
            .map((line) => JSON.parse(line) as unknown);
        files.push({ name, bytes: bytes.length, sha256: sha256(bytes), text, lines });
    }
    return files;
}

/** The equation of a SQLite store file the public adapter closed: raw bytes plus a compact copy. */
export async function readSqliteStore(path: string): Promise<{
    readonly file: string;
    readonly bytes: number;
    readonly sha256: string;
    readonly encoding: "gzip+base64";
    readonly gzipBase64: string;
}> {
    const content = await readFile(path);
    return {
        file: basename(path),
        bytes: content.length,
        sha256: sha256(content),
        encoding: "gzip+base64",
        gzipBase64: gzipSync(content).toString("base64"),
    };
}

export async function fileNames(directory: string): Promise<string[]> {
    return (await readdir(directory)).sort();
}

export async function fileSize(path: string): Promise<number> {
    return (await stat(path)).size;
}

export async function rejection(promise: Promise<unknown>): Promise<Record<string, unknown>> {
    try {
        await promise;
        return { rejected: false };
    } catch (error) {
        const value = error as { name?: string; message?: string };
        return { rejected: true, name: value.name ?? "Error", message: value.message ?? String(error) };
    }
}

/** A storage failure observed through the public `Session.closed` promise. */
export async function sessionEnd(session: any): Promise<Record<string, unknown>> {
    const end = await session.closed;
    if (end.reason === "closed") return { reason: "closed" };
    return { reason: "failed", errorMessage: end.error instanceof Error ? end.error.message : String(end.error) };
}

export type ControlledStorage = {
    /** The `Storage` handed to `createSession`. */
    readonly storage: any;
    /** Exact detached batches admitted through the public `Storage.commit`. */
    readonly admitted: readonly unknown[][];
    /** Hold the next commit after recording it; resolve `entered`, then wait for `release`. */
    holdCommits(): { readonly entered: Promise<void>; release(): void };
    /** Make the next commit throw before it reaches the wrapped storage. */
    failNextCommit(error: Error): void;
};

/**
 * A `Storage` wrapper that can hold or fail one commit. Holding observes what a caller sees while
 * a commit is admitted but not yet settled; failing observes the documented fatal storage error.
 * Every other method delegates to the wrapped public storage implementation.
 */
export function controlledStorage(inner: any): ControlledStorage {
    const admitted: unknown[][] = [];
    let hold: { entered: () => void; gate: Promise<void>; release: () => void } | undefined;
    let failure: Error | undefined;

    const storage = new Proxy(inner, {
        get(target, property, receiver) {
            if (property === "commit") {
                return async (writes: readonly unknown[], context: unknown) => {
                    admitted.push(structuredClone(writes));
                    const held = hold;
                    if (held !== undefined) {
                        hold = undefined;
                        held.entered();
                        await held.gate;
                    }
                    if (failure !== undefined) {
                        const error = failure;
                        failure = undefined;
                        throw error;
                    }
                    return target.commit(writes, context);
                };
            }
            const value = Reflect.get(target, property, receiver);
            return typeof value === "function" ? value.bind(target) : value;
        },
    });

    return {
        storage,
        admitted,
        holdCommits() {
            let entered!: () => void;
            let release!: () => void;
            const enteredPromise = new Promise<void>((resolve) => (entered = resolve));
            const gate = new Promise<void>((resolve) => (release = resolve));
            hold = { entered, gate, release };
            return { entered: enteredPromise, release: () => release() };
        },
        failNextCommit(error: Error) {
            failure = error;
        },
    };
}

export type RecordingFileSystem = {
    /** The `FileSystem` handed to `JsonlStorage.open`. */
    readonly fs: any;
    /** Basenames of append calls in call order. */
    readonly appends: string[];
    /** Basenames of flush calls in call order. */
    readonly flushes: string[];
    /** Basenames of remove calls in call order. */
    readonly removes: string[];
    /** Make the next append to this basename fail with a public file error. */
    failNextAppendOf(file: string): void;
};

/** The selected public calls of a `FileSystem`, in call order; everything else delegates. */
export function recordingFileSystem(base: any): RecordingFileSystem {
    const appends: string[] = [];
    const flushes: string[] = [];
    const removes: string[] = [];
    let failAppend: string | undefined;

    const fs = new Proxy(base, {
        get(target, property, receiver) {
            if (property === "appendFile") {
                return async (path: string, content: unknown, context: unknown) => {
                    const file = basename(path);
                    appends.push(file);
                    if (failAppend === file) {
                        failAppend = undefined;
                        return { ok: false, error: targetError(path) };
                    }
                    return target.appendFile(path, content, context);
                };
            }
            if (property === "flushFile") {
                return async (path: string, context: unknown) => {
                    flushes.push(basename(path));
                    return target.flushFile(path, context);
                };
            }
            if (property === "remove") {
                return async (path: string, removeOptions: unknown, context: unknown) => {
                    removes.push(basename(path));
                    return target.remove(path, removeOptions, context);
                };
            }
            const value = Reflect.get(target, property, receiver);
            return typeof value === "function" ? value.bind(target) : value;
        },
    });

    function targetError(path: string): Error {
        const error = new Error(`injected append failure for ${basename(path)}`) as Error & { code: string; path: string };
        error.name = "FileError";
        error.code = "unknown";
        error.path = path;
        return error;
    }

    return {
        fs,
        appends,
        flushes,
        removes,
        failNextAppendOf(file: string) {
            failAppend = file;
        },
    };
}

export type CountingStorage = {
    readonly storage: any;
    readonly counts: { document: number; findDocument: number };
};

/** Count the public document reads a retention configuration causes. */
export function countingStorage(inner: any): CountingStorage {
    const counts = { document: 0, findDocument: 0 };
    const storage = new Proxy(inner, {
        get(target, property, receiver) {
            if (property === "document") {
                return async (...args: unknown[]) => {
                    counts.document++;
                    return target.document(...args);
                };
            }
            if (property === "findDocument") {
                return async (...args: unknown[]) => {
                    counts.findDocument++;
                    return target.findDocument(...args);
                };
            }
            const value = Reflect.get(target, property, receiver);
            return typeof value === "function" ? value.bind(target) : value;
        },
    });
    return { storage, counts };
}

/** Compact a committed publication to the fields the comparison scenarios name. */
export function summarizePublications(publications: readonly any[]): unknown[] {
    return publications.map((publication) => ({
        seq: publication.seq,
        changes: publication.changes.map((change: any) => {
            if (change.type === "document" || change.type === "document.copy") {
                return {
                    type: change.type,
                    recordId: change.record.id,
                    kind: change.record.kind,
                    value: change.value ?? null,
                    ops: change.ops ?? [],
                    source: change.source ?? null,
                };
            }
            return { type: change.type, id: change.value.id, kind: change.kind ?? null };
        }),
    }));
}
