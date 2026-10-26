import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const repository = join(dirname(scriptPath), "../../..");
const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT is required");
const baseline = "pi-v1.3.0";
const family = "audit-1072";
const registry = JSON.parse(readFileSync(join(repository, "fixtures/pi-v1.3.0/baselines.json"), "utf8"));
const revision = registry.baselines[baseline].revision;
const git = (...args: string[]) => execFileSync("git", ["-C", checkout, ...args], { encoding: "utf8" }).trim();
if (revision !== "71023aec1546d6d677a65669c5665c0927ce0dd0" || git("rev-parse", "HEAD") !== revision)
    throw new Error("frozen revision mismatch");
if (git("status", "--porcelain")) throw new Error("frozen checkout must be clean");
const report = process.report.getReport() as { header: { glibcVersionRuntime?: string } };
if (process.platform !== "linux" || process.arch !== "x64" || !report.header.glibcVersionRuntime)
    throw new Error("Linux x64 glibc is required");
const tsx = join(checkout, "node_modules/.bin/tsx");
const environment = {
    platform: process.platform, arch: process.arch, libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime, node: process.version,
    tsx: execFileSync(tsx, ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "").trim(),
};
const { SessionManager } = await import(join(checkout, "packages/coding-agent/src/core/session-manager.ts"));
const { serializeSessionBranch } = await import(join(checkout, "packages/coding-agent/src/core/session-export.ts"));
const { exportSessionToHtml } = await import(join(checkout, "packages/coding-agent/src/core/export-html/index.ts"));
const temporary = mkdtempSync(join(tmpdir(), "pi-1072-"));
const manager = SessionManager.create(temporary, temporary);
manager.appendMessage({ role: "user", content: "first branch", timestamp: 1 });
const first = manager.appendMessage({ role: "assistant", content: [{ type: "text", text: "first answer" }],
    api: "openai-responses", provider: "fake", model: "fake", stopReason: "stop", timestamp: 2,
    usage: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0,
        cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } } });
manager.appendMessage({ role: "user", content: "later branch", timestamp: 3 });
manager.branch(first);
const branch = serializeSessionBranch(manager).trim().split("\n").map((line: string) => JSON.parse(line));
const htmlPath = join(temporary, "export.html");
await exportSessionToHtml(manager, undefined, { outputPath: htmlPath, themeName: "dark" });
const html = readFileSync(htmlPath, "utf8");
const payload = html.match(/<script id="session-data" type="application\/json">\s*([^<]+)<\/script>/);
if (!payload) throw new Error("HTML payload missing");
const htmlData = JSON.parse(Buffer.from(payload[1].trim(), "base64").toString("utf8"));
const branchObservation = {
    jsonlMessages: branch.slice(1).map((entry: any) => entry.message.content),
    htmlMessages: htmlData.entries.map((entry: any) => entry.message.content),
    htmlLeafIsSelected: htmlData.leafId === first,
};
const renamed = manager.appendSessionInfo("  Live\r\nSession  ");
const info = manager.getEntry(renamed);
const scenarios = [
    {
        name: "export uses the live selected branch while HTML retains all entries",
        inputs: { messages: ["first branch", "first answer", "later branch"], selected: "first answer" },
        expected: branchObservation,
        normalizations: ["Entry identifiers compared by relationship; wall-clock timestamps and generated header identity omitted."],
    },
    {
        name: "live session info append advances the leaf under the selected parent",
        inputs: { name: "  Live\r\nSession  ", selected: "first answer" },
        expected: { name: manager.getSessionName(), parentIsSelected: info.parentId === first,
            leafIsRename: manager.getLeafId() === renamed, entryCount: manager.getEntries().length },
        normalizations: ["Generated identifiers compared by relationship; timestamps omitted."],
    },
];
rmSync(temporary, { recursive: true, force: true });
const sourceEndpoints = [
    "packages/coding-agent/src/core/session-manager.ts:SessionManager.branch,appendSessionInfo,getEntries,getLeafId",
    "packages/coding-agent/src/core/session-export.ts:serializeSessionBranch",
    "packages/coding-agent/src/core/export-html/index.ts:exportSessionToHtml",
];
const capturedAt = new Date().toISOString();
const schema = "pike/pi-v1.3.0-audit-evidence/1";
const artifact = { schema, baseline, revision, capturedAt, environment, sourceEndpoints, scenarios };
const bytes = `${JSON.stringify(artifact, null, 2)}\n`;
const sha256 = (value: Buffer | string) => createHash("sha256").update(value).digest("hex");
const relativeScript = "docs/research/pi-v1.3.0/capture-1072-live-session.mts";
const digest = sha256(readFileSync(scriptPath));
const artifactPath = "live-session.json";
const manifest = {
    schema, baseline, revision, ticket: "1072", family, capturedAt, environment,
    generator: { path: relativeScript, sha256: digest,
        command: `PI_CHECKOUT=${checkout} NODE_OPTIONS=--conditions=source TSX_TSCONFIG_PATH=${join(checkout, "tsconfig.json")} ${tsx} ${relativeScript}`,
        sources: [{ path: relativeScript, sha256: digest }] },
    artifacts: [{ path: artifactPath, family, bytes: Buffer.byteLength(bytes), sha256: sha256(bytes), sourceEndpoints, scenarios }],
};
const bundle = join(repository, "fixtures/pi-v1.3.0", family);
mkdirSync(bundle, { recursive: true });
writeFileSync(join(bundle, artifactPath), bytes);
writeFileSync(join(bundle, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(JSON.stringify(scenarios, null, 2));
