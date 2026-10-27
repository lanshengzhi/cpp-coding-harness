// Capture model-visible structured JSON against a clean frozen pi checkout.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT is required");
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: checkout, encoding: "utf8" }).trim();
if (revision !== "71023aec1546d6d677a65669c5665c0927ce0dd0") throw new Error("wrong baseline revision");
if (execFileSync("git", ["status", "--porcelain"], { cwd: checkout, encoding: "utf8" }).trim()) throw new Error("dirty checkout");
const baseline = "pi-v1.3.0";
const family = "audit-1152";
const registry = JSON.parse(readFileSync(join(repo, "fixtures/pi-v1.3.0/baselines.json"), "utf8"));
if (registry.baselines[baseline].bundles[family]) throw new Error("already registered");
const { toLlmContent } = await import("./pi.mts");
const inputs = [
    ["mixed-keys", '{"b":1,"2":2,"a":3,"1":4}'],
    ["numbers", '{"a":1.0,"b":1e2,"c":-0,"d":1e21,"e":9007199254740993}'],
    ["reverse", '{"z":"last","a":"first","m":"middle"}'],
    ["nested", '{"z":{"b":1,"2":2,"a":3,"1":4},"a":true}'],
    ["strings", '{"z":"line\\nquote\\\" slash\\\\ tab\\t","a":"é"}'],
];
const scenarios = inputs.map(([name, raw]) => ({ name, inputs: { raw }, expected: { text: toLlmContent({ structuredContent: JSON.parse(raw) })[0].text }, normalizations: [] }));
const tsx = join(checkout, "node_modules/.bin/tsx");
const report = process.report.getReport() as { header: { glibcVersionRuntime: string } };
const environment = { platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: report.header.glibcVersionRuntime, node: process.version, tsx: execFileSync(tsx, ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "").trim() };
const capturedAt = new Date().toISOString();
const sourceEndpoints = ["packages/mcp/src/protocol/content.ts:toLlmContent"];
const common = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline, revision, ticket: "1152", family, capturedAt, environment };
const artifact = JSON.stringify({ ...common, sourceEndpoints, scenarios }, null, 2) + "\n";
const directory = dirname(script);
writeFileSync(join(directory, "structured-text.json"), artifact);
const hash = (data: string | Buffer) => createHash("sha256").update(data).digest("hex");
const sources = [script, join(directory, "pi.mts")].map(path => ({ path: relative(repo, path), sha256: hash(readFileSync(path)) }));
const manifest = { ...common, generator: { path: relative(repo, script), sha256: hash(readFileSync(script)), command: "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1152/capture-1152.mts", sources }, artifacts: [{ path: "structured-text.json", family, bytes: Buffer.byteLength(artifact), sha256: hash(artifact), sourceEndpoints, scenarios }] };
writeFileSync(join(directory, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
console.log(`captured ${scenarios.length} scenarios`);
