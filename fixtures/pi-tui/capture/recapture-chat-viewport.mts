/// One-off: capture the product chat-viewport family (#1036, spec #946) and
/// append its manifest records + coupled hashes, leaving every other artifact
/// byte-identical. Mirrors recapture-cli-tui-mode.mts's envelope/manifest shape.
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

const { captureChatViewport } = await import(
	pathToFileURL(path.join(root, "capture/chat-viewport-scenarios.mts")).href
);
const observation = await captureChatViewport(piCheckout);
const bundle = path.join(root, selected.bundle);
const manifestPath = path.join(bundle, "manifest.json");
const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));

/// JSON that stays readable while the per-cell style grids sit on one line:
/// they are over half of the bytes and are compared by the replay, not read
/// by eye. Everything else keeps the bundle's two-space shape.
function serialize(value: unknown, indent = ""): string {
	const child = `${indent}  `;
	if (Array.isArray(value)) {
		if (value.length === 0) return "[]";
		return `[\n${value.map((entry) => `${child}${serialize(entry, child)}`).join(",\n")}\n${indent}]`;
	}
	if (value !== null && typeof value === "object") {
		const entries = Object.entries(value as Record<string, unknown>);
		if (entries.length === 0) return "{}";
		const body = entries.map(
			([key, entry]) => `${child}${JSON.stringify(key)}: ${key === "cells" ? JSON.stringify(entry) : serialize(entry, child)}`,
		);
		return `{\n${body.join(",\n")}\n${indent}}`;
	}
	return JSON.stringify(value);
}

/// Supplementary capture within the existing bundle session: keep the bundle's
/// manifest capturedAt/environment so every artifact stays consistent (the
/// verifier requires artifact.capturedAt == manifest.capturedAt).
const artifact = {
	schema: "pike/tui-evidence/1",
	baseline,
	revision,
	capturedAt: manifest.capturedAt,
	environment: manifest.environment,
	sourceEndpoints: observation.sourceEndpoints,
	scenarios: observation.scenarios,
};
const bytes = Buffer.from(`${serialize(artifact)}\n`);
// The compact cell serialization must not change the parsed evidence.
if (JSON.stringify(JSON.parse(bytes.toString("utf8"))) !== JSON.stringify(artifact)) {
	throw new Error("compact evidence serialization changed the artifact payload");
}
const fileName = "chat-viewport.json";
writeFileSync(path.join(bundle, fileName), bytes);
const record = {
	path: fileName,
	family: "chat-viewport",
	bytes: bytes.length,
	sha256: sha256(bytes),
	sourceEndpoints: observation.sourceEndpoints,
};
const index = manifest.artifacts.findIndex((entry: any) => entry.path === record.path);
if (index >= 0) manifest.artifacts[index] = record;
else manifest.artifacts.push(record);
const capturePath = "capture/chat-viewport-scenarios.mts";
const sourceRecord = { path: capturePath, sha256: sha256(readFileSync(path.join(root, capturePath))) };
const sourceIndex = manifest.generator.sources.findIndex((entry: any) => entry.path === capturePath);
if (sourceIndex >= 0) manifest.generator.sources[sourceIndex] = sourceRecord;
else manifest.generator.sources.push(sourceRecord);
manifest.generator.sha256 = sha256(readFileSync(path.join(root, manifest.generator.path)));
for (const entry of manifest.artifacts) {
	if (entry.path === fileName) continue;
	const payload = readFileSync(path.join(bundle, entry.path));
	if (payload.length !== entry.bytes || sha256(payload) !== entry.sha256) {
		throw new Error(`append recipe touched another artifact: ${entry.path}`);
	}
}
const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + "\n");
writeFileSync(manifestPath, manifestBytes);
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(registryPath, JSON.stringify(registry, null, 2) + "\n");
console.log(
	`captured ${observation.scenarios.length} chat-viewport scenarios at ${revision}; ${manifest.artifacts.length} artifacts, ${bytes.length} bytes`,
);
