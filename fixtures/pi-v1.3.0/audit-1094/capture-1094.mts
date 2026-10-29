#!/usr/bin/env tsx
/** Frozen public Harness observations for #1094; no live provider or plugins. */
import { execFileSync, spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { appendFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";

const script = fileURLToPath(import.meta.url);
const directory = path.dirname(script);
const repository = path.resolve(directory, "../../..");
const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT is required");
if (!process.env.TSX_TSCONFIG_PATH) {
    const run = spawnSync(path.join(checkout, "node_modules/.bin/tsx"), [script], {
        stdio: "inherit", env: { ...process.env, TSX_TSCONFIG_PATH: path.join(checkout, "tsconfig.json"),
            NODE_OPTIONS: "--conditions=source" },
    });
    if (run.error) throw run.error;
    process.exit(run.status ?? 1);
}
const registry = JSON.parse(readFileSync(path.join(directory, "../baselines.json"), "utf8"));
const baseline = process.env.PI_BASELINE ?? registry.default;
const family = "audit-1094";
for (const [name, value] of Object.entries<any>(registry.baselines))
    if (name !== baseline && value.bundles?.[family]) throw new Error("The family belongs to another baseline");
const revision = registry.baselines[baseline]?.revision;
if (!revision || execFileSync("git", ["-C", checkout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim() !== revision)
    throw new Error("The checkout must match the named baseline");
if (execFileSync("git", ["-C", checkout, "status", "--porcelain"], { encoding: "utf8" }).trim())
    throw new Error("A clean frozen checkout is required");
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) throw new Error("Linux x86-64 glibc is required");
const pi = await import("../audit-1056/pi.mts");
const { openChat, closeChat, entriesOf, answer, toolCall, toolResults } = await import("../audit-1056/support.mts");
const { BACKGROUND_CONTEXT: context, MemoryStorage, CodemodeSandbox, CodemodeStoreDoc,
    createCodemodeExtension, defineExtension, defineTool, Type } = pi;
const png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a8kUAAAAASUVORK5CYII=";
const normalize = (result: any) => ({ isError: result.isError,
    content: result.content.map((block: any) => block.type === "text"
        ? { ...block, text: block.text.replace(/Wall time [\d.]+ seconds/, "Wall time <seconds>") } : block),
    calls: result.details.calls.map((call: any) => ({ id: call.id, name: call.name, args: call.args,
        status: call.status, durationPresent: typeof call.durationMs === "number" })),
});
const scenarios: any[] = [];
const outputSource = `text("alpha"); image("data:image/png;base64,${png}"); console.log("console"); return "omega";`;
{
    const chat = await openChat(new MemoryStorage());
    chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
    chat.faux.setResponses([toolCall("codemode", { code: outputSource }), answer("done")]);
    await (await chat.root.submit({ type: "input", content: "output" }, context)).wait(context);
    scenarios.push({ name: "durable codemode returns ordered text image console and value output",
        inputs: { source: outputSource }, expected: normalize(toolResults(await entriesOf(chat.root))[0]),
        normalizations: ["wall time replaced by <seconds>; nested durations recorded as presence"] });
    await closeChat(chat);
}
async function restartWorker(database: string, first: boolean, replaySafe: boolean) {
    const effectsPath = database + ".effects";
    const chat = await openChat(await pi.openNodeSqliteStorage(database));
    chat.registry.install(defineExtension({ name: "restart-tools", tools: [
        defineTool({ name: "effect", description: "Record an effect", parameters: Type.Object({ stamp: Type.String() }),
            execute: async ({ stamp }: any) => { appendFileSync(effectsPath, stamp + "\n");
                return { output: [{ type: "text", text: "effect done" }] }; } }),
        defineTool({ name: "pause", description: "Wait", replay: replaySafe ? "safe" : "never", parameters: Type.Object({}),
            execute: async () => {
                if (first) { process.stdout.write("ready\n"); await new Promise<void>(() => {}); }
                return { output: [{ type: "text", text: "resumed" }] };
            } }),
    ] }));
    chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
    const source = "const stamp = Date.now() + ':' + Math.random(); await tools.effect({stamp}); await tools.pause(); store('stamp', stamp); return stamp;";
    chat.faux.setResponses(first ? [toolCall("codemode", { code: source }), answer("unused")] : [answer("done")]);
    if (first) await (await chat.root.submit({ type: "input", content: "restart" }, context)).wait(context);
    else { chat.harness.resume(); await chat.root.waitForIdle(context); }
    const result = toolResults(await entriesOf(chat.root))[0];
    const store = await chat.harness.snapshot(CodemodeStoreDoc, chat.root.id, context);
    const effects = readFileSync(effectsPath, "utf8").trim().split("\n");
    const expected = { effectCalls: effects.length, isError: result.isError,
        outputMatchesOriginalStamp: result.content.some((item: any) => item.text?.endsWith(effects[0])),
        storeMatchesOriginalStamp: store.values.stamp === effects[0],
        statuses: result.details.calls.map((call: any) => call.status) };
    await closeChat(chat);
    return expected;
}
if (process.env.CCH_1094_WORKER) {
    const result = await restartWorker(process.env.CCH_1094_DATABASE!, process.env.CCH_1094_WORKER === "first",
        process.env.CCH_1094_SAFE === "1");
    console.log(JSON.stringify(result));
    process.exit(0);
}
for (const replaySafe of [true, false]) {
    const temporary = mkdtempSync(path.join(tmpdir(), "cch-1094-frozen-"));
    const database = path.join(temporary, "store.sqlite");
    const child = spawn(path.join(checkout, "node_modules/.bin/tsx"), [script], {
        env: { ...process.env, CCH_1094_WORKER: "first", CCH_1094_DATABASE: database, CCH_1094_SAFE: replaySafe ? "1" : "0" },
        stdio: ["ignore", "pipe", "inherit"], detached: true,
    });
    const exited = new Promise<void>(resolve => child.once("exit", () => resolve()));
    try {
        await new Promise<void>((resolve, reject) => {
            const timeout = setTimeout(() => reject(new Error("Restart worker did not reach pause")), 15000);
            child.stdout!.once("data", () => { clearTimeout(timeout); resolve(); });
            child.once("error", error => { clearTimeout(timeout); reject(error); });
            child.once("exit", code => { clearTimeout(timeout); if (code !== null) reject(new Error("Worker exited before pause: " + code)); });
        });
    } finally {
        if (child.pid) {
            try { process.kill(-child.pid, "SIGKILL"); }
            catch (error: any) { if (error.code !== "ESRCH") throw error; }
            await exited;
        }
    }
    const resumed = spawnSync(path.join(checkout, "node_modules/.bin/tsx"), [script], {
        env: { ...process.env, CCH_1094_WORKER: "resume", CCH_1094_DATABASE: database, CCH_1094_SAFE: replaySafe ? "1" : "0" },
        encoding: "utf8", timeout: 15000,
    });
    if (resumed.status !== 0) throw new Error(resumed.stderr || String(resumed.error));
    scenarios.push({ name: replaySafe ? "durable codemode reopens with the same clock seed and recorded nested answer" :
            "durable codemode refuses to repeat an unsafe interrupted nested call",
        inputs: { source: "record clock and random; effect; pause; store and return stamp",
            replay: replaySafe ? "safe" : "never",
            interruptedAt: "SIGKILL after pause entered and effect answer committed" },
        expected: JSON.parse(resumed.stdout),
        normalizations: ["clock and random stamp compared to the first admitted effect argument, not emitted"] });
    rmSync(temporary, { recursive: true, force: true });
}
const capturedAt = new Date().toISOString();
const requirePi = createRequire(path.join(checkout, "package.json"));
const environment = { platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: glibc,
    node: process.version, tsx: requirePi("tsx/package.json").version };
const sourceEndpoints = ["packages/durable/src/codemode/codemode.ts:CodemodeCall,CodemodeStoreDoc",
    "packages/durable/src/codemode/output.ts:formatResult", "packages/durable/test/codemode.test.ts:code mode"];
const base = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline, revision, ticket: "1094", family, capturedAt, environment };
const artifact = Buffer.from(JSON.stringify({ ...base, sourceEndpoints, scenarios }, null, 2) + "\n");
const hash = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const generatorPath = path.relative(repository, script);
const sources = [generatorPath, "fixtures/pi-v1.3.0/audit-1056/pi.mts", "fixtures/pi-v1.3.0/audit-1056/support.mts"]
    .map(file => ({ path: file, sha256: hash(readFileSync(path.join(repository, file))) }));
const manifest = { ...base, generator: { path: generatorPath, sha256: sources[0].sha256,
    command: "PI_CHECKOUT=<frozen-checkout> PI_BASELINE=pi-v1.3.0 <frozen-checkout>/node_modules/.bin/tsx " + generatorPath, sources },
    artifacts: [{ path: "codemode.json", family, bytes: artifact.length, sha256: hash(artifact), sourceEndpoints, scenarios }] };
writeFileSync(path.join(directory, "codemode.json"), artifact);
writeFileSync(path.join(directory, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
console.log(`Captured ${scenarios.length} frozen Harness scenarios`);
