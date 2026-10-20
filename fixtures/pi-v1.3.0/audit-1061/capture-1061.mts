/**
 * `audit-1061` capture: frozen Chord observations that audit-1057 recorded as
 * gaps — `Tracker.prepareReplace`, the draft array-hole rejection, and the
 * review-repair distinctions for mutable apply, draft reads, adoption
 * revocation and operation folding.
 *
 * The script refuses to write evidence unless the private checkout is the exact
 * baseline revision and its tracked source is clean. It re-executes itself
 * under that checkout's tsx so the recorded tsx version belongs to the install
 * that ran the capture.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1050-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1050-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1061/capture-1061.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import type { Scenario } from "./scenarios.mts";
import {
    adoptionInvalidationScenarios,
    draftReadScenarios,
    mutableApplyScenarios,
    operationFoldingScenarios,
} from "./review-fixes.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1061";
const TICKET = "1061";
const BASELINE = process.env.PI_BASELINE ?? "pi-v1.3.0";
const CHECKOUT = process.env.PI_CHECKOUT;
const EXPECTED_REVISION = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const TSX = CHECKOUT ? join(CHECKOUT, "node_modules/.bin/tsx") : "";

if (!CHECKOUT) {
    console.error("PI_CHECKOUT must name the private frozen pi v1.3.0 checkout");
    process.exit(1);
}
if (!existsSync(TSX)) {
    console.error(`the checkout has no tsx: ${TSX}`);
    process.exit(1);
}

function run(command: string, arguments_: string[], cwd = REPO): string {
    return execFileSync(command, arguments_, { cwd, encoding: "utf8" }).trim();
}

function sha256(content: Buffer | string): string {
    return createHash("sha256").update(content).digest("hex");
}

if (!process.env.AUDIT_1061_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1061_TSX: "1",
            NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
            TSX_TSCONFIG_PATH: join(CHECKOUT, "tsconfig.json"),
        },
        stdio: "inherit",
    });
    process.exit(0);
}

const revision = run("git", ["rev-parse", "HEAD"], CHECKOUT);
if (revision !== EXPECTED_REVISION) {
    console.error(`checkout revision ${revision} is not ${EXPECTED_REVISION}`);
    process.exit(1);
}
const dirty = run("git", ["status", "--porcelain"], CHECKOUT);
if (dirty !== "") {
    console.error(`checkout is not clean:\n${dirty}`);
    process.exit(1);
}
if (process.platform !== "linux" || process.arch !== "x64") {
    console.error(`unsupported platform ${process.platform}-${process.arch}`);
    process.exit(1);
}
const report = process.report?.getReport() as { header?: { glibcVersionRuntime?: string } } | undefined;
if (!report?.header?.glibcVersionRuntime) {
    console.error("the captured environment is not glibc Linux");
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/chord/src/delta/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/chord/src/delta/index.ts is missing");
    process.exit(1);
}

const registryPath = join(REPO, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { revision?: string; bundles?: Record<string, unknown> }>;
};
const registered = registry.baselines[BASELINE];
if (registered?.revision !== EXPECTED_REVISION) {
    console.error(`baseline ${BASELINE} does not resolve to ${EXPECTED_REVISION}`);
    process.exit(1);
}
if (registered.bundles?.[FAMILY] !== undefined) {
    console.error(`${FAMILY} is already registered; a deliberate recapture needs an explicit registry replacement`);
    process.exit(1);
}

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const capturedAt = new Date().toISOString();
const environment = {
    platform: "linux",
    arch: "x64",
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};

const { draftArrayHoleScenarios, prepareReplaceScenarios } = await import("./scenarios.mts");

async function artifactFor(
    filename: string,
    capture: () => Promise<Scenario[]>,
    sourceEndpoints: string[],
): Promise<{ filename: string; content: string; scenarios: Scenario[]; sourceEndpoints: string[] }> {
    const scenarios = await capture();
    if (scenarios.length === 0) throw new Error(`${FAMILY}/${filename} captured no scenarios`);
    const names = new Set<string>();
    for (const scenario of scenarios) {
        if (names.has(scenario.name)) throw new Error(`${FAMILY}/${filename} has a duplicate scenario: ${scenario.name}`);
        names.add(scenario.name);
    }
    const artifact = {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: BASELINE,
        revision: EXPECTED_REVISION,
        ticket: TICKET,
        family: FAMILY,
        capturedAt,
        environment,
        sourceEndpoints,
        scenarios,
    };
    return { filename, content: `${JSON.stringify(artifact, null, 2)}\n`, scenarios, sourceEndpoints };
}

const artifacts = await Promise.all([
    artifactFor("prepare-replace.json", prepareReplaceScenarios, [
        "packages/chord/src/delta/tracker.ts:Tracker.prepareReplace,Tracker.adopt,Prepared.abort",
        "packages/chord/src/delta/index.ts:Op,applyImmutable",
        "packages/chord/test/delta.test.ts:takes immutable ownership of replacement input and applies no-op normalization",
        "packages/chord/test/delta.test.ts:rejects foreign, stale, and repeated preparations while allowing competing changes",
        "packages/chord/test/delta.test.ts:makes competing same-base preparations stale after adopting a no-op",
    ]),
    artifactFor("draft-array-holes.json", draftArrayHoleScenarios, [
        "packages/chord/src/delta/tracker.ts:deleteProperty",
        "packages/chord/src/delta/index.ts:Op",
    ]),
    artifactFor("review-fixes.json", async () => [
        ...(await mutableApplyScenarios()),
        ...(await draftReadScenarios()),
        ...(await adoptionInvalidationScenarios()),
        ...(await operationFoldingScenarios()),
    ], [
        "packages/chord/src/delta/index.ts:apply,applyOps",
        "packages/chord/src/delta/tracker.ts:getProperty,getArrayIndex",
        "packages/chord/src/delta/tracker.ts:TrackerImpl.adopt,TrackerImpl.#invalidate,assertReadable,assertWritable",
        "packages/chord/src/delta/tracker.ts:deleteProperty,emitObjectOperations,setProperty,materializePrepared",
    ]),
]);

const bundle = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(bundle, { recursive: true });
for (const artifact of artifacts) {
    writeFileSync(join(bundle, artifact.filename), artifact.content, "utf8");
    console.log(
        `${artifact.filename}: ${artifact.scenarios.length} scenario(s), ${Buffer.byteLength(artifact.content)} bytes`,
    );
}

const generatorPath = "fixtures/pi-v1.3.0/audit-1061/capture-1061.mts";
const sourceFiles = [
    "fixtures/pi-v1.3.0/audit-1061/capture-1061.mts",
    "fixtures/pi-v1.3.0/audit-1061/scenarios.mts",
    "fixtures/pi-v1.3.0/audit-1061/review-fixes.mts",
];
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision: EXPECTED_REVISION,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    generator: {
        path: generatorPath,
        sha256: sha256(readFileSync(join(REPO, generatorPath))),
        command:
            `PI_CHECKOUT=${CHECKOUT} PI_BASELINE=${BASELINE} ` +
            `${join(CHECKOUT, "node_modules/.bin/tsx")} ${generatorPath}`,
        sources: sourceFiles.map((path) => ({ path, sha256: sha256(readFileSync(join(REPO, path))) })),
    },
    artifacts: artifacts.map((artifact) => ({
        path: artifact.filename,
        family: FAMILY,
        bytes: Buffer.byteLength(artifact.content),
        sha256: sha256(artifact.content),
        sourceEndpoints: artifact.sourceEndpoints,
        scenarios: artifact.scenarios,
    })),
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(bundle, "manifest.json"), manifestContent, "utf8");
console.log(`manifest.json: ${Buffer.byteLength(manifestContent)} bytes, sha256 ${sha256(manifestContent)}`);
console.log(
    JSON.stringify(
        {
            [FAMILY]: {
                manifest: `${FAMILY}/manifest.json`,
                manifestSha256: sha256(manifestContent),
            },
        },
        null,
        2,
    ),
);
