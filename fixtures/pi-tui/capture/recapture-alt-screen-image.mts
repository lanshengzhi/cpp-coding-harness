/// One-off: capture the fullscreen image-placement family for issue #1012 and
/// append its manifest records + coupled hashes, leaving every other artifact
/// byte-identical. Mirrors recapture-scroll-view.mts's envelope/manifest shape.
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

const { captureAltScreenImage } = await import(
    pathToFileURL(path.join(root, "capture/alt-screen-image-scenarios.mts")).href
);
const observation = await captureAltScreenImage(piCheckout);
const bundle = path.join(root, selected.bundle);
const manifestPath = path.join(bundle, "manifest.json");
const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));

/// Supplementary capture within the existing bundle session: keep the bundle's
/// manifest capturedAt/environment so every artifact stays consistent (the
/// verifier requires artifact.capturedAt == manifest.capturedAt).
const envelope = (name: string, scenarios: unknown[]) => {
    const bytes = Buffer.from(
        JSON.stringify(
            {
                schema: "pike/tui-evidence/1",
                baseline,
                revision,
                capturedAt: manifest.capturedAt,
                environment: manifest.environment,
                sourceEndpoints: observation.sourceEndpoints,
                scenarios,
            },
            null,
            2,
        ) + "\n",
    );
    writeFileSync(path.join(bundle, name), bytes);
    return bytes;
};

const kittyScenarios = observation.scenarios.filter((scenario: any) => scenario.inputs.capabilities === "kitty");
const iterm2Scenarios = observation.scenarios.filter((scenario: any) => scenario.inputs.capabilities === "iterm2");
if (kittyScenarios.length === 0 || iterm2Scenarios.length === 0) {
    throw new Error("capture must observe both the kitty and the iterm2 fullscreen sessions");
}
const records = [
    { name: "alt-screen-image.json", family: "alt-screen-image", scenarios: kittyScenarios },
    { name: "iterm2-image.json", family: "iterm2-image", scenarios: iterm2Scenarios },
].map(({ name, family, scenarios }) => {
    const bytes = envelope(name, scenarios);
    return {
        path: name,
        family,
        bytes: bytes.length,
        sha256: sha256(bytes),
        sourceEndpoints: observation.sourceEndpoints,
    };
});

for (const record of records) {
    const index = manifest.artifacts.findIndex((entry: any) => entry.path === record.path);
    if (index >= 0) manifest.artifacts[index] = record;
    else manifest.artifacts.push(record);
}
const capturePath = "capture/alt-screen-image-scenarios.mts";
const sourceRecord = { path: capturePath, sha256: sha256(readFileSync(path.join(root, capturePath))) };
const sourceIndex = manifest.generator.sources.findIndex((entry: any) => entry.path === capturePath);
if (sourceIndex >= 0) manifest.generator.sources[sourceIndex] = sourceRecord;
else manifest.generator.sources.push(sourceRecord);
const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + "\n");
writeFileSync(manifestPath, manifestBytes);
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(registryPath, JSON.stringify(registry, null, 2) + "\n");
console.log(
    `captured ${kittyScenarios.length} kitty + ${iterm2Scenarios.length} iterm2 fullscreen image scenarios at ${revision}`,
);
