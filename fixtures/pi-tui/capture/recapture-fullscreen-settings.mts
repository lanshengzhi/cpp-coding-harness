// Append artifact 36 (fullscreen settings family, #1020). All existing artifact/source bytes remain immutable.
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, existsSync } from "node:fs";
import path from "node:path";

// Identity: the frozen capture removes every ambient PI_* variable and pins PI_PACKAGE_DIR
// before its imports run; the requested checkout is read before that handoff.
const requestedCheckout = process.env.PI_CHECKOUT;
const piCheckout = requestedCheckout ?? "/home/lansy/Work/github/coding-agent/pi";

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
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
const name = "fullscreen-settings.json";
if (existsSync(path.join(bundle, name))) throw new Error("supplement already exists; explicit review required before recapture");
if (manifest.artifacts.length !== 35) throw new Error("expected exactly 35 immutable predecessor artifacts");
const verifyPredecessors = (label: string) => {
	for (const entry of manifest.artifacts) {
		const bytes = readFileSync(path.join(bundle, entry.path));
		if (bytes.length !== entry.bytes || sha256(bytes) !== entry.sha256) throw new Error(`${label}: prior artifact changed: ${entry.path}`);
	}
	for (const entry of manifest.generator.sources) {
		if (sha256(readFileSync(path.join(root, entry.path))) !== entry.sha256) throw new Error(`${label}: prior source changed: ${entry.path}`);
	}
	console.log(`predecessors verified ${label}: ${manifest.artifacts.length} artifacts, ${manifest.generator.sources.length} sources`);
};
verifyPredecessors("before capture");

const { captureFullscreenSettings } = await import("./fullscreen-settings-scenarios.mts");
const observation = await captureFullscreenSettings(piCheckout);
if (observation.identity.frozenVersion !== "1.0.4") throw new Error("frozen VERSION is not 1.0.4");
if (observation.identity.piPackageDir !== path.join(piCheckout, "packages/coding-agent")) throw new Error("frozen PI_PACKAGE_DIR was not pinned");
if (process.env.PI_PACKAGE_DIR !== path.join(piCheckout, "packages/coding-agent")) throw new Error("PI_PACKAGE_DIR pin was not applied");
if (observation.identity.foreignResolutions.length > 0) throw new Error("foreign module resolution detected");
verifyPredecessors("after capture");

const artifact = {
	schema: "pike/tui-evidence/1",
	baseline,
	revision,
	capturedAt: manifest.capturedAt,
	environment: manifest.environment,
	observationRun: {
		timestamp: new Date().toISOString(),
		node: process.version,
		platform: process.platform,
		arch: process.arch,
		caller: "recapture-fullscreen-settings.mts",
		scenarioCount: observation.scenarios.length,
		envelopeCapturedAtIsHistorical: true,
		measurementType:
			"in-memory @xterm/headless VirtualTerminal, frozen ScrollView/layout frames and controlled InteractiveMode.prototype contexts; not OS-level PTY proof",
		clipboardInjection: "copySelection sink injected into the frozen fullscreen consumer; the host clipboard is never written",
		identity: observation.identity,
	},
	sourceEndpoints: observation.sourceEndpoints,
	scenarios: observation.scenarios,
};
const bytes = Buffer.from(`${JSON.stringify(artifact, null, 2)}\n`);
manifest.artifacts.push({
	path: name,
	family: "fullscreen-settings",
	bytes: bytes.length,
	sha256: sha256(bytes),
	sourceEndpoints: observation.sourceEndpoints,
});
for (const source of ["capture/fullscreen-settings-scenarios.mts", "capture/recapture-fullscreen-settings.mts"]) {
	manifest.generator.sources.push({ path: source, sha256: sha256(readFileSync(path.join(root, source))) });
}
const manifestBytes = Buffer.from(`${JSON.stringify(manifest, null, 2)}\n`);
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(path.join(bundle, name), bytes);
writeFileSync(manifestPath, manifestBytes);
writeFileSync(registryPath, `${JSON.stringify(registry, null, 2)}\n`);
verifyPredecessors("after write");
if (sha256(readFileSync(path.join(bundle, name))) !== manifest.artifacts[manifest.artifacts.length - 1].sha256) throw new Error("new artifact digest mismatch");
console.log(`captured ${observation.scenarios.length} fullscreen-settings scenarios, ${manifest.artifacts.length} artifacts, ${bytes.length} bytes; prior 35 unchanged`);
