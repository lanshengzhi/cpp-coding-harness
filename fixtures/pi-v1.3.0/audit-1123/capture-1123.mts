#!/usr/bin/env tsx
/**
 * Capture the `audit-1123` named-session program-status family against frozen `pi-v1.3.0` (#1123).
 *
 * HONEST SCOPE: this is a SOURCE PROBE, not a product-level capture. The frozen
 * `ProgramStatusReporter` is driven directly with a recording Terminal stub and a local
 * variable standing in for pi's `getSessionName`; no model turn, no gateway, no network, and no
 * interactive session is involved. What it freezes is the reporter's WIRE OUTPUT for a named
 * session (working/done carry the name as `msg`, a rename re-reports, the error/blocked/compaction
 * reports do not), which is exactly the slice pike's `ProgramStatusReporter` had to reproduce.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout at 71023aec1546> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1123/capture-1123.mts
 */

import { spawnSync } from "node:child_process";
import { writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1123";
const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi"));
const baselineName = process.env.PI_BASELINE ?? "pi-v1.3.0";
const baselineRevision = "71023aec1546d6d677a65669c5665c0927ce0dd0";

if (process.env.TSX_TSCONFIG_PATH === undefined) {
    const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
    const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
        stdio: "inherit",
        env: { ...process.env, TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json") },
    });
    if (result.error) {
        throw new Error(`cannot re-exec ${tsx} to resolve the pi checkout's workspace paths: ${result.error}`);
    }
    process.exit(result.status ?? 1);
}

const head = spawnSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).stdout.trim();
if (head !== baselineRevision) {
    throw new Error(`pi checkout must be at ${baselineRevision} (baseline ${baselineName}), found ${head}`);
}

const { ProgramStatusReporter } = await import(
    pathToFileURL(path.join(piCheckout, "packages/coding-agent/src/modes/interactive/program-status-reporter.ts")).href
);
const { formatProgramStatus } = await import(
    pathToFileURL(path.join(piCheckout, "packages/tui/src/program-status.ts")).href
);

const writes: string[] = [];
const terminal = { setProgramStatus(status: unknown) { writes.push(formatProgramStatus(status as never)); } };
let name: string | undefined = "review-session";
const reporter = new ProgramStatusReporter(() => terminal as never, () => name);
const steps: { label: string; wire: string | null }[] = [];
function record(label: string) {
    steps.push({ label, wire: writes.at(-1) ?? null });
    writes.length = 0;
}
function event(value: unknown) { reporter.handleEvent(value as never); }

event({ type: "agent_start" });
record("named_agent_start_working");
event({ type: "message_end", message: { role: "assistant", stopReason: "stop" } });
record("named_message_end_working_unchanged");
event({ type: "agent_settled", aborted: false });
record("named_agent_settled_done");
name = "renamed-session";
event({ type: "session_info_changed" });
record("named_rename_after_done");
event({ type: "agent_start" });
record("named_second_agent_start_working");
name = "renamed-while-working";
event({ type: "session_info_changed" });
record("named_rename_while_working");
event({ type: "compaction_start" });
record("named_compaction_start_override");
event({ type: "compaction_end", aborted: false, reason: "auto" });
record("named_compaction_end_working_name");
event({ type: "message_end", message: { role: "assistant", stopReason: "error", errorMessage: "Invalid API key\n{details}" } });
record("named_message_end_error_while_active");
event({ type: "agent_settled", aborted: false });
record("named_settled_error");
name = undefined;
event({ type: "session_info_changed" });
record("unnamed_rename_after_error_no_message");
event({ type: "agent_start" });
event({ type: "message_end", message: { role: "assistant", stopReason: "stop" } });
event({ type: "agent_settled", aborted: false });
record("unnamed_done_no_message");
reporter.setBlocked("trust", { kind: "question", message: "Trust project folder?" });
record("blocked_question_named_absent");
reporter.setBlocked("trust", undefined);
reporter.reset();
record("reset_idle");

const capturedAt = new Date().toISOString();
const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    recordKind: "source-probe",
    baseline: baselineName,
    revision: baselineRevision,
    capturedAt,
    environment: {
        platform: "linux",
        arch: "x64",
        libc: "glibc",
        glibcVersion: "2.44",
        node: "v26.12.0",
        runner: "node_modules/.bin/tsx (pi checkout)",
        network: "none (reporter is pure; no model, no gateway)",
    },
    sourceEndpoints: [
        "packages/coding-agent/src/modes/interactive/program-status-reporter.ts:constructor(getTerminal,getSessionName),handleEvent,setBlocked,reset,currentStatus",
        "packages/coding-agent/src/modes/interactive/interactive-mode.ts:551-554 (getSessionName = sessionManager.getSessionName)",
        "packages/tui/src/program-status.ts:formatProgramStatus",
    ],
    probe: "ProgramStatusReporter driven directly with a recording Terminal stub; session name supplied by a local variable (pi getSessionName contract), session_info_changed emitted on rename, no model turn",
    scenarios: [
        {
            name: "named-session-reports-working-and-done-message",
            inputs: {
                initialSessionName: "review-session",
                renameAfterDone: "renamed-session",
                renameWhileWorking: "renamed-while-working",
            },
            observedSteps: steps,
            expected: {
                namedWorking: "\u001b]7501;state=working:app=pi:msg=cmV2aWV3LXNlc3Npb24=\u001b\\",
                namedDone: "\u001b]7501;state=done:app=pi:msg=cmV2aWV3LXNlc3Npb24=\u001b\\",
                renameAfterDone: "\u001b]7501;state=done:app=pi:msg=cmVuYW1lZC1zZXNzaW9u\u001b\\",
                renameWhileWorking: "\u001b]7501;state=working:app=pi:msg=cmVuYW1lZC13aGlsZS13b3JraW5n\u001b\\",
                compactionOverride: "\u001b]7501;state=working:app=pi:msg=Q29tcGFjdGluZyBjb250ZXh0\u001b\\",
                settledError: "\u001b]7501;state=error:app=pi:msg=SW52YWxpZCBBUEkga2V5\u001b\\",
                unnamedDone: "\u001b]7501;state=done:app=pi\u001b\\",
                blockedQuestion: "\u001b]7501;state=blocked:app=pi:kind=question:msg=VHJ1c3QgcHJvamVjdCBmb2xkZXI/\u001b\\",
                resetIdle: "\u001b]7501;state=idle:app=pi\u001b\\",
                messageStates: ["working", "done"],
            },
        },
    ],
};

const target = path.join(familyDir, "named-session-program-status.json");
writeFileSync(target, `${JSON.stringify(artifact, null, 1)}\n`, "utf8");
process.stdout.write(`captured ${family} at ${capturedAt}\n`);
process.exit(0);
