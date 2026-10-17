// Append artifact 35 (fullscreen exit live measurement).
// All existing 1–34 artifact bytes and prior capture files remain immutable.
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, existsSync } from "node:fs";
import path from "node:path";
import { captureFullscreenExitLive } from "./fullscreen-exit-live-scenarios.mts";

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const piCheckout = process.env.PI_CHECKOUT ?? "/home/lansy/Work/github/coding-agent/pi";
const registryPath = path.join(root, "baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8"));
const baseline = registry.default;
const selected = registry.baselines[baseline];
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: piCheckout }).toString().trim();
if (revision !== selected.revision) throw new Error(`wrong frozen revision: ${revision}`);
if (execFileSync("git", ["status", "--porcelain"], { cwd: piCheckout }).toString().trim()) throw new Error("frozen checkout must be clean");
const bundle = path.join(root, selected.bundle);
const manifestPath = path.join(bundle, "manifest.json");
const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
const sha256 = (data: Buffer) => createHash("sha256").update(data).digest("hex");
const name = "fullscreen-exit-live.json";
if (existsSync(path.join(bundle, name))) throw new Error("supplement already exists; explicit review required before recapture");
if (manifest.artifacts.length !== 34) throw new Error("expected exactly 34 immutable predecessor artifacts");
for (const entry of manifest.artifacts) {
	const bytes = readFileSync(path.join(bundle, entry.path));
	if (bytes.length !== entry.bytes || sha256(bytes) !== entry.sha256) throw new Error(`prior artifact changed: ${entry.path}`);
}
for (const entry of manifest.generator.sources) {
	if (sha256(readFileSync(path.join(root, entry.path))) !== entry.sha256) throw new Error(`prior source changed: ${entry.path}`);
}

const observation = await captureFullscreenExitLive(piCheckout);
console.log(JSON.stringify({ command: "captureFullscreenExitLive", piCheckout, revision, observationRun: observation.observationRun, scenarioCount: observation.scenarios.length }, null, 2));

const artifact = {
	schema: "pike/tui-evidence/1",
	baseline,
	revision,
	capturedAt: manifest.capturedAt,
	environment: manifest.environment,
	observationRun: observation.observationRun,
	sourceEndpoints: observation.sourceEndpoints,
	scenarios: observation.scenarios,
};
const bytes = Buffer.from(JSON.stringify(artifact, null, 2) + "\n");
manifest.artifacts.push({
	path: name,
	family: "fullscreen-exit-live",
	bytes: bytes.length,
	sha256: sha256(bytes),
	sourceEndpoints: observation.sourceEndpoints,
});
for (const source of ["capture/fullscreen-exit-live-scenarios.mts", "capture/recapture-fullscreen-exit-live.mts"]) {
	manifest.generator.sources.push({ path: source, sha256: sha256(readFileSync(path.join(root, source))) });
}
const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + "\n");
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(path.join(bundle, name), bytes);
writeFileSync(manifestPath, manifestBytes);
writeFileSync(registryPath, JSON.stringify(registry, null, 2) + "\n");
console.log(`captured ${observation.scenarios.length} live exit scenarios, 35 artifacts, ${bytes.length} bytes; prior 34 unchanged`);
