import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const revision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT is required");
if (execFileSync("git", ["-C", checkout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim() !== revision)
    throw new Error("Wrong frozen revision");
if (execFileSync("git", ["-C", checkout, "status", "--porcelain"], { encoding: "utf8" }).trim())
    throw new Error("Frozen checkout must be clean");
if (process.platform !== "linux" || process.arch !== "x64") throw new Error("Requires Linux x64");
const report = process.report.getReport() as { header: { glibcVersionRuntime?: string } };
if (!report.header.glibcVersionRuntime) throw new Error("Requires glibc");

const { MemoryStorage, Harness, createRegistry, defineExtension, defineTool } =
    await import(join(checkout, "packages/durable/src/index.ts"));
const { createModels, fauxProvider, fauxAssistantMessage, fauxText, fauxToolCall, Type } =
    await import(join(checkout, "packages/ai/src/index.ts"));
const { BACKGROUND_CONTEXT } = await import(join(checkout, "packages/chord/src/context/index.ts"));
const deferred = () => {
    let resolve!: () => void;
    return { promise: new Promise<void>((done) => { resolve = done; }), resolve: () => resolve() };
};
const entriesOf = async (root: any) => [...(await root.entries({}, 1000, undefined, BACKGROUND_CONTEXT)).items].reverse();
const transcript = (entries: any[]) => entries.map((entry) => {
    const message = entry.model?.[0];
    if (entry.kind === "pi.system") return [entry.kind, Object.keys(message?.sections ?? {}), (message?.toolsAdded ?? []).map((tool: any) => tool.name)];
    const text = typeof message?.content === "string" ? message.content : (message?.content ?? []).filter((item: any) => item.type === "text").map((item: any) => item.text).join("|");
    if (entry.kind === "pi.tool-result") return [entry.kind, message.toolName, message.toolCallId, message.isError, message.durationMs !== undefined, text];
    return [entry.kind, text];
});
const entered = deferred();
const release = deferred();
let executions = 0;
let finished = false;
const registry = createRegistry();
registry.install(defineExtension({
    name: "held-tool",
    tools: [defineTool({
        name: "echo", description: "Echo text", parameters: Type.Object({ text: Type.String() }),
        execute: async ({ text }: { text: string }) => {
            executions++;
            entered.resolve();
            await release.promise;
            return { output: [{ type: "text", text: `echo:${text}` }] };
        },
    })],
}));
const faux = fauxProvider();
const models = createModels();
models.setProvider(faux.provider);
const harness = await Harness.open(new MemoryStorage(), { registry, models }, BACKGROUND_CONTEXT);
const root = await harness.root(BACKGROUND_CONTEXT, { agent: { model: { provider: "faux", modelId: "faux-1" } } });
faux.setResponses([
    fauxAssistantMessage([fauxToolCall("echo", { text: "hello" }, { id: "call-1" })], { stopReason: "toolUse" }),
    fauxAssistantMessage([fauxText("finished")]),
]);
const submission = await root.submit({ type: "input", content: "run the echo tool" }, BACKGROUND_CONTEXT);
const settled = submission.wait(BACKGROUND_CONTEXT).then((receipt: any) => { finished = true; return receipt; });
await entered.promise;
const before = {
    settled: finished,
    executions,
    requests: faux.state.callCount,
    entries: transcript(await entriesOf(root)),
};
release.resolve();
const receipt = await settled;
const after = {
    status: receipt.status,
    executions,
    requests: faux.state.callCount,
    entries: transcript(await entriesOf(root)),
};
await harness.close(BACKGROUND_CONTEXT);

const family = "audit-1079";
const output = fileURLToPath(new URL("./", import.meta.url));
mkdirSync(output, { recursive: true });
const common = {
    schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0", revision, ticket: "1079", family,
    capturedAt: new Date().toISOString(),
    environment: { platform: process.platform, arch: process.arch, libc: "glibc",
        glibcVersion: report.header.glibcVersionRuntime, node: process.version,
        tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "") },
};
const sourceEndpoints = ["packages/durable/src/harness/harness.ts:Harness.open,root,close",
    "packages/durable/src/harness/generation.ts:GenerationTask", "packages/durable/src/harness/tool.ts:ToolTask",
    "packages/durable/src/harness/submissions.ts:Submissions"];
const scenarios = [{ name: "a held real tool prevents the next model request and submission settlement",
    inputs: { storage: "MemoryStorage", turns: ["toolUse echo {text:hello}", "stop finished"],
        tool: "echo", hold: "inside execute until caller releases" },
    expected: { before, after }, normalizations: [
        "generated IDs and timestamps are omitted; entry ordering and text remain",
        "elapsed duration is compared by presence, not its wall-clock value",
    ] }];
const artifact = JSON.stringify({ ...common, sourceEndpoints, scenarios }, null, 2) + "\n";
writeFileSync(join(output, "held-tool.json"), artifact);
const hash = (data: string | Buffer) => createHash("sha256").update(data).digest("hex");
const sources = [{
    path: "fixtures/pi-v1.3.0/audit-1079/capture-1079.mts",
    sha256: hash(readFileSync(fileURLToPath(import.meta.url))),
}];
writeFileSync(join(output, "manifest.json"), JSON.stringify({ ...common,
    generator: { path: sources[0].path, sha256: sources[0].sha256, sources,
        command: `PI_CHECKOUT=${checkout} NODE_OPTIONS=--conditions=source TSX_TSCONFIG_PATH=${checkout}/tsconfig.json ${checkout}/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1079/capture-1079.mts` },
    artifacts: [{ path: "held-tool.json", family, bytes: Buffer.byteLength(artifact), sha256: hash(artifact), sourceEndpoints, scenarios }],
}, null, 2) + "\n");
console.log(JSON.stringify({ before, after }, null, 2));
