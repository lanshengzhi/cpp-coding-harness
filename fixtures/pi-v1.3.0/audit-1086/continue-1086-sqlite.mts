// Frozen pi public Harness continuation of a C++-created SQLite store, and
// the pi side of the pi -> C++ -> pi continuation test (#1086).
import { join } from "node:path";
import { execFileSync } from "node:child_process";
import { writeFileSync } from "node:fs";
const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT must identify the frozen checkout");
const git = (args: string[]) => execFileSync("git", ["-C", checkout, ...args], { encoding: "utf8" }).trim();
if (git(["rev-parse", "HEAD"]) !== "71023aec1546d6d677a65669c5665c0927ce0dd0" || git(["status", "--porcelain"]) !== "")
    throw new Error("pi checkout must be clean at the frozen revision");
const directory = process.argv[2];
const mode = process.argv[3] ?? "advance";
const taskId = Number(process.argv[4]);
const submissionId = Number(process.argv[5]);
const conversationId = Number(process.argv[6]);
if (!directory) throw new Error("missing SQLite directory");
const { Harness, defineTask, defineDoc, createRegistry } = await import(join(checkout, "packages/durable/src/index.ts"));
const { openNodeSqliteStorage } = await import(join(checkout, "packages/durable/src/storage/sqlite/node.ts"));
const { BACKGROUND_CONTEXT: context } = await import(join(checkout, "packages/chord/src/context/index.ts"));
const TASK_KIND = "audit1086.work\"\\é";
const DOC_KIND = "audit1086.history\"\\é";
const REQUEST_ID = "audit1086/request\"\\é";
const doc = defineDoc({ kind: DOC_KIND, version: 1, scope: "conversation",
    history: "rewindable", fork: "asOf", initial: () => ({ n: 0 }) });
let reached: () => void = () => {};
let rejectPhase: (error: unknown) => void = () => {};
const advanced = new Promise<void>((resolve) => { reached = resolve; });
const phaseFailure = new Promise<never>((_, reject) => { rejectPhase = reject; });
let advanceTimeout: ReturnType<typeof setTimeout> | undefined;
const advancedOrTimeout = Promise.race([
    advanced,
    phaseFailure,
    new Promise<void>((_, reject) => {
        advanceTimeout = setTimeout(() => reject(new Error("advance did not reach the running checkpoint")), 5000);
    }),
]);
const task = defineTask({ name: TASK_KIND, version: 1,
    initial: () => ({ phase: "start", n: 7 }),
    phases: { start: async (record: any, runtime: any, ctx: any) => {
        try {
            if (record.state.checkpoint?.phase !== "start" || record.state.checkpoint?.n !== 7)
                throw new Error("unexpected start checkpoint");
            const indexed = await storage.scanTasks({ kind: TASK_KIND }, 10, undefined, ctx);
            if (!indexed.items.some((item: any) => item.id === taskId))
                throw new Error("task kind index did not find the C++-created task");
            const byRequest = await storage.submissionByRequest(conversationId, REQUEST_ID, ctx);
            if (byRequest?.id !== submissionId)
                throw new Error("submission request index did not find the C++-created submission");
            const foundDoc = await storage.findDocument({ kind: DOC_KIND, scope: { kind: "conversation", conversationId } }, "current", ctx);
            if (foundDoc?.id === undefined) throw new Error("document index did not find the C++-created document");
            await runtime.commit(async (tx: any) => {
                const draft = await tx.doc(doc, record.conversationId);
                if (draft.n !== 1) throw new Error("unexpected document history");
                draft.n = 2;
                tx.settleSubmission(submissionId, { status: "unanswered", reason: "aborted" });
                return { status: "running", checkpoint: { phase: "finish", n: 8 } };
            }, ctx);
            if (mode === "advance") {
                reached();
                await runtime.sleep(Date.now() + 2000, ctx);
            }
        } catch (error) {
            rejectPhase(error);
            throw error;
        }
    }, finish: async (record: any, runtime: any, ctx: any) => {
        if (mode !== "finish" || record.state.checkpoint?.phase !== "finish" || record.state.checkpoint?.n !== 8)
            throw new Error("unexpected finish checkpoint");
        const saved = await storage.submission(submissionId, ctx);
        if (saved?.status !== "unanswered" || saved.reason !== "aborted" || saved.requestId !== REQUEST_ID)
            throw new Error("submission not settled");
        const indexed = await storage.scanTasks({ kind: TASK_KIND }, 10, undefined, ctx);
        if (!indexed.items.some((item: any) => item.id === taskId))
            throw new Error("task kind index did not find the task");
        const byRequest = await storage.submissionByRequest(conversationId, REQUEST_ID, ctx);
        if (byRequest?.id !== submissionId)
            throw new Error("submission request index did not find the submission");
        const foundDoc = await storage.findDocument({ kind: DOC_KIND, scope: { kind: "conversation", conversationId } }, "current", ctx);
        if (foundDoc?.id === undefined) throw new Error("document index did not find the document");
        await runtime.commit(async (tx: any) => {
            const draft = await tx.doc(doc, record.conversationId);
            if (draft.n !== 2) throw new Error("unexpected document history");
            draft.n = 3;
            return { status: "terminal", outcome: { status: "completed", result: 17 } };
        }, ctx);
    } }, abort: async () => {} });
const registry = createRegistry();
registry.install({ name: "audit1086", tasks: [task] });
const storage = await openNodeSqliteStorage(join(directory, "storage.sqlite"));
const harness = await Harness.open(storage, { registry }, context);
if (mode === "create") {
    const root = await harness.root(context);
    const ids = await root.commit(async (tx: any) => {
        const id = await tx.createTask(task, {}, { ownership: { kind: "conversation" } });
        const submission = await tx.createSubmission({ conversationId: root.id, requestId: REQUEST_ID, type: "input", status: "queued" });
        const draft = await tx.doc(doc, root.id);
        draft.n = 1;
        return { id, submission: submission.id, conversation: root.id };
    }, context);
    await harness.close(context);
    writeFileSync(join(directory, "pi-created.json"), JSON.stringify(ids));
    process.exit(0);
}
if (mode === "finish") {
    const saved = await storage.submission(submissionId, context);
    if (saved?.status !== "unanswered" || saved.reason !== "aborted" || saved.requestId !== REQUEST_ID)
        throw new Error("submission not settled");
}
harness.resume();
if (mode === "finish") {
    const receipt = await harness.waitForTask(taskId, context);
    if (receipt.state.outcome?.status !== "completed" || receipt.state.outcome.result !== 17)
        throw new Error("unexpected terminal receipt");
    const current = await storage.document((await storage.findDocument({ kind: DOC_KIND, scope: { kind: "conversation", conversationId } }, "current", context))!.id, "current", context);
    if (current?.value?.n !== 3) throw new Error("unexpected final document");
} else await advancedOrTimeout;
if (advanceTimeout !== undefined) clearTimeout(advanceTimeout);
await harness.close(context);
