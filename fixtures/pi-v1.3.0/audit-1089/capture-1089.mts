import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, relative } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const checkout = process.env.PI_CHECKOUT!;
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: checkout, encoding: "utf8" }).trim();
if (revision !== "71023aec1546d6d677a65669c5665c0927ce0dd0" || execFileSync("git", ["status", "--porcelain"], { cwd: checkout, encoding: "utf8" }).trim()) throw new Error("frozen checkout is not clean at baseline");
const load = (path: string) => import(pathToFileURL(join(checkout, path)).href);
const { NodeExecutionEnv } = await load("packages/durable/src/env/node.ts");
const { BACKGROUND_CONTEXT: context, withCancel } = await load("packages/chord/src/context/index.ts");
const directory = mkdtempSync(join(tmpdir(), "pi-1089-"));
const env = new NodeExecutionEnv({ cwd: directory });
const bounded = <T>(promise: Promise<T>): Promise<T> => Promise.race([promise, new Promise<never>((_, reject) => { const timer = setTimeout(() => reject(new Error("bounded wait expired")), 5000); timer.unref(); })]);
try {
    let count = 0;
    let notify!: () => void;
    const changed = new Promise<void>(resolve => { notify = resolve; });
    const opened = await env.watch([{ path: "missing.txt" }], () => { count++; notify(); }, context);
    if (!opened.ok) throw opened.error;
    await writeFile(join(directory, "missing.txt"), "first");
    await bounded(changed);
    await opened.value.close(context);
    const afterClose = count;
    await writeFile(join(directory, "missing.txt"), "second");
    // A subsequent, independent watch confirms the second mutation has propagated.
    let syncNotify!: () => void;
    const syncChanged = new Promise<void>(resolve => { syncNotify = resolve; });
    const sync = await env.watch([{ path: "sync.txt" }], () => syncNotify(), context);
    if (!sync.ok) throw sync.error;
    await writeFile(join(directory, "sync.txt"), "sync");
    await bounded(syncChanged);
    await sync.value.close(context);
    const watch = { observedAfterReturn: afterClose > 0, callbacksStableAfterClose: count === afterClose, mode: opened.value.mode };

    const normal = await env.exec(["/bin/sh", "-c", "exit 7"], undefined, context);
    const cancel = withCancel(context);
    let started!: () => void;
    const ready = new Promise<void>(resolve => { started = resolve; });
    const pending = env.exec(["/bin/sh", "-c", "printf ready; exec sleep 30"], { onOutput: (text: string) => { if (text.includes("ready")) started(); } }, cancel.context);
    await bounded(ready);
    cancel.cancel();
    const aborted = await bounded(pending);
    const timed = await bounded(env.exec(["/bin/sh", "-c", "exec sleep 30"], { timeout: 0.1 }, context));
    const execution = { normal: normal.ok ? { ok: true, exitCode: normal.value.exitCode } : { ok: false, code: normal.error.code }, cancelled: aborted.ok ? { ok: true } : { ok: false, code: aborted.error.code, message: aborted.error.message }, timeout: timed.ok ? { ok: true } : { ok: false, code: timed.error.code, message: timed.error.message } };
    const scenarios = [
        { name: "watch covers creation after return and stops callbacks after close", inputs: { target: "missing file", mutations: ["create after watch returns", "write after close"] }, expected: watch, normalizations: ["temporary paths omitted"] },
        { name: "normal exit cancellation and timeout have distinct shell outcomes", inputs: { normalExit: 7, cancelledCommand: "wait after readiness output", timeoutSeconds: 0.1 }, expected: execution, normalizations: ["process identifiers omitted"] },
    ];
    const capturedAt = new Date().toISOString();
    const environment = { platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: (process.report!.getReport() as any).header.glibcVersionRuntime, node: process.version, tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "") };
    const endpoints = ["packages/durable/src/env/node.ts:NodeExecutionEnv.watch,exec", "packages/durable/src/env/node-watch.ts:NodeFileWatcher.close"];
    const base = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0", revision, ticket: "1089", family: "audit-1089", capturedAt, environment };
    const artifact = JSON.stringify({ ...base, sourceEndpoints: endpoints, scenarios }, null, 2) + "\n";
    const hash = (value: string | Buffer) => createHash("sha256").update(value).digest("hex");
    const folder = dirname(script);
    await writeFile(join(folder, "environment.json"), artifact);
    const sources = [{ path: relative(repo, script), sha256: hash(readFileSync(script)) }];
    const manifest = { ...base, generator: { path: relative(repo, script), sha256: hash(readFileSync(script)), command: "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1089/capture-1089.mts", sources }, artifacts: [{ path: "environment.json", family: "audit-1089", bytes: Buffer.byteLength(artifact), sha256: hash(artifact), sourceEndpoints: endpoints, scenarios }] };
    await writeFile(join(folder, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
} finally {
    await env.cleanup(context);
    rmSync(directory, { recursive: true, force: true });
}
