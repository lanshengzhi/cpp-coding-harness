/**
 * `audit-1180` capture: the frozen pi v1.3.0 JSONL sidecar publication order for a
 * commit carrying two sidecars (issue #1180, owner ruling 2026-11-05 decision B).
 *
 * Both registered audit-1054 observations for this baseline carry one sidecar, so
 * neither pins the relative order of two sidecars. This capture closes that gap
 * with the discriminating shape #1180 names: one commit creates two documents in
 * write order `doc 9` then `doc 3`, which sorted-filename append order
 * (`doc-3.jsonl` before `doc-9.jsonl`) would reverse. The capture stays on the
 * public `JsonlStorage` boundary through the frozen NodeExecutionEnv with a
 * FileSystem spy recording only the public operation order.
 *
 * Recorded per option shape (fsync off and on):
 *   - the public FileSystem append order (sidecars in write order, then the marker),
 *   - the flush order with fsync on (the same append order),
 *   - the resulting store file bytes for content comparison.
 *
 * The script refuses to write evidence unless the private checkout is the exact
 * baseline revision and its tracked source is clean. It re-executes itself under
 * that checkout's tsx so the recorded `tsx` version belongs to the install that
 * ran the capture.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/home/lansy/Work/github/coding-agent/pi PI_BASELINE=pi-v1.3.0 \
 *     /home/lansy/Work/github/coding-agent/pi/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1180/capture-1180-sidecar-order.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1180";
const TICKET = "1180";
const BASELINE = process.env.PI_BASELINE ?? "pi-v1.3.0";
const CHECKOUT = process.env.PI_CHECKOUT;
const EXPECTED_REVISION = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const TSX = CHECKOUT ? join(CHECKOUT, "node_modules/.bin/tsx") : "";

if (!CHECKOUT) {
    console.error("PI_CHECKOUT must name the private frozen pi v1.3.0 checkout");
    process.exit(1);
}
if (!existsSync(TSX)) {
    console.error(`the checkout has no tsx: ${TSX}`);
    process.exit(1);
}

function run(command: string, arguments_: string[], cwd = REPO): string {
    return execFileSync(command, arguments_, { cwd, encoding: "utf8" }).trim();
}

function sha256(content: Buffer | string): string {
    return createHash("sha256").update(content).digest("hex");
}

if (!process.env.AUDIT_1180_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1180_TSX: "1",
            NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
            TSX_TSCONFIG_PATH: join(CHECKOUT, "tsconfig.json"),
        },
        stdio: "inherit",
    });
    process.exit(0);
}

const revision = run("git", ["rev-parse", "HEAD"], CHECKOUT);
if (revision !== EXPECTED_REVISION) {
    console.error(`checkout revision ${revision} is not ${EXPECTED_REVISION}`);
    process.exit(1);
}
const dirty = run("git", ["status", "--porcelain"], CHECKOUT);
if (dirty !== "") {
    console.error(`checkout is not clean:\n${dirty}`);
    process.exit(1);
}
if (process.platform !== "linux" || process.arch !== "x64") {
    console.error(`unsupported platform ${process.platform}-${process.arch}`);
    process.exit(1);
}
const report = process.report?.getReport() as { header?: { glibcVersionRuntime?: string } } | undefined;
if (!report?.header?.glibcVersionRuntime) {
    console.error("the captured environment is not glibc Linux");
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/durable/src/storage/jsonl/storage.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/durable/src/storage/jsonl/storage.ts is missing");
    process.exit(1);
}

const registryPath = join(REPO, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { revision?: string; bundles?: Record<string, unknown> }>;
};
const registered = registry.baselines[BASELINE];
if (registered?.revision !== EXPECTED_REVISION) {
    console.error(`baseline ${BASELINE} does not resolve to ${EXPECTED_REVISION}`);
    process.exit(1);
}
if (registered.bundles?.[FAMILY] !== undefined) {
    console.error(`${FAMILY} is already registered; a deliberate recapture needs an explicit registry replacement`);
    process.exit(1);
}

const { NodeExecutionEnv } = await import(join(CHECKOUT, "packages/durable/src/env/node.ts"));
const { JsonlStorage } = await import(join(CHECKOUT, "packages/durable/src/storage/jsonl/storage.ts"));
const { BACKGROUND_CONTEXT: context } = await import(join(CHECKOUT, "packages/chord/src/context/index.ts"));

type SpyFs = InstanceType<typeof NodeExecutionEnv> & {};
const operations: string[] = [];
const makeSpy = (inner: SpyFs): SpyFs =>
    new Proxy(inner, {
        get(target, prop, receiver) {
            const value = Reflect.get(target, prop, receiver);
            if (typeof value !== "function") return value;
            if (prop === "appendFile" || prop === "flushFile" || prop === "remove") {
                return async (path: string, ...rest: unknown[]) => {
                    operations.push(`${String(prop)} ${String(path).split("/").pop()}`);
                    return (value as (...args: unknown[]) => unknown).call(target, path, ...rest);
                };
            }
            return (value as (...args: unknown[]) => unknown).bind(target);
        },
    }) as SpyFs;

const docCreate = (id: number, text: string) => ({
    type: "document.create",
    // Session scope: the natural storage write shape; the frozen marker
    // record then matches what the C++ encoder emits for the same write.
    record: { id, kind: "note", key: `k${id}`, scope: { kind: "session" } },
    content: { kind: "base", version: 1, value: { text } },
});

type Scenario = {
    readonly name: string;
    readonly inputs: Record<string, unknown>;
    readonly expected: Record<string, unknown>;
    readonly normalizations: readonly string[];
};

async function capture(fsync: boolean): Promise<Scenario> {
    const tmp = mkdtempSync(join(tmpdir(), "audit-1180-"));
    try {
        const store = join(tmp, "store");
        operations.length = 0;
        const fs = makeSpy(new NodeExecutionEnv({ cwd: tmp }));
        const storage = await JsonlStorage.open(store, fs as never, context, { fsync });
        await storage.commit([docCreate(9, "nine"), docCreate(3, "three")], context);
        await storage.close(context);
        const names = readdirSync(store).sort();
        const files = names.map((name) => {
            const text = readFileSync(join(store, name), "utf8");
            return { name, bytes: Buffer.byteLength(text), sha256: sha256(text), text };
        });
        const appends = operations.filter((entry) => entry.startsWith("appendFile "));
        const flushes = operations.filter((entry) => entry.startsWith("flushFile "));
        return {
            name: fsync
                ? "JSONL fsync flushes two sidecars in append order before the main marker"
                : "a JSONL commit appends two sidecars in write order before the main marker",
            inputs: {
                backend: "JsonlStorage",
                options: { fsync },
                commit: "create document 9 (key k9) then document 3 (key k3) in one batch",
                observation: "public FileSystem append and flush order",
            },
            expected: {
                appendOrder: appends.map((entry) => entry.slice("appendFile ".length)),
                flushOrder: flushes.map((entry) => entry.slice("flushFile ".length)),
                files,
            },
            normalizations: [],
        };
    } finally {
        rmSync(tmp, { recursive: true, force: true });
    }
}

const scenarios = [await capture(false), await capture(true)];

const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: run(TSX, ["--version"]).split("\n")[0].replace(/^tsx /, ""),
};

const capturedAt = new Date().toISOString();
const sourceEndpoints = [
    "packages/durable/src/storage/jsonl/storage.ts:JsonlStorage.commit,JsonlStorage.encodeCommit",
    "packages/durable/src/env/node.ts:NodeExecutionEnv.appendFile,NodeExecutionEnv.flushFile",
    "packages/durable/test/jsonl-storage.test.ts:Pico JsonlStorage publication and recovery",
];
const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision: EXPECTED_REVISION,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    sourceEndpoints,
    scenarios,
};
const artifactPath = join(REPO, `fixtures/pi-v1.3.0/${FAMILY}/sidecar-order.json`);
mkdirSync(dirname(artifactPath), { recursive: true });
writeFileSync(artifactPath, `${JSON.stringify(artifact, null, 1)}\n`);
console.log(`wrote ${artifactPath}`);
console.log(`sha256 ${sha256(readFileSync(artifactPath))}`);
