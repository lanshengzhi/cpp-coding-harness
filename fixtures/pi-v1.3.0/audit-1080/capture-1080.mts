/**
 * `audit-1080` capture: submission/inbox boundary observations against a private frozen pi v1.3.0
 * checkout — the steer-at-postTools placement, the request-ID dedup with a different payload,
 * conversation-abort outcomes on queued input vs queued write, the follow-up-at-final-boundary
 * ordering and the atomic queued admission batch — plus the reset and stale-write boundaries in a
 * second artifact (`reset-writes.json`): the pinned-head write settled unanswered and stale at an
 * entry before the reset head, the queued `head: "self"` reset at the postTools boundary ending
 * the current run unanswered and reset before starting the selected queued input in the reset
 * context, and the display-only write queuing behind a queued reset instead of settling done at
 * once.
 *
 * The script refuses to write evidence unless the checkout is the exact baseline revision and its
 * tracked source is clean. It re-executes itself under the checkout's tsx so the recorded runtime
 * belongs to the same install. The family is already registered, so a deliberate recapture needs
 * AUDIT_1080_REPLACE=1; the parent serial merger re-registers the new manifest after the merge.
 *
 * The scenario modules load the frozen checkout, so they are imported dynamically only in the
 * re-executed process, which runs with the checkout's `source` export condition; this outer
 * process stays free of frozen imports so it runs before the re-execution guard below.
 *
 * Run from the Pike repository:
 *   AUDIT_1080_REPLACE=1 NODE_OPTIONS=--conditions=source PI_CHECKOUT=/tmp/cch-1080-pi-frozen \
 *     PI_BASELINE=pi-v1.3.0 /tmp/cch-1080-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1080/capture-1080.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import type { Scenario } from "./scenarios.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1080";
const TICKET = "1080";
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

if (!process.env.AUDIT_1080_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1080_TSX: "1",
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
if (registry.baselines[BASELINE]?.bundles?.[FAMILY] !== undefined && process.env.AUDIT_1080_REPLACE !== "1") {
    console.error(`${FAMILY} is already registered; set AUDIT_1080_REPLACE=1 for a deliberate recapture`);
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/durable/src/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/durable/src/index.ts is missing");
    process.exit(1);
}

const {
    atomicQueuedAdmissionBatch,
    conversationAbortQueuedInputVsWrite,
    followUpAtFinalBoundaryAnsweredBySecondRun,
    requestIdDifferentPayloadReturnsExisting,
    steerAtPostToolsFollowUpHeldToFinal,
} = await import("./scenarios.mts");
const {
    displayOnlyWriteBehindQueuedResetQueues,
    queuedResetAtPostToolsEndsRunAndStartsQueuedInputsWithResetContext,
    stalePinnedHeadWriteAfterResetHead,
} = await import("./reset-scenarios.mts");

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

/** Fixture inputs for each scenario; the executable description is the scenario function. */
function scenarioInputs(name: string): Record<string, unknown> {
    const inputs: Record<string, Record<string, unknown>> = {
        "a busy steer input is placed at the postTools boundary and a busy follow-up is held to the final boundary": {
            submissions: [
                { type: "input", content: "a" },
                { type: "input", content: "s", whenBusy: "steer" },
                { type: "input", content: "f" },
            ],
            tool: "hold",
            turns: ["toolUse hold", "after tools", "follow-up"],
        },
        "a duplicate request ID with a different payload returns the existing submission with zero new storage batches": {
            requestId: "req-1080",
            firstPayload: "a",
            secondPayload: "a different payload",
            storage: "recording MemoryStorage",
        },
        "a conversation abort settles the queued input unanswered and aborted, keeps the queued write queued and settles the placed input unanswered and aborted": {
            submissions: [
                "placed input",
                "queued input",
                { type: "write", entry: { kind: "note", model: [{ role: "user", content: "n", timestamp: 0 }] } },
            ],
            abort: "conversation abort",
        },
        "a busy follow-up is placed at the final boundary and answered by a second run": {
            submissions: [
                { type: "input", content: "a" },
                { type: "input", content: "f" },
            ],
            turns: ["held first response answers first", "second"],
        },
        "a busy queued admission commits one batch with the submission and the pi.inbox items change": {
            storage: "recording MemoryStorage",
            submissions: ["busy input", "queued input"],
        },
        "a pinned-head write at the entry before the reset head settles unanswered and stale while one at the reset head is done": {
            oldEntry: { kind: "note" },
            reset: "head:self without handoff on an idle conversation",
            writes: [
                { kind: "summary", head: "the note before the reset" },
                { kind: "summary", head: "the reset head" },
            ],
        },
        "a queued head:self reset at the postTools boundary ends the current run unanswered and reset and starts the selected queued input in the reset context": {
            submissions: [
                { type: "input", content: "a" },
                { type: "input", content: "f" },
            ],
            tool: "hold",
            reset: "queued head:self without handoff while the tool round is held",
            followUpAnswer: "fresh",
        },
        "a display-only write behind a queued reset queues and lands after the reset when the run ends": {
            submissions: [{ type: "input", content: "a" }],
            tool: "hold",
            reset: "queued head:self without handoff while the tool round is held",
            write: { kind: "note", displayOnly: true },
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

const inboxNormalizations = [
    "wall-clock capture timestamps are omitted; the Harness clock is not part of these observations",
    "generated task, conversation, entry and submission IDs are compared only where the identity relation is the observation; answer identity is also recorded as the ordinal of the answering assistant entry",
    "the faux provider table and the recording storage hold no durable state beyond the recorded batches",
];
const resetNormalizations = [
    ...inboxNormalizations,
    "follow-up request messages are recorded as role:text strings with the system baseline text elided",
];
const artifactSpecs: ArtifactSpec[] = [
    {
        path: "submissions-inbox.json",
        sourceEndpoints: [
            "packages/durable/src/harness/submissions.ts:Submissions,admitSubmission,submissionByRequest",
            "packages/durable/src/harness/inbox.ts:InboxDoc,prepareBoundary,applyBoundary,withdrawQueuedInputs",
            "packages/durable/src/harness/generation.ts:finishToolRound,answer,endRun",
            "packages/durable/src/session/transaction.ts:createSubmission",
            "packages/durable/test/harness-inbox.test.ts:postTools boundary",
            "packages/durable/test/harness-submissions.test.ts:request id",
        ],
        normalizations: inboxNormalizations,
        factories: [
            steerAtPostToolsFollowUpHeldToFinal,
            requestIdDifferentPayloadReturnsExisting,
            conversationAbortQueuedInputVsWrite,
            followUpAtFinalBoundaryAnsweredBySecondRun,
            atomicQueuedAdmissionBatch,
        ],
    },
    {
        path: "reset-writes.json",
        sourceEndpoints: [
            "packages/durable/src/harness/inbox.ts:isStale,isDisplayOnly,hasQueuedReset,applyBoundary",
            "packages/durable/src/harness/submissions.ts:admitSubmission",
            "packages/durable/src/harness/generation.ts:finishToolRound",
            "packages/durable/src/harness/live.ts:endRun",
            "packages/durable/src/harness/harness.ts:reset",
            "packages/durable/test/harness-inbox.test.ts:queued reset after tools, stale head write, display-only write",
        ],
        normalizations: resetNormalizations,
        factories: [
            stalePinnedHeadWriteAfterResetHead,
            queuedResetAtPostToolsEndsRunAndStartsQueuedInputsWithResetContext,
            displayOnlyWriteBehindQueuedResetQueues,
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
    join(artifactDirectory, "scenarios.mts"),
    join(artifactDirectory, "reset-scenarios.mts"),
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
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1080/capture-1080.mts",
        sources: pinnedSources,
    },
    artifacts,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
