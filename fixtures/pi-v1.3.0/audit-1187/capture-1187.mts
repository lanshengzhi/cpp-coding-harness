/**
 * `audit-1187` capture: Chord progress pacing in frozen pi v1.3.0.
 *
 * The `cch_chord:output:progress` row had no frozen observation because a wall-clock pacing
 * measurement would normalize scheduler lateness into the expected value. This capture records the
 * deterministic commit schedule of the frozen `Progress` class under virtual timers instead: the
 * pacing arithmetic is observed exactly, and no C++ expectation is an input.
 *
 * The script refuses a checkout whose HEAD is not the registered baseline revision, a modified
 * source tree, a non-Linux/glibc host, and a family already registered. It re-executes itself under
 * the checkout's tsx so the recorded `tsx` version belongs to the same install.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/pi-1113-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/pi-1113-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1187/capture-1187.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1187";
const TICKET = "1187";
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

if (!process.env.AUDIT_1187_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1187_TSX: "1",
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

const registryPath = join(REPO, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { bundles?: Record<string, { manifest?: string }> }>;
};
const registered = registry.baselines[BASELINE]?.bundles?.[FAMILY];
if (registered) {
    console.error(`${FAMILY} is already registered: ${registered.manifest}`);
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/chord/src/output.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/chord/src/output.ts is missing");
    process.exit(1);
}

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

const { progressKeepsExactFractionalByteRatePause, progressPacesLaterCommitsByWrittenBytes } = await import(
    "./scenarios.mts"
);

interface ArtifactDefinition {
    path: string;
    sourceEndpoints: string[];
    scenarios: () => Promise<Array<{ name: string; source: string; expected: unknown }>>;
    inputs: Record<string, unknown>;
    normalizations: string[];
}

const NORMALIZATIONS = [
    "the frozen Progress class runs under deterministic virtual timers, so the observation is its pacing arithmetic and never a wall-clock latency measurement",
    "commit timestamps and written byte counts are recorded exactly; no elapsed-time bound or tolerance is applied",
];

const ARTIFACTS: ArtifactDefinition[] = [
    {
        path: "chord-progress.json",
        sourceEndpoints: [
            "packages/chord/src/output.ts:PROGRESS_BYTES_PER_SECOND,Progress",
            "packages/chord/test/output.test.ts:Progress",
            "packages/durable/src/harness/tool.ts:publishProgress,PROGRESS_BYTES_PER_SECOND",
        ],
        scenarios: async () => [
            await progressPacesLaterCommitsByWrittenBytes(),
            await progressKeepsExactFractionalByteRatePause(),
        ],
        inputs: {
            "progress commits the first change at once then paces later commits by their written bytes": {
                minIntervalMs: 100,
                writeBytes: [51_200, 10, 10],
                steps: [
                    { mark: 1 },
                    { advanceMs: 0 },
                    { mark: 2 },
                    { advanceMs: 499 },
                    { advanceMs: 1 },
                    { mark: 1 },
                    { advanceMs: 99 },
                    { advanceMs: 1 },
                ],
            },
            "progress keeps the exact fractional byte-rate pause instead of rounding it": {
                minIntervalMs: 0,
                writeBytes: [1_025, 1],
                steps: [{ mark: 1 }, { advanceMs: 0 }, { mark: 1 }, { advanceMs: 10.009765625 }],
            },
        },
        normalizations: NORMALIZATIONS,
    },
];

const capturedAt = new Date().toISOString();
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};

const pinnedSources = [SCRIPT, join(artifactDirectory, "scenarios.mts"), join(artifactDirectory, "pi.mts")].map(
    (path) => ({ path: relative(REPO, path), sha256: sha256(readFileSync(path)) }),
);

const manifestArtifacts: Array<Record<string, unknown>> = [];
for (const definition of ARTIFACTS) {
    const scenarios = await definition.scenarios();
    const names = new Set<string>();
    for (const scenario of scenarios) {
        if (!scenario.name.trim() || !scenario.source.trim() || scenario.expected === undefined) {
            throw new Error(`${definition.path}: a scenario needs a name, source and expected value`);
        }
        if (names.has(scenario.name)) throw new Error(`${definition.path}: duplicate scenario ${scenario.name}`);
        names.add(scenario.name);
        if (definition.inputs[scenario.name] === undefined) {
            throw new Error(`${definition.path}: scenario ${scenario.name} has no declared inputs`);
        }
    }
    const records = scenarios.map((scenario) => ({
        name: scenario.name,
        inputs: definition.inputs[scenario.name],
        expected: scenario.expected,
        normalizations: definition.normalizations,
    }));
    const artifact = {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: BASELINE,
        revision,
        ticket: TICKET,
        family: FAMILY,
        capturedAt,
        environment,
        sourceEndpoints: definition.sourceEndpoints,
        scenarios: records,
    };
    const content = `${JSON.stringify(artifact, null, 2)}\n`;
    writeFileSync(join(artifactDirectory, definition.path), content);
    manifestArtifacts.push({
        path: definition.path,
        family: FAMILY,
        bytes: Buffer.byteLength(content),
        sha256: sha256(content),
        sourceEndpoints: definition.sourceEndpoints,
        scenarios: records,
    });
    console.log(`captured ${definition.path} (${scenarios.length} scenarios)`);
}

const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    generator: {
        path: relative(REPO, SCRIPT),
        sha256: sha256(readFileSync(SCRIPT)),
        command:
            "PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1187/capture-1187.mts",
        sources: pinnedSources,
    },
    artifacts: manifestArtifacts,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
