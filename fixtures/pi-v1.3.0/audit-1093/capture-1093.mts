/**
 * `audit-1093` capture: the deterministic-run OUTCOME-before-settle ordering
 * that the `audit-1053` `deterministic-replay` artifact did not record —
 * `CodemodeRun.finished` is set when the outcome is decided, strictly before
 * `result` settles, and a finished run ignores later answers.
 *
 * The script refuses to write evidence unless the checkout is the exact
 * baseline revision and its tracked source is clean. It re-executes itself
 * under the checkout's tsx so the recorded runtime belongs to the same
 * install. A deliberate recapture needs AUDIT_1093_REPLACE=1.
 *
 * The scenario module loads the frozen checkout, so it is imported dynamically
 * only in the re-executed process, which runs with the checkout's `source`
 * export condition; this outer process stays free of frozen imports so it runs
 * before the re-execution guard below.
 *
 * Run from the repository root:
 *   NODE_OPTIONS=--conditions=source PI_CHECKOUT=/tmp/cch-1080-pi-frozen \
 *     PI_BASELINE=pi-v1.3.0 /tmp/cch-1080-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1093/capture-1093.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import type { Scenario } from "./pi.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1093";
const TICKET = "1093";
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

if (!process.env.AUDIT_1093_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1093_TSX: "1",
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
if (!existsSync(join(CHECKOUT, "packages/codemode/src/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/codemode/src/index.ts is missing");
    process.exit(1);
}

const registryPath = join(REPO, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { bundles?: Record<string, unknown> }>;
};
if (registry.baselines[BASELINE]?.bundles?.[FAMILY] !== undefined && process.env.AUDIT_1093_REPLACE !== "1") {
    console.error(`${FAMILY} is already registered; set AUDIT_1093_REPLACE=1 for a deliberate recapture`);
    process.exit(1);
}

const { outcomeVisibleBeforeSettleOnAbort, preAbortedRunIsFinishedAtStart, finishedTransitionsOnTheNormalPath } =
    await import("./outcome-settle.mts");

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

const outcomeSettleNormalizations = [
    "pi decides the outcome synchronously in the AbortSignal listener, on the thread that calls abort(); the contract pins the visibility ordering (finished strictly before the result settles), not the thread that decides — another caller may observe the decision on the run's own thread",
    "the settle-continuation witness is a .then callback on the result promise; the promise may already be resolved while the callback has not run, so the witness records whether the caller's continuation ran, not whether the promise settled",
    "wall-clock call durations are compared as a presence flag; the deterministic observations do not pin duration values",
    "recorded output and error text are the scenario's own markers",
];

const artifactSpec = {
    path: "outcome-settle.json",
    sourceEndpoints: [
        "packages/codemode/src/types.ts:CodemodeRun.finished,CodemodeRun.result",
        "packages/codemode/src/runtime/execution.ts:Execution.finish,CodemodeRun.finished,CodemodeRun.deliver",
        "packages/codemode/test/deterministic.test.ts:ends with an error the script cannot catch when the caller aborts,is finished once it has its outcome",
    ],
    normalizations: outcomeSettleNormalizations,
    factories: [outcomeVisibleBeforeSettleOnAbort, preAbortedRunIsFinishedAtStart, finishedTransitionsOnTheNormalPath],
};

const capturedAt = new Date().toISOString();
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};

const pinnedSources = [SCRIPT, join(artifactDirectory, "outcome-settle.mts"), join(artifactDirectory, "pi.mts")].map(
    (path) => ({ path: relative(REPO, path), sha256: sha256(readFileSync(path)) }),
);

console.error(`START ${artifactSpec.path}`);
const scenarios: Scenario[] = [];
for (const factory of artifactSpec.factories) scenarios.push(await factory());
console.error(`DONE ${artifactSpec.path}`);
const names = new Set<string>();
for (const scenario of scenarios) {
    if (!scenario.name.trim() || !scenario.source.trim() || scenario.expected === undefined) {
        throw new Error("a scenario needs a name, source and expected value");
    }
    if (names.has(scenario.name)) throw new Error(`duplicate scenario ${scenario.name}`);
    names.add(scenario.name);
    if (JSON.stringify(scenario.expected) !== JSON.stringify(scenario.observed)) {
        throw new Error(`expected and observed diverge in ${scenario.name}:\n${JSON.stringify(scenario.observed)}`);
    }
}

const records = scenarios.map((scenario) => ({
    name: scenario.name,
    inputs: scenario.inputs,
    expected: scenario.expected,
    observed: scenario.observed,
    normalizations: artifactSpec.normalizations,
}));
const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    sourceEndpoints: artifactSpec.sourceEndpoints,
    scenarios: records,
};
const content = `${JSON.stringify(artifact, null, 2)}\n`;
writeFileSync(join(artifactDirectory, artifactSpec.path), content);
console.log(`captured ${artifactSpec.path} (${scenarios.length} scenarios)`);

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
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1093/capture-1093.mts",
        sources: pinnedSources,
    },
    artifacts: [
        {
            path: artifactSpec.path,
            family: FAMILY,
            bytes: Buffer.byteLength(content),
            sha256: sha256(content),
            sourceEndpoints: artifactSpec.sourceEndpoints,
            scenarios: records,
        },
    ],
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
