/**
 * The killed half of the `audit-1055` crash/reopen scenarios.
 *
 * Each mode opens the SQLite store named on the command line, reaches one named durable boundary and
 * writes `READY\n` to stdout. The parent capture then SIGKILLs this process. The boundary is always
 * after a committed record and before the next durable commit, so a reopened Harness must recover
 * rather than guess.
 */

import { readFileSync, writeFileSync } from "node:fs";
import { BACKGROUND_CONTEXT } from "./pi.mts";
import {
    abortCrashTask,
    oneShotTask,
    openChat,
    openHarness,
    openSqlite,
    transferTask,
} from "./audit-tasks.mts";

const [mode, database] = process.argv.slice(2);
if (mode === undefined || database === undefined) throw new Error("usage: crash-worker.mts <mode> <sqlite-path>");

function ready(payload: unknown): void {
    writeFileSync(`${database}.ready`, JSON.stringify(payload));
    process.stdout.write("READY\n");
}

/** Keep the event loop alive after the boundary; the parent kills this process. */
async function block(): Promise<never> {
    setInterval(() => {}, 1_000);
    return await new Promise<never>(() => {});
}

switch (mode) {
    case "generation": {
        const { harness, root, faux } = await openChat(await openSqlite(database!));
        let reached = false;
        faux.setResponses([
            async (_request: unknown, options: any) => {
                reached = true;
                return await new Promise((_, reject) =>
                    options!.signal!.addEventListener("abort", () => reject(options!.signal!.reason), { once: true }),
                );
            },
        ]);
        const submission = await root.submit(
            { type: "input", content: "crash input", requestId: "crash-1" },
            BACKGROUND_CONTEXT,
        );
        while (!reached) await new Promise((resolve) => setTimeout(resolve, 2));
        ready({ submissionId: submission.id, conversationId: root.id });
        await block();
        break;
    }
    case "effect": {
        const effectPath = `${database}.effect.json`;
        writeFileSync(effectPath, JSON.stringify({ calls: 0, applied: {} }));
        const effect = async (key: string, amount: number) => {
            const state = JSON.parse(readFileSync(effectPath, "utf8"));
            state.calls++;
            if (!(key in state.applied)) state.applied[key] = amount * 10;
            writeFileSync(effectPath, JSON.stringify(state));
            return state.applied[key];
        };
        const Transfer = transferTask(effect, async (task) => {
            ready({ taskId: task.id, key: task.state.checkpoint.key, receipt: task.input.amount * 10 });
            await block();
        });
        const opened = await openHarness(await openSqlite(database), [Transfer]);
        const harness = opened.harness;
        const root = await harness.root(BACKGROUND_CONTEXT);
        await root.commit(
            (tx: any) => tx.createTask(Transfer, { amount: 7 }, { ownership: { kind: "conversation" } }),
            BACKGROUND_CONTEXT,
        );
        harness.resume();
        await block();
        break;
    }
    case "abort": {
        const log: string[] = [];
        const Abortable = abortCrashTask(log, async () => {
            log.push("abort");
            ready({ log });
            await block();
        });
        const opened = await openHarness(await openSqlite(database), [Abortable]);
        const harness = opened.harness;
        const root = await harness.root(BACKGROUND_CONTEXT);
        const id = await root.commit(
            (tx: any) => tx.createTask(Abortable, null, { ownership: { kind: "conversation" } }),
            BACKGROUND_CONTEXT,
        );
        harness.resume();
        while (!log.includes("run")) await new Promise((resolve) => setTimeout(resolve, 2));
        void harness.abortTask(id, BACKGROUND_CONTEXT).catch(() => {});
        await block();
        break;
    }
    case "terminal": {
        const log: string[] = [];
        const Once = oneShotTask("audit.terminal", "once", log);
        const opened = await openHarness(await openSqlite(database), [Once]);
        const harness = opened.harness;
        const root = await harness.root(BACKGROUND_CONTEXT);
        const id = await root.commit(
            (tx: any) => tx.createTask(Once, null, { ownership: { kind: "conversation" } }),
            BACKGROUND_CONTEXT,
        );
        const receipt = await harness.waitForTask(id, BACKGROUND_CONTEXT);
        const submission = await root.submit(
            { type: "write", entry: { kind: "audit.terminal.note", data: { done: true } }, requestId: "terminal-1" },
            BACKGROUND_CONTEXT,
        );
        await submission.wait(BACKGROUND_CONTEXT);
        ready({ taskId: id, submissionId: submission.id, log, outcome: receipt.state.outcome });
        await block();
        break;
    }
    default:
        throw new Error(`unknown worker mode ${mode}`);
}
