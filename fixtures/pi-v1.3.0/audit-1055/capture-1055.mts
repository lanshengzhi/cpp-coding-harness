/**
 * `audit-1055` capture: durable tasks, submissions, inbox admission, ownership/abort, versioning and
 * real process crash/reopen against a private frozen pi v1.3.0 checkout.
 *
 * The script refuses to write evidence unless the checkout is the exact baseline revision and its
 * tracked source is clean. It re-executes itself under the checkout's tsx so the recorded runtime
 * belongs to the same install. Crash scenarios spawn `crash-worker.mts` and SIGKILL it only after the
 * worker reports a committed boundary.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1055-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1055-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1055/capture-1055.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import {
    abandonedOnRestart,
    abortCascadeBottomUp,
    abortOrderingWithHooks,
    abortResults,
    allSettledJoin,
    atomicAdmissionBatch,
    busyFollowUpQueued,
    busyReject,
    busySteerQueued,
    displayOnlyWriteWhileBusy,
    idleWriteSettles,
    completingHold,
    conversationAbortEffects,
    counterLifecycle,
    crashedAbortMark,
    crashedGenerationSubmission,
    crashedTerminalTask,
    crashedTransferTask,
    definitionHandover,
    failFastJoin,
    hooksOrderAndIsolation,
    idleInputAnswered,
    inadmissibleOwnership,
    lifecycleTimesAcrossWait,
    memoAcrossCheckpoint,
    migrationBeforeReservation,
    migrationFailureAndRecovery,
    missingDefinition,
    queueModes,
    requestIdDeduplication,
    storageFailureRecovery,
    submissionWaitCancellation,
    versionTooOld,
    waitCancellationIsNotAbort,
    type Scenario,
} from "./scenarios.mts";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1055";
const TICKET = "1055";
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

if (!process.env.AUDIT_1055_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1055_TSX: "1",
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
if (registry.baselines[BASELINE]?.bundles?.[FAMILY] !== undefined) {
    console.error(`${FAMILY} is already registered`);
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/durable/src/index.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/durable/src/index.ts is missing");
    process.exit(1);
}

const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const artifactDirectory = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(artifactDirectory, { recursive: true });

interface ArtifactDefinition {
    path: string;
    sourceEndpoints: string[];
    scenarios: () => Promise<Scenario[]>;
}

/** Fixture inputs for each scenario; the executable description is the scenario function. */
function scenarioInputs(name: string): Record<string, unknown> {
    const inputs: Record<string, Record<string, unknown>> = {
        "a multi-phase task replaces checkpoint state and reports a typed terminal result": {
            task: "audit.counter",
            input: { to: 3 },
            clockMs: 1000,
        },
        "task lifecycle times come from the Harness clock and survive a durable wait": {
            task: "audit.clock",
            waitingOn: "audit.hold-child",
            clockMs: [1000, 3000, 4000],
        },
        "a task memo survives checkpoint replacement and is absent from the terminal record": {
            task: "audit.memo",
            memo: { requested: 7 },
        },
        "a completing owner holds its terminal outcome until ordinary owned work drains": {
            task: "audit.hold-parent",
            child: "audit.hold-child",
        },
        "task hooks run in extension order and an ordinary throw is reported while the task continues": {
            task: "audit.hooked",
            hooks: ["hook-throw", "hook-observer"],
        },
        "reservation migrates a stored definition before running the newer version": {
            storedVersion: 1,
            installedVersion: 2,
            migrate: "input.n += fromVersion; checkpoint.n += 10",
        },
        "a task whose stored version is newer than the installed definition stays pending and blocked": {
            storedVersion: 2,
            installedVersion: 1,
        },
        "a task with no installed definition stays pending and runs when its definition returns": {
            installedDefinition: "removed then restored",
            version: 1,
        },
        "a failed migration is reported and leaves the task pending until a usable definition is installed": {
            storedVersion: 1,
            brokenVersion: 2,
            repairedVersion: 2,
        },
        "a running task hands over to a newer definition at a phase boundary": {
            runningVersion: 1,
            replacementVersion: 2,
            boundary: "after p1 checkpoint commit",
        },
        "inadmissible child ownership is rejected before the commit writes": {
            cases: ["terminal owner", "missing owner", "background child", "other conversation"],
        },
        "a storage failure at the terminal commit fails the Session and the task reruns after reopen": {
            failurePoint: "terminal task commit",
            storage: "node:sqlite",
        },
        "a failFast join aborts live siblings and the parent reads ordered terminal outcomes": {
            join: "failFast",
            children: ["failed", "running", "running"],
        },
        "an allSettled join lets surviving siblings finish and still reads ordered outcomes": {
            join: "allSettled",
            children: ["failed", "completing"],
        },
        "aborting a parent runs descendants' abort handlers before the parent and preserves order": {
            abort: "abortTask(parent)",
            child: "running",
        },
        "abortTask marks a running task and runs its abort handler after the run invocation joins": {
            task: "audit.abort-order",
            abortAt: "while the run phase awaits its signal",
        },
        "cancelling a wait cancels only the waiter; the persistent task keeps running and completes": {
            task: "audit.slow",
            cancellation: "waitForTask AbortSignal",
        },
        "an idle input is placed and answered by a run": { submission: "input", busy: false },
        "an idle passive write is appended and settled done without starting a run": {
            submission: "write",
            entry: "audit.note",
            busy: false,
        },
        "a busy steer input queues in steer mode and a conversation abort withdraws it": {
            submission: "input",
            busy: true,
            whenBusy: "steer",
        },
        "a display-only write is appended and settled while the conversation is busy": {
            submission: "write",
            entry: "display-only",
            busy: true,
        },
        "a busy input queues as a follow-up and is placed at the final boundary": {
            submission: "input",
            busy: true,
            whenBusy: "followUp",
        },
        "a duplicate request ID returns the existing submission without a storage write": {
            requestId: "req-9",
            duplicateType: "input vs write",
        },
        "a busy input with whenBusy reject throws ConversationBusy and writes no submission": {
            whenBusy: "reject",
            busy: true,
        },
        "admission commits the queued submission and its inbox change in one storage batch": {
            storage: "recording MemoryStorage",
            submission: "busy input",
        },
        "queue modes decide whether all follow-ups or one are placed at the final boundary": {
            followUpMode: ["all", "one-at-a-time"],
            followUps: 2,
        },
        "abort reports queued, placed and settled outcomes and removes the queued item": {
            submissions: ["placed input", "queued input"],
        },
        "conversation abort withdraws queued inputs, keeps queued writes and settles the placed input unanswered": {
            submissions: ["placed input", "queued input", "queued write"],
        },
        "cancelling a submission wait leaves the submission placed until it settles": {
            cancellation: "Submission.wait AbortSignal",
        },
        "a killed process leaves an admitted input and its generation task recoverable without a duplicate submission": {
            worker: "generation",
            kill: "SIGKILL after request dispatch",
            storage: "node:sqlite",
            requestId: "crash-1",
        },
        "a killed process resumes an intent/effect/outcome task from its checkpoint and applies the effect once": {
            worker: "effect",
            kill: "SIGKILL after effect, before outcome commit",
            storage: "node:sqlite",
        },
        "an abort mark survives a killed process and only the abort handler runs after reopen": {
            worker: "abort",
            kill: "SIGKILL inside the abort handler before its terminal commit",
        },
        "a killed process after a terminal commit does not rerun the task or duplicate the submission": {
            worker: "terminal",
            kill: "SIGKILL after terminal task and write settlement",
        },
        "a task abandoned on restart is aborted with reason restart after reopen": {
            option: "abandonOnRestart",
            reopen: "clean close then open",
        },
    };
    return inputs[name] ?? { scenario: name };
}

