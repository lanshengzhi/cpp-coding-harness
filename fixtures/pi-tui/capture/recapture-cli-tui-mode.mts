/// One-off: capture the CLI TUI-mode family for issue #1017 and append its
/// manifest records + coupled hashes, leaving every other artifact
/// byte-identical. Mirrors recapture-mouse-components.mts's envelope/manifest
/// shape.
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const piCheckout = process.env.PI_CHECKOUT ?? "/home/lansy/Work/github/coding-agent/pi";
const registryPath = path.join(root, "baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8"));
const baseline = process.env.PI_BASELINE ?? registry.default;
const selected = registry.baselines[baseline];

const sha256 = (data: Buffer | string) => createHash("sha256").update(data).digest("hex");

const { execSync } = await import("node:child_process");
const revision = execSync("git rev-parse HEAD", { cwd: piCheckout }).toString().trim();
if (revision !== selected.revision) {
	throw new Error(`pi checkout must be at ${selected.revision}, found ${revision}`);
}
const dirty = execSync("git status --porcelain", { cwd: piCheckout }).toString().trim();
if (dirty) throw new Error(`pi checkout must be clean, found:\n${dirty}`);

const { captureCliTuiMode } = await import(
	pathToFileURL(path.join(root, "capture/cli-tui-mode-scenarios.mts")).href
);
const observation = await captureCliTuiMode(piCheckout);
const bundle = path.join(root, selected.bundle);
const manifestPath = path.join(bundle, "manifest.json");
const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));

/// Supplementary capture within the existing bundle session: keep the bundle's
/// manifest capturedAt/environment so every artifact stays consistent (the
/// verifier requires artifact.capturedAt == manifest.capturedAt).
const bytes = Buffer.from(
	JSON.stringify(
		{
			schema: "pike/tui-evidence/1",
			baseline,
			revision,
			capturedAt: manifest.capturedAt,
			environment: manifest.environment,
			sourceEndpoints: observation.sourceEndpoints,
			scenarios: observation.scenarios,
		},
		null,
		2,
	) + "\n",
);
const fileName = "cli-tui-mode.json";
writeFileSync(path.join(bundle, fileName), bytes);
const record = {
	path: fileName,
	family: "cli-tui-mode",
	bytes: bytes.length,
	sha256: sha256(bytes),
	sourceEndpoints: observation.sourceEndpoints,
};
const index = manifest.artifacts.findIndex((entry: any) => entry.path === record.path);
if (index >= 0) manifest.artifacts[index] = record;
else manifest.artifacts.push(record);
const capturePath = "capture/cli-tui-mode-scenarios.mts";
const sourceRecord = { path: capturePath, sha256: sha256(readFileSync(path.join(root, capturePath))) };
const sourceIndex = manifest.generator.sources.findIndex((entry: any) => entry.path === capturePath);
if (sourceIndex >= 0) manifest.generator.sources[sourceIndex] = sourceRecord;
else manifest.generator.sources.push(sourceRecord);
manifest.generator.sha256 = sha256(readFileSync(path.join(root, manifest.generator.path)));
const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + "\n");
writeFileSync(manifestPath, manifestBytes);
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(registryPath, JSON.stringify(registry, null, 2) + "\n");
console.log(`captured ${observation.scenarios.length} CLI TUI-mode scenarios at ${revision}`);
