/** #1105: observe real frozen AgentSession resource isolation and late tool binding. */
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const checkout = process.env.PI_CHECKOUT;
const revision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
if (!checkout) throw new Error("PI_CHECKOUT must name the frozen checkout");
const git = (...args: string[]) => execFileSync("git", args, { cwd: checkout, encoding: "utf8" }).trim();
if (git("rev-parse", "HEAD") !== revision || git("status", "--porcelain") !== "") {
    throw new Error("capture needs a clean frozen pi-v1.3.0 checkout");
}
if (process.env.PI_BASELINE !== "pi-v1.3.0") throw new Error("PI_BASELINE must be pi-v1.3.0");
const report = process.report.getReport() as { header: { glibcVersionRuntime?: string } };
if (process.platform !== "linux" || process.arch !== "x64" || !report.header.glibcVersionRuntime) {
    throw new Error("capture needs Linux x86-64 glibc");
}
const load = (path: string) => import(pathToFileURL(join(checkout, path)).href);
const { createHarness, createTestUiContext, getMessageText } = await load("packages/coding-agent/test/suite/harness.ts");
const { createMcpExtension } = await load("packages/coding-agent/src/extensions/mcp/index.ts");
const { createInMemoryTransportPair } = await load("packages/mcp/src/testing/index.ts");
const { LATEST_PROTOCOL_VERSION } = await load("packages/mcp/src/index.ts");
const { fauxAssistantMessage, fauxToolCall } = await load("packages/ai/src/index.ts");

async function waitFor(predicate: () => boolean) {
    const deadline = Date.now() + 2000;
    while (!predicate()) {
        if (Date.now() > deadline) throw new Error("capture wait timed out");
        await new Promise((resolve) => setTimeout(resolve, 5));
    }
}

