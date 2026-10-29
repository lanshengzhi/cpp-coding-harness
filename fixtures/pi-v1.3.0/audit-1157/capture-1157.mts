import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const checkout = process.env.PI_CHECKOUT!;
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: checkout, encoding: "utf8" }).trim();
if (revision !== "71023aec1546d6d677a65669c5665c0927ce0dd0" || execFileSync("git", ["status", "--porcelain"], { cwd: checkout, encoding: "utf8" }).trim()) throw new Error("frozen checkout is not clean at baseline");
const load = (path: string) => import(pathToFileURL(join(checkout, path)).href);
const { NodeExecutionEnv } = await load("packages/durable/src/env/node.ts");
const { BACKGROUND_CONTEXT: context } = await load("packages/chord/src/context/index.ts");
const env = new NodeExecutionEnv({ cwd: "/tmp" });
try {
    const window = { maxBytes: 4, maxLines: 1, minIntervalMs: 0, bytesPerSecond: 100000 };
    const scenarios = [];
    for (const input of ["abc", "1234567890abcdef"]) {
        let text = "";
        const skips: unknown[] = [];
        const result = await env.exec(["/usr/bin/printf", "%s", input], { window, onOutput: (chunk: string, _: unknown, info: { skipped?: unknown }) => { text += chunk; if (info.skipped !== undefined) skips.push(info.skipped); } }, context);
        if (!result.ok) throw result.error;
        scenarios.push({ name: input.length > window.maxBytes ? "output exceeding the window" : "output within the window", inputs: { input, window }, expected: { text, skips, exitCode: result.value.exitCode }, normalizations: ["chunk boundaries omitted"] });
    }
    const capturedAt = new Date().toISOString();
    const environment = { platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: (process.report!.getReport() as any).header.glibcVersionRuntime, node: process.version, tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "") };
    const endpoints = ["packages/durable/src/env/node.ts:NodeExecutionEnv.exec", "packages/durable/src/env/index.ts:ShellOutputWindow,ShellOutputSkip"];
    const base = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0", revision, ticket: "1157", family: "audit-1157", capturedAt, environment };
    const artifact = JSON.stringify({ ...base, sourceEndpoints: endpoints, scenarios }, null, 2) + "\n";
    const hash = (value: string | Buffer) => createHash("sha256").update(value).digest("hex");
    writeFileSync(join(dirname(script), "environment.json"), artifact);
    const generator = { path: relative(repo, script), sha256: hash(readFileSync(script)), command: "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1157/capture-1157.mts", sources: [{ path: relative(repo, script), sha256: hash(readFileSync(script)) }] };
    writeFileSync(join(dirname(script), "manifest.json"), JSON.stringify({ ...base, generator, artifacts: [{ path: "environment.json", family: "audit-1157", bytes: Buffer.byteLength(artifact), sha256: hash(artifact), sourceEndpoints: endpoints, scenarios }] }, null, 2) + "\n");
} finally {
    await env.cleanup(context);
}
