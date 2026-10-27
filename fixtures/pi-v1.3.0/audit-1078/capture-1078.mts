/**
 * `audit-1078` capture: the ownership, join, abort and wait boundaries of the
 * durable Harness that the `audit-1055` `ownership-abort` artifact did not
 * record — the background ownership boundary, the ordinary idle wait and the
 * ownership restrictions a transaction enforces.
 *
 * The script refuses to write evidence unless the checkout is the exact
 * baseline revision and its tracked source is clean. It re-executes itself
 * under the checkout's tsx so the recorded runtime belongs to the same
 * install. A deliberate recapture needs AUDIT_1078_REPLACE=1.
 *
 * The scenario module loads the frozen checkout, so it is imported dynamically
 * only in the re-executed process, which runs with the checkout's `source`
 * export condition; this outer process stays free of frozen imports so it runs
 * before the re-execution guard below.
 *
 * Run from the Pike repository:
 *   AUDIT_1078_REPLACE=1 NODE_OPTIONS=--conditions=source PI_CHECKOUT=/tmp/cch-1080-pi-frozen \
 *     PI_BASELINE=pi-v1.3.0 /tmp/cch-1080-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1078/capture-1078.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import type { Scenario } from "./pi.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1078";
const TICKET = "1078";
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

if (!process.env.AUDIT_1078_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1078_TSX: "1",
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
    baselines: Record<string, { bundles?: Record<string, unknown> }>;
};
if (registry.baselines[BASELINE]?.bundles?.[FAMILY] !== undefined && process.env.AUDIT_1078_REPLACE !== "1") {
    console.error(`${FAMILY} is already registered; set AUDIT_1078_REPLACE=1 for a deliberate recapture`);
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/durable/src/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/durable/src/index.ts is missing");
    process.exit(1);
}

const {
    backgroundBoundarySurvivesConversationAbort,
    backgroundCrossingConversationAbort,
    idleWaitIgnoresBackgroundWork,
    ownershipRestrictionsRejected,
} = await import("./ownership-waits.mts");

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

/** Fixture inputs for each scenario; the executable description is the scenario function. */
function scenarioInputs(name: string): Record<string, unknown> {
    const inputs: Record<string, Record<string, unknown>> = {
        "a conversation abort keeps live background work while a direct abort reaches it": {
            conversationAbort: "ordinary",
            tasks: ["foreground (ordinary)", "background"],
            directAbort: "abortTask(background)",
        },
        "a background-crossing conversation abort reaches and waits for a live background task": {
            conversationAbort: "{background: true}",
            tasks: ["background"],
        },
        "an idle wait resolves with live background work and holds on a live foreground task": {
            idleWaits: ["a background-only conversation", "the root with a live foreground task", "the harness"],
            tasks: ["foreground (ordinary)", "background", "another conversation's background"],
        },
        "a child task and a task-owned conversation are rejected for a terminal owner and accepted for a live one": {
            rejected: [
                "child of a terminal owner",
                "child of a missing owner",
                "background child",
                "child in another conversation",
                "conversation owned by a terminal task",
            ],
            accepted: ["child of a live owner", "conversation owned by a live task"],
        },
    };
    return inputs[name] ?? { scenario: name };
}

/** One artifact of the family: its frozen-source endpoints and the scenarios it carries. */
type ArtifactSpec = {
    readonly path: string;
    readonly sourceEndpoints: readonly string[];
    readonly normalizations: readonly string[];
    readonly factories: ReadonlyArray<() => Promise<Scenario>>;
};

const ownershipNormalizations = [
    "wall-clock capture timestamps are omitted; the Harness clock is not part of these observations",
    "generated task and conversation IDs are compared only where the identity relation is the observation; an owner edge is compared as a boolean relation to the same ID the scenario created",
    "the in-memory storage holds no durable state beyond the captured records",
    "task log entries are the scenario's own phase and abort markers, recorded in the order the scheduler ran them",
];

const artifactSpecs: ArtifactSpec[] = [
    {
        path: "ownership-waits.json",
        sourceEndpoints: [
            "packages/durable/src/harness/scheduler.ts:abortConversation,waitForIdle,#walkDown,#inScope,#idle",
            "packages/durable/src/session/transaction.ts:Transaction.createTask,Transaction.createConversation,#validateOwners",
            "packages/durable/test/harness-ownership.test.ts:background boundary,idle wait,ownership restrictions",
        ],
        normalizations: ownershipNormalizations,
        factories: [
            backgroundBoundarySurvivesConversationAbort,
            backgroundCrossingConversationAbort,
            idleWaitIgnoresBackgroundWork,
            ownershipRestrictionsRejected,
        ],
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

const pinnedSources = [
    SCRIPT,
    join(artifactDirectory, "ownership-waits.mts"),
    join(artifactDirectory, "pi.mts"),
].map((path) => ({ path: relative(REPO, path), sha256: sha256(readFileSync(path)) }));

const allFactories = artifactSpecs.flatMap((spec) => spec.factories);
console.error(`START ${artifactSpecs.map((spec) => spec.path).join(", ")}`);
const scenarios: Scenario[] = [];
for (const factory of allFactories) scenarios.push(await factory());
console.error(`DONE ${artifactSpecs.map((spec) => spec.path).join(", ")}`);
const names = new Set<string>();
for (const scenario of scenarios) {
    if (!scenario.name.trim() || !scenario.source.trim() || scenario.expected === undefined) {
        throw new Error("a scenario needs a name, source and expected value");
    }
    if (names.has(scenario.name)) throw new Error(`duplicate scenario ${scenario.name}`);
    names.add(scenario.name);
}

const artifacts = artifactSpecs.map((spec) => {
    const offset = artifactSpecs
        .slice(0, artifactSpecs.indexOf(spec))
        .reduce((sum, prior) => sum + prior.factories.length, 0);
    const own = scenarios.slice(offset, offset + spec.factories.length);
    const records = own.map((scenario) => ({
        name: scenario.name,
        inputs: scenarioInputs(scenario.name),
        expected: scenario.expected,
        normalizations: spec.normalizations,
    }));
    const artifact = {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: BASELINE,
        revision,
        ticket: TICKET,
        family: FAMILY,
        capturedAt,
        environment,
        sourceEndpoints: spec.sourceEndpoints,
        scenarios: records,
    };
    const content = `${JSON.stringify(artifact, null, 2)}\n`;
    writeFileSync(join(artifactDirectory, spec.path), content);
    console.log(`captured ${spec.path} (${own.length} scenarios)`);
    return {
        path: spec.path,
        family: FAMILY,
        bytes: Buffer.byteLength(content),
        sha256: sha256(content),
        sourceEndpoints: spec.sourceEndpoints,
        scenarios: records,
    };
});

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
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1078/capture-1078.mts",
        sources: pinnedSources,
    },
    artifacts,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