const artifacts: ArtifactDefinition[] = [
    {
        path: "task-lifecycle.json",
        sourceEndpoints: [
            "packages/durable/src/tasks.ts:defineTask",
            "packages/durable/src/harness/scheduler.ts:TaskScheduler",
            "packages/durable/src/session/transaction.ts:Transaction.createTask",
            "packages/durable/test/harness-tasks.test.ts:task phases",
            "packages/durable/test/harness-ownership.test.ts:structured concurrency",
        ],
        scenarios: async () => [
            await counterLifecycle(),
            await lifecycleTimesAcrossWait(),
            await memoAcrossCheckpoint(),
            await completingHold(),
            await hooksOrderAndIsolation(),
        ],
    },
    {
        path: "task-versioning.json",
        sourceEndpoints: [
            "packages/durable/src/harness/scheduler.ts:#reserve,#decide,#blockedReason",
            "packages/durable/src/session/transaction.ts:Transaction.createTask",
            "packages/durable/src/session/session.ts:HarnessImpl.fail",
            "packages/durable/test/harness-tasks-recovery.test.ts:versioned",
            "packages/durable/test/harness-tasks.test.ts:migration",
        ],
        scenarios: async () => [
            await migrationBeforeReservation(),
            await versionTooOld(),
            await missingDefinition(),
            await migrationFailureAndRecovery(),
            await definitionHandover(),
            await inadmissibleOwnership(),
            await storageFailureRecovery(),
        ],
    },
    {
        path: "ownership-abort.json",
        sourceEndpoints: [
            "packages/durable/src/harness/scheduler.ts:#runAbort,#abortTask",
            "packages/durable/src/harness/inbox.ts:withdrawQueuedInputs",
            "packages/durable/test/harness-ownership.test.ts:failFast",
            "packages/durable/test/harness-tasks.test.ts:abort",
        ],
        scenarios: async () => [
            await failFastJoin(),
            await allSettledJoin(),
            await abortCascadeBottomUp(),
            await abortOrderingWithHooks(),
            await waitCancellationIsNotAbort(),
        ],
    },
    {
        path: "submissions-inbox.json",
        sourceEndpoints: [
            "packages/durable/src/harness/submissions.ts:Submissions,admitSubmission",
            "packages/durable/src/harness/inbox.ts:InboxDoc,prepareBoundary,applyBoundary",
            "packages/durable/src/harness/harness.ts:Harness.submit,abortSubmission",
            "packages/durable/test/harness-inbox.test.ts:inbox",
            "packages/durable/test/harness-submissions.test.ts:submissions",
        ],
        scenarios: async () => [
            await idleInputAnswered(),
            await idleWriteSettles(),
            await busyFollowUpQueued(),
            await busySteerQueued(),
            await displayOnlyWriteWhileBusy(),
            await requestIdDeduplication(),
            await busyReject(),
            await atomicAdmissionBatch(),
            await queueModes(),
            await abortResults(),
            await conversationAbortEffects(),
            await submissionWaitCancellation(),
        ],
    },
    {
        path: "crash-recovery.json",
        sourceEndpoints: [
            "packages/durable/src/harness/scheduler.ts:Harness.open,#reconcile,#runAbort",
            "packages/durable/src/storage/sqlite/node.ts:openNodeSqliteStorage",
            "packages/durable/src/harness/submissions.ts:Submissions.wait",
            "packages/durable/test/harness-tasks-recovery.test.ts:crash recovery",
            "packages/durable/test/harness-submissions.test.ts:reacquires a submission after reopen",
        ],
        scenarios: async () => [
            await crashedGenerationSubmission(),
            await crashedTransferTask(),
            await crashedAbortMark(),
            await crashedTerminalTask(),
            await abandonedOnRestart(),
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
const normalizations = [
    "temporary SQLite paths, temporary effect files and the killed worker's process id are omitted",
    "wall-clock capture timestamps are omitted; durable lifecycle times use the Harness clock recorded in the scenario",
    "generated task, conversation and submission IDs are compared only where the identity relation is the observation",
];

const pinnedSources = [
    SCRIPT,
    join(artifactDirectory, "scenarios.mts"),
    join(artifactDirectory, "audit-tasks.mts"),
    join(artifactDirectory, "pi.mts"),
    join(artifactDirectory, "crash-worker.mts"),
].map((path) => ({ path: relative(REPO, path), sha256: sha256(readFileSync(path)) }));

const manifestArtifacts: Array<Record<string, unknown>> = [];
for (const definition of artifacts) {
    console.error(`START ${definition.path}`);
    const scenarios = await definition.scenarios();
    console.error(`DONE ${definition.path}`);
    const names = new Set<string>();
    for (const scenario of scenarios) {
        if (!scenario.name.trim() || !scenario.source.trim() || scenario.expected === undefined) {
            throw new Error(`${definition.path}: a scenario needs a name, source and expected value`);
        }
        if (names.has(scenario.name)) throw new Error(`${definition.path}: duplicate scenario ${scenario.name}`);
        names.add(scenario.name);
    }
    const records = scenarios.map((scenario) => ({
        name: scenario.name,
        inputs: scenarioInputs(scenario.name),
        expected: scenario.expected,
        normalizations,
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

const captureRelative = relative(REPO, SCRIPT);
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    generator: {
        path: captureRelative,
        sha256: sha256(readFileSync(SCRIPT)),
        command:
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1055/capture-1055.mts",
        sources: pinnedSources,
    },
    artifacts: manifestArtifacts,
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`wrote ${relative(REPO, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${sha256(manifestContent)}`);
