// Capture deterministic Remote delivery refusals from the named frozen pi baseline.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT is required");
const git = (args: string[]) => execFileSync("git", ["-C", checkout, ...args], { encoding: "utf8" }).trim();
if (git(["rev-parse", "HEAD"]) !== "71023aec1546d6d677a65669c5665c0927ce0dd0" || git(["status", "--porcelain"]))
    throw new Error("the pi checkout must be clean at the named baseline");
const { RemoteCodemodeSandbox, serveCodemodeRemote, loadQuickJSWasm } =
    await import(join(checkout, "packages/codemode/src/index.ts"));
const sandbox = new RemoteCodemodeSandbox({ remote: () => ({
    execute: (request: any, exchange: any) => serveCodemodeRemote(request, exchange,
        { wasm: loadQuickJSWasm(), maxInterruptBudget: 50000 }),
}) });
const controller = new AbortController();
const calls: any[] = [];
const idles: any[] = [];
const run = sandbox.start("await Promise.all([tools.wait(1), tools.wait(2)])", {
    startedAt: 1768471200000, seed: 1, tools: [{ name: "wait" }], signal: controller.signal,
    onCall: (call: any) => calls.push(call), onIdle: (seq: number, issued: number) => idles.push([seq, issued]),
});
const deadline = Date.now() + 5000;
while (calls.length !== 2 || idles.length === 0) {
    if (Date.now() > deadline) throw new Error("remote did not become idle");
    await new Promise((resolve) => setTimeout(resolve, 5));
}
const error = (fn: () => void) => { try { fn(); return null; } catch (e) { return (e as Error).message; } };
const unknown = error(() => run.deliver(1, 1768471200000, 99, { ok: true, value: null }));
run.deliver(2, 1768471200000, calls[1].id, { ok: true, value: null });
const duplicate = error(() => run.deliver(2, 1768471200000, calls[0].id, { ok: true, value: null }));
await new Promise((resolve) => setTimeout(resolve, 50));
const missing = { finished: run.finished, idles: [...idles] };
controller.abort(new Error("missing result"));
const result = await run.result;
const observed = { unknown, duplicate, missing,
    outcome: { ok: result.ok, error: result.ok ? null : result.error,
        calls: result.calls.map(({ name, status }: any) => ({ name, status })) } };
await sandbox.close();
const expected = { unknown: "Call 99 is not waiting for a result",
    duplicate: "Result number 2 is not a positive integer used once",
    missing: { finished: false, idles: [[0, 2]] },
    outcome: { ok: false, error: { kind: "aborted", message: "missing result" },
        calls: [{ name: "wait", status: "cancelled" }, { name: "wait", status: "ok" }] } };
if (JSON.stringify(observed) !== JSON.stringify(expected)) throw new Error(JSON.stringify(observed));
const environment = { platform: process.platform, arch: process.arch, libc: "glibc",
    glibcVersion: (process.report.getReport() as any).header.glibcVersionRuntime, node: process.version,
    tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "") };
if (environment.platform !== "linux" || environment.arch !== "x64" || !environment.glibcVersion)
    throw new Error("capture requires Linux x64 glibc");
const sourceEndpoints = ["packages/codemode/src/runtime/remote.ts:RemoteCodemodeSandbox,serveCodemodeRemote",
    "packages/codemode/src/runtime/execution.ts:CodemodeRun.deliver,Execution.finish"];
const scenario = { name: "remote deterministic delivery rejects duplicates and missing calls remain pending",
    inputs: { script: "await Promise.all([tools.wait(1), tools.wait(2)])", startedAt: 1768471200000,
        seed: 1, actions: ["unknown call 99", "deliver sequence 2", "duplicate sequence 2", "wait 50ms", "abort"] },
    expected, observed, normalizations: ["wall-clock call durations omitted"] };
const common = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0",
    revision: git(["rev-parse", "HEAD"]), ticket: "1099", family: "audit-1099", capturedAt: new Date().toISOString(), environment };
const content = JSON.stringify({ ...common, sourceEndpoints, scenarios: [scenario] }, null, 2) + "\n";
const hash = (value: string | Buffer) => createHash("sha256").update(value).digest("hex");
const source = "docs/research/pi-v1.3.0/capture-1099-remote.mts";
const sourceHash = hash(readFileSync(source));
const directory = "fixtures/pi-v1.3.0/audit-1099";
mkdirSync(directory, { recursive: true });
const manifest = { ...common, generator: { path: source, sha256: sourceHash,
    command: "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> TSX_TSCONFIG_PATH=<pi-checkout>/tsconfig.json <pi-checkout>/node_modules/.bin/tsx docs/research/pi-v1.3.0/capture-1099-remote.mts",
    sources: [{ path: source, sha256: sourceHash }] },
    artifacts: [{ path: "remote-failures.json", family: "audit-1099", bytes: Buffer.byteLength(content),
        sha256: hash(content), sourceEndpoints, scenarios: [scenario] }] };
writeFileSync(join(directory, "remote-failures.json"), content);
writeFileSync(join(directory, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
console.log(JSON.stringify(observed, null, 2));
