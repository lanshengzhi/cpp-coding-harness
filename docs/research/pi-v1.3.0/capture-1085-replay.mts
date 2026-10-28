// Bounded local probe of frozen pi's public Harness and JSONL continuation.
// Run with NODE_OPTIONS=--conditions=source and the frozen checkout's tsx.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const checkout = "/tmp/cch-1080-pi-frozen";
const revision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const git = (arg: string[]) => execFileSync("git", ["-C", checkout, ...arg], { encoding: "utf8" }).trim();
if (git(["rev-parse", "HEAD"]) !== revision || git(["status", "--porcelain"]) !== "")
    throw new Error("frozen pi checkout must be clean at the named revision");
const directory = process.argv[2];
if (!directory) throw new Error("supply an empty local probe directory");
mkdirSync(directory, { recursive: true });
if (readdirSync(directory).length) throw new Error("probe directory is not empty");
const { Harness, defineTask, createRegistry } = await import(join(checkout, "packages/durable/src/index.ts"));
const { openNodeJsonlStorage } = await import(join(checkout, "packages/durable/src/storage/jsonl/node.ts"));
const { BACKGROUND_CONTEXT: context } = await import(join(checkout, "packages/chord/src/context/index.ts"));
const seen: unknown[] = [];
const task = defineTask({
    name: "audit1085.work", version: 1,
    initial: () => ({ phase: "start", n: 7 }),
    phases: { start: async (record: any, runtime: any, ctx: any) => {
        seen.push(record.state.checkpoint);
        await runtime.commit(() => ({ status: "terminal", outcome: { status: "completed", result: 17 } }), ctx);
    } },
    abort: async () => {},
});
const registry = createRegistry();
registry.install({ name: "audit1085", tasks: [task] });
let storage = await openNodeJsonlStorage(directory, context);
let harness = await Harness.open(storage, { registry }, context);
const root = await harness.root(context);
const id = await root.commit((tx: any) => tx.createTask(task, {}, { ownership: { kind: "conversation" } }), context);
await harness.close(context);
const files = Object.fromEntries(readdirSync(directory).sort().map((name) => [name, readFileSync(join(directory, name), "utf8")]));
storage = await openNodeJsonlStorage(directory, context);
harness = await Harness.open(storage, { registry }, context);
harness.resume();
const receipt = await harness.waitForTask(id, context);
await harness.close(context);
const capturedAt = new Date().toISOString();
const environment = { platform: process.platform, arch: process.arch, libc: "glibc",
    glibcVersion: (process.report.getReport() as any).header.glibcVersionRuntime,
    node: process.version, tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "") };
if (environment.platform !== "linux" || environment.arch !== "x64" || !environment.glibcVersion)
    throw new Error("capture requires Linux x64 glibc");
const scenario = { name: "pi JSONL Harness resumes a pending checkpoint after close and reopen",
    inputs: { task: "audit1085.work", initial: { phase: "start", n: 7 }, action: "close, reopen, resume" },
    expected: { id, files, seen, state: receipt.state } };
const sourceEndpoints = ["packages/durable/src/harness/harness.ts:Harness.open,resume,waitForTask,close",
    "packages/durable/src/storage/jsonl/storage.ts:JsonlStorage", "packages/durable/src/tasks.ts:defineTask"];
const common = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0", revision,
    ticket: "1085", family: "audit-1085-replay", capturedAt, environment };
const bytes = JSON.stringify({ ...common, sourceEndpoints, scenarios: [scenario] }, null, 2) + "\n";
const bundle = process.argv[3];
if (!bundle) throw new Error("supply a new evidence output directory");
mkdirSync(bundle, { recursive: true });
if (readdirSync(bundle).length) throw new Error("evidence output directory is not empty");
const hash = (content: string | Buffer) => createHash("sha256").update(content).digest("hex");
const script = "docs/research/pi-v1.3.0/capture-1085-replay.mts";
const scriptHash = hash(readFileSync(script));
const manifest = { ...common, generator: { path: script, sha256: scriptHash,
    command: `NODE_OPTIONS=--conditions=source TSX_TSCONFIG_PATH=${checkout}/tsconfig.json ${checkout}/node_modules/.bin/tsx ${script} ${directory} ${bundle}`,
    sources: [{ path: script, sha256: scriptHash }] },
    artifacts: [{ path: "harness.json", family: "audit-1085-replay", bytes: Buffer.byteLength(bytes), sha256: hash(bytes), sourceEndpoints, scenarios: [scenario] }] };
writeFileSync(join(bundle, "harness.json"), bytes);
writeFileSync(join(bundle, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