async function scenario(fail: boolean, binding = false) {
    const methods: string[] = [];
    const pair = createInMemoryTransportPair();
    let tools = ["echo"];
    pair.server.onMessage((message: any) => {
        if (!message.method || message.id === undefined) return;
        methods.push(message.method);
        let result: unknown = {};
        if (message.method === "initialize") {
            result = { protocolVersion: LATEST_PROTOCOL_VERSION, capabilities: { tools: {}, resources: {} },
                serverInfo: { name: "echo", version: "1" } };
        } else if (message.method === "tools/list") {
            const field = tools.length === 1 ? "text" : "message";
            const inputSchema = binding ? { type: "object", additionalProperties: false, required: [field],
                properties: { [field]: { type: "string" } } } : { type: "object" };
            result = { tools: tools.map((name) => ({ name, inputSchema,
                ...(binding ? { description: tools.length === 1 ? "original" : "updated" } : {}) })) };
        } else if (message.method === "resources/list") {
            if (fail) {
                void pair.server.send({ jsonrpc: "2.0", id: message.id, error: { code: -32603, message: "resource boom" } });
                return;
            }
            result = { resources: [{ uri: "docs://intro", name: "intro" }] };
        } else if (message.method === "tools/call") {
            const text = binding ? `${message.params.name}:${message.params.arguments.message ?? message.params.arguments.text}`
                : `${message.params.name} after resources`;
            result = { content: [{ type: "text", text }] };
        }
        void pair.server.send({ jsonrpc: "2.0", id: message.id, result });
    });
    await pair.server.start();
    const harness = await createHarness({ initialActiveToolNames: [], extensionFactories: [createMcpExtension({
        loadConfig: () => ({ servers: [{ name: "echo", config: { url: "http://unused.invalid", exposure: "direct" }, source: "test" }], errors: [] }),
        createTransport: () => pair.client,
    })] });
    try {
        await harness.session.bindExtensions({ uiContext: createTestUiContext() });
        await waitFor(() => harness.session.getActiveToolNames().includes("mcp__echo__echo"));
        const startupMethods = [...methods];
        const call = (name: string, args = {}) => fauxAssistantMessage([fauxToolCall(name, args)], { stopReason: "toolUse" });
        harness.setResponses(binding ? [call("mcp__echo__echo", { text: "first" }), fauxAssistantMessage("done")]
            : [call("list_mcp_resources", { server: "echo" }), call("mcp__echo__echo"),
                call("mcp__echo__echo"), fauxAssistantMessage("done")]);
        await harness.session.prompt("list resources then echo twice");
        const resource = harness.session.messages.find((m: any) => m.role === "toolResult" && m.toolName === "list_mcp_resources");
        const echoes = harness.session.messages.filter((m: any) => m.role === "toolResult" && m.toolName === "mcp__echo__echo");
        tools = ["late", "echo"];
        await pair.server.send({ jsonrpc: "2.0", method: "notifications/tools/list_changed" });
        await waitFor(() => harness.session.getActiveToolNames().includes("mcp__echo__late"));
        const callableAfterChange = harness.session.getCallableToolNames().filter((name: string) => name.startsWith("mcp__"));
        harness.setResponses([call("mcp__echo__late", binding ? { message: "bound" } : {}), fauxAssistantMessage("done")]);
        await harness.session.prompt("call late tool");
        const late = harness.session.messages.find((m: any) => m.role === "toolResult" && m.toolName === "mcp__echo__late");
        if (binding) {
            harness.setResponses([call("mcp__echo__echo", { message: "second" }), fauxAssistantMessage("done")]);
            await harness.session.prompt("call updated echo");
            const updated = harness.session.messages.findLast((m: any) => m.role === "toolResult" && m.toolName === "mcp__echo__echo");
            if (late.isError || updated.isError) throw new Error("updated Session binding was not callable");
            return { name: "real Session replaces MCP callable schemas after a tool-list change",
                inputs: { exposure: "direct", initialRequiredArgument: "text", updatedRequiredArgument: "message",
                    additionalProperties: false, updatedServerOrder: ["late", "echo"] },
                expected: { startupMethods, firstEchoText: getMessageText(echoes[0]), callableAfterChange,
                    lateText: getMessageText(late), updatedEchoText: getMessageText(updated),
                    descriptionsAfterChange: callableAfterChange.map((name: string) =>
                        harness.session.getAllTools().find((tool: any) => tool.name === name)?.description), methods },
                normalizations: ["notifications/initialized omitted from request-only method trace", "non-MCP callable tools omitted"] };
        }
        return { name: fail ? "real Session calls tools after resources/list fails and binds a late tool" : "real Session reads resources on demand and binds a late tool",
            inputs: { exposure: "direct", resourcesList: fail ? "JSON-RPC error" : "success" },
            expected: { startupMethods, resourceText: getMessageText(resource), resourceIsError: resource.isError,
                echoes: echoes.map(getMessageText), callableAfterChange, lateText: getMessageText(late), methods },
            normalizations: ["notifications/initialized omitted from request-only method trace", "non-MCP callable tools omitted"] };
    } finally {
        harness.cleanup();
        await pair.server.close();
    }
}

const scenarios = [await scenario(false), await scenario(true), await scenario(false, true)];
const environment = { platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: report.header.glibcVersionRuntime,
    node: process.version, tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "") };
const sourceEndpoints = ["packages/coding-agent/src/core/agent-session.ts:prompt,getCallableToolNames",
    "packages/coding-agent/src/extensions/mcp/runtime.ts:connectOnce,refreshTools",
    "packages/coding-agent/src/extensions/mcp/index.ts:registerTools,syncResourceTools",
    "packages/coding-agent/test/suite/harness.ts:createHarness"];
const base = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0", revision, ticket: "1105", family: "audit-1105",
    capturedAt: new Date().toISOString(), environment };
const sha256 = (content: string | Buffer) => createHash("sha256").update(content).digest("hex");
const content = JSON.stringify({ ...base, sourceEndpoints, scenarios }, null, 2) + "\n";
writeFileSync(join(dirname(script), "session-binding.json"), content);
const source = { path: relative(repo, script), sha256: sha256(readFileSync(script)) };
const manifest = { ...base, generator: { ...source, command: "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1105/capture-1105.mts", sources: [source] },
    artifacts: [{ path: "session-binding.json", family: "audit-1105", bytes: Buffer.byteLength(content), sha256: sha256(content), sourceEndpoints, scenarios }] };
writeFileSync(join(dirname(script), "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
console.log(JSON.stringify(scenarios, null, 2));
