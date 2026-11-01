/** #1116: real frozen summary requests and transcript preservation after reopen. */
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, relative } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const checkout = process.env.PI_CHECKOUT;
const revision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
if (!checkout || process.env.PI_BASELINE !== "pi-v1.3.0") throw new Error("select PI_CHECKOUT and PI_BASELINE=pi-v1.3.0");
const git = (...args: string[]) => execFileSync("git", args, { cwd: checkout, encoding: "utf8" }).trim();
if (git("rev-parse", "HEAD") !== revision || git("status", "--porcelain", "--untracked-files=no") !== "") {
    throw new Error("capture requires an unmodified frozen pi-v1.3.0 checkout");
}
const report = process.report.getReport() as { header: { glibcVersionRuntime?: string } };
if (process.platform !== "linux" || process.arch !== "x64" || !report.header.glibcVersionRuntime) {
    throw new Error("capture requires Linux x86-64 glibc");
}
const load = (path: string) => import(pathToFileURL(join(checkout, path)).href);
const { createHarness } = await load("packages/coding-agent/test/suite/harness.ts");
const { SessionManager } = await load("packages/coding-agent/src/core/session-manager.ts");
const { fauxAssistantMessage, fauxThinking, fauxText } = await load("packages/ai/src/compat.ts");
const text = (content: any) => typeof content === "string" ? content
    : (content ?? []).filter((b: any) => b.type === "text").map((b: any) => b.text).join("\n");
const thinking = (entries: any[]) => entries.filter((e: any) => e.type === "message")
    .flatMap((e: any) => e.message.content ?? []).filter((b: any) => b.type === "thinking").map((b: any) => b.thinking);

async function scenario(kind: "compaction" | "branch", fail: boolean) {
    const root = mkdtempSync(join(tmpdir(), "pi-1116-"));
    const manager = SessionManager.create(root, root);
    const harness = await createHarness({ sessionManager: manager, initialActiveToolNames: [],
        settings: { compaction: { enabled: false, reserveTokens: 1000, keepRecentTokens: 1 }, retry: { enabled: false } } });
    try {
        harness.setResponses([
            fauxAssistantMessage([fauxThinking("SECRET_THINKING_FIRST"), fauxText("first answer")]),
            fauxAssistantMessage([fauxThinking("SECRET_THINKING_SECOND"), fauxText("second answer")]),
        ]);
        await harness.session.prompt("first request");
        await harness.session.prompt("second request");
        // Seed a stored assistant continuation so the preceding assistant's
        // thinking is in the split-turn prefix, not merely in the kept tail.
        manager.appendMessage(fauxAssistantMessage("tail answer"));
        const file = manager.getSessionFile();
        const before = readFileSync(file, "utf8");
        const entriesBefore = manager.getEntries();
        const target = entriesBefore.find((e: any) => e.type === "message" && e.message.role === "user");
        const leafBefore = manager.getLeafId();
        const requests: any[] = [];
        const response = (answer: string, error = false) => (context: any) => {
            const user = context.messages.find((m: any) => m.role === "user");
            const prompt = text(user?.content);
            const match = prompt.match(/<conversation>\n([\s\S]*?)\n<\/conversation>/)
                ?? prompt.match(/# Conversation\n([\s\S]*?)\n\n# Instructions/);
            requests.push({ roles: context.messages.map((m: any) => m.role),
                conversation: match?.[1], containsThinking: prompt.includes("SECRET_THINKING"), prompt });
            return error ? fauxAssistantMessage("", { stopReason: "error", errorMessage: "summary unavailable" })
                : fauxAssistantMessage(answer);
        };
        harness.setResponses(kind === "compaction"
            ? [response("## Goal\nfrozen history summary"), response("## Original Request\nfrozen turn prefix summary", fail)]
            : [response("frozen branch summary", fail)]);
        let result: any;
        let error: string | undefined;
        try {
            result = kind === "compaction" ? await harness.session.compact("focus on persistence")
                : await harness.session.navigateTree(target.id, { summarize: true });
        } catch (caught) {
            error = String((caught as Error).message);
        }
        const after = readFileSync(file, "utf8");
        const reopened = SessionManager.open(file, root);
        const appended = after.slice(before.length).trim().split("\n").filter(Boolean).map((line) => JSON.parse(line));
        return { name: `${kind} ${fail ? "failure" : "success"} omits request thinking and preserves the reopened transcript`,
            inputs: { kind, fail, prompts: ["first request", "second request"], seededAssistantContinuation: "tail answer",
                failureAtRequest: fail ? (kind === "compaction" ? 2 : 1) : null },
            expected: { requests, error: error ?? null,
                summary: result?.summary ?? result?.summaryEntry?.summary ?? null,
                originalBytesPreserved: after.startsWith(before), originalBytesUnchanged: after === before,
                appendedTypes: appended.map((e: any) => e.type),
                originalThinking: thinking(entriesBefore), reopenedThinking: thinking(reopened.getEntries()),
                leafUnchanged: manager.getLeafId() === leafBefore },
            normalizations: ["host-generated ids, timestamps, paths and system prompt omitted; request prompts retained verbatim"] };
    } finally {
        harness.cleanup();
        rmSync(root, { recursive: true, force: true });
    }
}

const scenarios = [];
for (const kind of ["compaction", "branch"] as const) {
    for (const fail of [false, true]) scenarios.push(await scenario(kind, fail));
}
const environment = { platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: report.header.glibcVersionRuntime,
    node: process.version, tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "") };
const sourceEndpoints = ["packages/coding-agent/src/core/agent-session.ts:compact,navigateTree",
    "packages/coding-agent/src/core/compaction/utils.ts:serializeConversation",
    "packages/coding-agent/src/core/session-manager.ts:open,getEntries",
    "packages/coding-agent/test/suite/harness.ts:createHarness"];
const base = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0", revision, ticket: "1116", family: "audit-1116",
    capturedAt: new Date().toISOString(), environment };
const sha256 = (content: string | Buffer) => createHash("sha256").update(content).digest("hex");
const content = JSON.stringify({ ...base, sourceEndpoints, scenarios }, null, 2) + "\n";
writeFileSync(join(dirname(script), "summary-requests.json"), content);
const source = { path: relative(repo, script), sha256: sha256(readFileSync(script)) };
const manifest = { ...base, generator: { ...source,
    command: "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<frozen-checkout> PI_BASELINE=pi-v1.3.0 <frozen-checkout>/node_modules/.bin/tsx --tsconfig <frozen-checkout>/tsconfig.json fixtures/pi-v1.3.0/audit-1116/capture-1116.mts", sources: [source] },
    artifacts: [{ path: "summary-requests.json", family: "audit-1116", bytes: Buffer.byteLength(content), sha256: sha256(content), sourceEndpoints, scenarios }] };
writeFileSync(join(dirname(script), "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
console.log(JSON.stringify(scenarios.map(({ name, expected }) => ({ name, ...expected, requests: expected.requests.map(({ prompt, ...rest }) => rest) })), null, 2));
