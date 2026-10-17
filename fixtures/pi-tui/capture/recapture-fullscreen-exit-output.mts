// Append artifact 33 (fullscreen exit output policy). All existing artifact/source bytes remain immutable.
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, existsSync } from "node:fs";
import path from "node:path";
import { captureFullscreenExitOutput } from "./fullscreen-exit-output-scenarios.mts";

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
const name = "fullscreen-exit-output.json";
if (existsSync(path.join(bundle, name))) throw new Error("supplement already exists; explicit review required before recapture");
if (manifest.artifacts.length !== 32) throw new Error("expected exactly 32 immutable predecessor artifacts");
for (const entry of manifest.artifacts) {
	const bytes = readFileSync(path.join(bundle, entry.path));
	if (bytes.length !== entry.bytes || sha256(bytes) !== entry.sha256) throw new Error(`prior artifact changed: ${entry.path}`);
}
for (const entry of manifest.generator.sources) {
	if (sha256(readFileSync(path.join(root, entry.path))) !== entry.sha256) throw new Error(`prior source changed: ${entry.path}`);
}
const observation = await captureFullscreenExitOutput(piCheckout);
console.log(JSON.stringify({ command: "captureFullscreenExitOutput", piCheckout, revision, observation }, null, 2));
const artifact = {
	schema: "pike/tui-evidence/1",
	baseline,
	revision,
	capturedAt: manifest.capturedAt,
	environment: manifest.environment,
	sourceEndpoints: observation.sourceEndpoints,
	scenarios: observation.scenarios,
};
const bytes = Buffer.from(JSON.stringify(artifact, null, 2) + "\n");
manifest.artifacts.push({
	path: name,
	family: "fullscreen-exit-output",
	bytes: bytes.length,
	sha256: sha256(bytes),
	sourceEndpoints: observation.sourceEndpoints,
});
for (const source of ["capture/fullscreen-exit-output-scenarios.mts", "capture/recapture-fullscreen-exit-output.mts"]) {
	manifest.generator.sources.push({ path: source, sha256: sha256(readFileSync(path.join(root, source))) });
}
const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + "\n");
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(path.join(bundle, name), bytes);
writeFileSync(manifestPath, manifestBytes);
writeFileSync(registryPath, JSON.stringify(registry, null, 2) + "\n");
console.log(`captured ${observation.scenarios.length} exit output scenarios, 33 artifacts, ${bytes.length} bytes; prior 32 unchanged`);
