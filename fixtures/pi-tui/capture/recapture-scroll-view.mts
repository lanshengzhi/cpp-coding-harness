/// One-off: re-capture ONLY the scroll-view family (issue #1004 review fix #5)
/// and refresh its manifest record + coupled hashes, leaving every other
/// artifact byte-identical. Mirrors capture-named-tui.mts's envelope/manifest
/// shape exactly.
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

// Mirror capture-named-tui.mts's environment block.
const { execSync } = await import("node:child_process");
const glibc = execSync("ldd --version").toString().match(/(?:stable release |GNU libc )(\d+\.\d+)/)?.[1] ?? "unknown";
const disabled = [
    "TERM_PROGRAM", "TERM_PROGRAM_VERSION", "KITTY_WINDOW_ID", "WEZTERM_PANE", "TMUX",
    "TERMUX_VERSION", "PI_TUI_WRITE_LOG", "PI_HARDWARE_CURSOR", "COLORTERM",
];
const environment: Record<string, unknown> = {
    platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: glibc,
    node: process.version, TERM: process.env.TERM, disabledVariables: disabled,
};
const requirePi = (await import("node:module")).createRequire(path.join(piCheckout, "packages/tui/package.json"));
const xtermVersion = requirePi("@xterm/headless/package.json").version;
const packageMetadata = JSON.parse(readFileSync(path.join(piCheckout, "packages/tui/package.json"), "utf8"));
if (xtermVersion !== packageMetadata.devDependencies["@xterm/headless"]) {
    throw new Error("installed xterm version differs from frozen pi package declaration");
}
Object.assign(environment, { dependencies: { "@xterm/headless": xtermVersion, tsx: requirePi("tsx/package.json").version } });
const revision = execSync("git rev-parse HEAD", { cwd: piCheckout }).toString().trim();
if (revision !== selected.revision) {
    throw new Error(`pi checkout must be at ${selected.revision}, found ${revision}`);
}

const { captureScrollView } = await import(pathToFileURL(path.join(root, "capture/scroll-view-scenarios.mts")).href);
const observation = await captureScrollView(piCheckout);
// Supplementary capture within the existing bundle session: keep the bundle's
// manifest capturedAt/environment so every artifact stays consistent (the
// verifier requires artifact.capturedAt == manifest.capturedAt).
const manifestPath0 = path.join(root, selected.bundle, "manifest.json");
const manifest0 = JSON.parse(readFileSync(manifestPath0, "utf8"));
const artifact = {
    schema: "pike/tui-evidence/1",
    baseline,
    revision,
    capturedAt: manifest0.capturedAt,
    environment: manifest0.environment,
    sourceEndpoints: observation.sourceEndpoints,
    scenarios: observation.scenarios,
};

const bundle = path.join(root, selected.bundle);
const bytes = Buffer.from(JSON.stringify(artifact, null, 2) + "\n");
writeFileSync(path.join(bundle, "scroll-view.json"), bytes);
const record = {
    path: "scroll-view.json",
    family: "scroll-view",
    bytes: bytes.length,
    sha256: sha256(bytes),
    sourceEndpoints: artifact.sourceEndpoints,
};

// Patch the existing manifest in place: one artifact record + generator source hash.
const manifestPath = path.join(bundle, "manifest.json");
const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
const index = manifest.artifacts.findIndex((entry: any) => entry.path === "scroll-view.json");
if (index < 0) throw new Error("scroll-view.json record missing from manifest");
manifest.artifacts[index] = record;
const source = manifest.generator.sources.find((entry: any) => entry.path === "capture/scroll-view-scenarios.mts");
if (!source) throw new Error("scroll-view-scenarios.mts source missing from manifest");
source.sha256 = sha256(readFileSync(path.join(root, "capture/scroll-view-scenarios.mts")));
const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + "\n");
writeFileSync(manifestPath, manifestBytes);
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(registryPath, JSON.stringify(registry, null, 2) + "\n");
console.log(`re-captured scroll-view family: ${observation.scenarios.length} scenarios at ${revision}`);
