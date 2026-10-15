/// Capture only the fullscreen navigation family and refresh its coupled hashes.
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const piCheckout = process.env.PI_CHECKOUT ?? "/home/lansy/Work/github/coding-agent/pi";
const registryPath = path.join(root, "baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8"));
const baseline = process.env.PI_BASELINE ?? registry.default;
const selected = registry.baselines[baseline];
if (!selected) throw new Error(`unknown pi baseline: ${baseline}`);
const revision = (await import("node:child_process")).execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (revision !== selected.revision) throw new Error(`pi checkout must be at ${selected.revision}, found ${revision}`);
const sha256 = (data: Buffer | string) => createHash("sha256").update(data).digest("hex");
const requirePi = createRequire(path.join(piCheckout, "packages/tui/package.json"));
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
const disabled = ["TERM_PROGRAM", "TERM_PROGRAM_VERSION", "KITTY_WINDOW_ID", "WEZTERM_PANE", "TMUX", "TERMUX_VERSION", "PI_TUI_WRITE_LOG", "PI_HARDWARE_CURSOR", "COLORTERM"];
const environment = {
	platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: glibc,
	node: process.version, TERM: "xterm-256color", disabledVariables: disabled,
	dependencies: { "@xterm/headless": requirePi("@xterm/headless/package.json").version, tsx: requirePi("tsx/package.json").version },
};
const { captureAltScreenNav } = await import(pathToFileURL(path.join(root, "capture/alt-screen-nav-scenarios.mts")).href);
const observation = await captureAltScreenNav(piCheckout);
const bundle = path.join(root, selected.bundle);
const manifestPath = path.join(bundle, "manifest.json");
const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
const artifact = {
	schema: "pike/tui-evidence/1", baseline, revision,
	capturedAt: manifest.capturedAt, environment: manifest.environment,
	sourceEndpoints: observation.sourceEndpoints, scenarios: observation.scenarios,
};
const bytes = Buffer.from(JSON.stringify(artifact, null, 2) + "\n");
writeFileSync(path.join(bundle, "alt-screen-nav.json"), bytes);
const record = {
	path: "alt-screen-nav.json", family: "alt-screen-nav", bytes: bytes.length,
	sha256: sha256(bytes), sourceEndpoints: artifact.sourceEndpoints,
};
const existing = manifest.artifacts.findIndex((entry: any) => entry.path === record.path);
if (existing < 0) manifest.artifacts.push(record); else manifest.artifacts[existing] = record;
const source = manifest.generator.sources.find((entry: any) => entry.path === "capture/alt-screen-nav-scenarios.mts");
const sourceRecord = {
	path: "capture/alt-screen-nav-scenarios.mts",
	sha256: sha256(readFileSync(path.join(root, "capture/alt-screen-nav-scenarios.mts"))),
};
if (source) Object.assign(source, sourceRecord); else manifest.generator.sources.push(sourceRecord);
manifest.generator.sha256 = sha256(readFileSync(path.join(root, "capture/capture-named-tui.mts")));
const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + "\n");
writeFileSync(manifestPath, manifestBytes);
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(registryPath, JSON.stringify(registry, null, 2) + "\n");
console.log(`re-captured alt-screen nav family: ${observation.scenarios.length} scenarios at ${revision}`);
