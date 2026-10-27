/**
 * `audit-1151` capture: the frozen prompt planner's remove/re-add order branches, driven by a real
 * generation sequence whose registry section (and tool) order changes between turns. The script
 * refuses to write evidence unless the checkout is the exact baseline revision and its tracked
 * source is clean.
 *
 * Run from the Pike repository:
 *   NODE_OPTIONS=--conditions=source PI_CHECKOUT=/tmp/cch-1080-pi-frozen \
 *     /tmp/cch-1080-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1151/capture-1151.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const family = "audit-1151";
const ticket = "1151";
const baseline = process.env.PI_BASELINE ?? "pi-v1.3.0";
const expectedRevision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const checkout = process.env.PI_CHECKOUT;

if (!checkout) {
    console.error("PI_CHECKOUT must name the private frozen pi v1.3.0 checkout");
    process.exit(1);
}
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: checkout, encoding: "utf8" }).trim();
if (revision !== expectedRevision) {
    console.error(`checkout revision ${revision} is not ${expectedRevision}`);
    process.exit(1);
}
const dirty = execFileSync("git", ["status", "--porcelain"], { cwd: checkout, encoding: "utf8" }).trim();
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
const tsx = join(checkout, "node_modules/.bin/tsx");
if (!existsSync(tsx)) {
    console.error(`the checkout has no tsx: ${tsx}`);
    process.exit(1);
}
const registryPath = join(repo, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { bundles?: Record<string, { manifest?: string }> }>;
};
if (registry.baselines[baseline]?.bundles?.[family]) {
    console.error(`${family} is already registered; a deliberate recapture must unregister it first`);
    process.exit(1);
}

const load = async (path: string) => import(new URL(`file://${join(checkout, path)}`).href);
const { durable, ai } = await import("./pi.mts");
const { chatSetup, openChat, allEntries } = await load("packages/durable/test/chat-support.ts");
const { addSection, addTool, tool } = await load("packages/durable/test/harness-support.ts");
const { context } = await load("packages/durable/test/session-support.ts");
const { getCurrentSystemMessage, getCurrentTools } = await load("packages/ai/src/utils/transcript.ts");
const { getSystemMessageText } = await load("packages/ai/src/utils/text.ts");

const tsxVersion = execFileSync(tsx, ["--version"], { encoding: "utf8" }).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const capturedAt = new Date().toISOString();
const artifactDirectory = join(repo, "fixtures/pi-v1.3.0", family);
mkdirSync(artifactDirectory, { recursive: true });

function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

const textOf = (content: unknown): string => {
    if (typeof content === "string") return content;
    if (!Array.isArray(content)) return "";
    return content.flatMap((block: any) => (block?.type === "text" ? [block.text] : [])).join("\n");
};

/** One transcript message without volatile timestamps. */
function normalizeMessage(message: any): unknown {
    if (message.role === "system") {
        return {
            role: "system",
            sections: Object.entries(message.sections ?? {}).map(([key, value]) => [key, value]),
            toolsRemoved: (message.toolsRemoved ?? []).map((entry: any) => entry.name),
            toolsAdded: (message.toolsAdded ?? []).map((entry: any) => entry.name),
        };
    }
    return { role: message.role, text: textOf(message.content) };
}

/** The effective prompt and tools the model sees after replaying the request's system messages. */
function effectiveRequest(messages: readonly any[]) {
    const current = getCurrentSystemMessage(messages);
    return {
        systemPrompt: current === undefined ? "" : getSystemMessageText(current),
        tools: getCurrentTools(messages).map((entry: any) => entry.name),
        messages: messages.filter((message: any) => message.role !== "system").map(normalizeMessage),
    };
}

/** The `pi.system` entries in order, with sections/tool deltas and no timestamps. */
async function systemEntries(root: any) {
    const entries = await allEntries(root);
    return entries
        .filter((entry: any) => entry.kind === "pi.system")
        .map((entry: any) => {
            const message = entry.model[0];
            return {
                sections: Object.entries(message.sections ?? {}).map(([key, value]) => [key, value]),
                toolsRemoved: (message.toolsRemoved ?? []).map((reference: any) => reference.name),
                toolsAdded: (message.toolsAdded ?? []).map((declaration: any) => declaration.name),
            };
        });
}

interface Observation {
    systemEntries: unknown;
    requests: unknown;
}

async function observe(root: any, requests: readonly any[]): Promise<Observation> {
    return {
        systemEntries: await systemEntries(root),
        requests: requests.map((request) => effectiveRequest(request.messages)),
    };
}

/** A chat whose faux responses record every request before answering. */
async function open(requests: any[], configure: (registry: any) => void) {
    const setup = chatSetup({ models: [{ id: "faux-1", contextWindow: 100000, maxTokens: 900 }] });
    configure(setup.registry);
    setup.faux.setResponses(
        Array.from({ length: 16 }, () => (transcript: any) => {
            requests.push(JSON.parse(JSON.stringify({ messages: transcript.messages })));
            return ai.fauxAssistantMessage("ok");
        }),
    );
    const { harness, root } = await openChat(new durable.MemoryStorage(), setup);
    harness.resume();
    return { setup, harness, root };
}

async function submit(root: any, text: string) {
    const submission = await root.submit({ type: "input", content: text }, context);
    const outcome = await submission.wait(context);
    assert(outcome.status === "done", `generation ${text} did not finish: ${outcome.status}`);
}

const scenarios: any[] = [];
const normalizations = [
    "timestamps, task and entry identifiers are omitted",
    "assistant text is abbreviated to the scripted answer",
    "sections are recorded as [name, value] pairs so object member order is explicit",
];

// Scenario 1: the registry's section order changes between turns. Frozen emits the first
// baseline, then a remove-all entry and a re-add-all entry, then nothing.
{
    const requests: any[] = [];
    let installed: any[] = [];
    const { setup, harness, root } = await open(requests, (registry: any) => {
        installed = [
            addSection(registry, "a", () => "1", { tag: false }),
            addSection(registry, "b", () => "2", { tag: false }),
        ];
    });
    await submit(root, "u1");
    const first = await observe(root, requests);
    for (const handle of installed) handle.dispose();
    addSection(setup.registry, "b", () => "2", { tag: false });
    addSection(setup.registry, "a", () => "1", { tag: false });
    await submit(root, "u2");
    const second = await observe(root, requests);
    await submit(root, "u3");
    const third = await observe(root, requests);
    const expected = {
        first,
        second,
        third,
        systemEntryCount: (third.systemEntries as any[]).length,
    };
    assert(expected.systemEntryCount === 3, `expected three pi.system entries, saw ${expected.systemEntryCount}`);
    assert(
        JSON.stringify((second.systemEntries as any[])[1]) ===
            JSON.stringify({ sections: [["a", null], ["b", null]], toolsRemoved: [], toolsAdded: [] }),
        `expected the remove-all entry, saw ${JSON.stringify((second.systemEntries as any[])[1])}`,
    );
    assert(
        JSON.stringify((second.systemEntries as any[])[2]) ===
            JSON.stringify({ sections: [["b", "2"], ["a", "1"]], toolsRemoved: [], toolsAdded: [] }),
        `expected the re-add entry, saw ${JSON.stringify((second.systemEntries as any[])[2])}`,
    );
    assert((third.systemEntries as any[]).length === 3, "the third generation must add no pi.system entry");
    await harness.close(context);
    scenarios.push({
        name: "reordered sections are removed and re-added in the desired order",
        inputs: { sectionsPerTurn: [["a", "b"], ["b", "a"], ["b", "a"]], turns: ["u1", "u2", "u3"] },
        expected,
        normalizations,
    });
}

// Scenario 2: the registry's tool order changes between turns. Frozen removes every offered
// tool and re-adds every desired tool in one entry, then nothing.
{
    const requests: any[] = [];
    let installed: any[] = [];
    const { setup, harness, root } = await open(requests, (registry: any) => {
        installed = [addTool(registry, tool("x")), addTool(registry, tool("y"))];
    });
    await submit(root, "u1");
    const first = await observe(root, requests);
    for (const handle of installed) handle.dispose();
    addTool(setup.registry, tool("y"));
    addTool(setup.registry, tool("x"));
    await submit(root, "u2");
    const second = await observe(root, requests);
    await submit(root, "u3");
    const third = await observe(root, requests);
    const expected = {
        first,
        second,
        third,
        systemEntryCount: (third.systemEntries as any[]).length,
    };
    assert(expected.systemEntryCount === 2, `expected two pi.system entries, saw ${expected.systemEntryCount}`);
    assert(
        JSON.stringify((second.systemEntries as any[])[1]) ===
            JSON.stringify({ sections: [], toolsRemoved: ["x", "y"], toolsAdded: ["y", "x"] }),
        `expected the tool remove/re-add entry, saw ${JSON.stringify((second.systemEntries as any[])[1])}`,
    );
    assert((third.systemEntries as any[]).length === 2, "the third generation must add no pi.system entry");
    await harness.close(context);
    scenarios.push({
        name: "reordered tools are removed and re-added in the desired order",
        inputs: { toolsPerTurn: [["x", "y"], ["y", "x"], ["y", "x"]], turns: ["u1", "u2", "u3"] },
        expected,
        normalizations,
    });
}

// Scenario 3: both orders change between turns. The tool changes ride on the last planned
// section entry, so the remove-all section entry carries no tool deltas.
{
    const requests: any[] = [];
    let installed: any[] = [];
    const { setup, harness, root } = await open(requests, (registry: any) => {
        installed = [
            addSection(registry, "a", () => "1", { tag: false }),
            addSection(registry, "b", () => "2", { tag: false }),
            addTool(registry, tool("x")),
            addTool(registry, tool("y")),
        ];
    });
    await submit(root, "u1");
    const first = await observe(root, requests);
    for (const handle of installed) handle.dispose();
    addSection(setup.registry, "b", () => "2", { tag: false });
    addSection(setup.registry, "a", () => "1", { tag: false });
    addTool(setup.registry, tool("y"));
    addTool(setup.registry, tool("x"));
    await submit(root, "u2");
    const second = await observe(root, requests);
    await submit(root, "u3");
    const third = await observe(root, requests);
    const expected = {
        first,
        second,
        third,
        systemEntryCount: (third.systemEntries as any[]).length,
    };
    assert(expected.systemEntryCount === 3, `expected three pi.system entries, saw ${expected.systemEntryCount}`);
    assert(
        JSON.stringify((second.systemEntries as any[])[1]) ===
            JSON.stringify({ sections: [["a", null], ["b", null]], toolsRemoved: [], toolsAdded: [] }),
        `expected the bare remove-all entry, saw ${JSON.stringify((second.systemEntries as any[])[1])}`,
    );
    assert(
        JSON.stringify((second.systemEntries as any[])[2]) ===
            JSON.stringify({ sections: [["b", "2"], ["a", "1"]], toolsRemoved: ["x", "y"], toolsAdded: ["y", "x"] }),
        `expected tool deltas on the re-add entry, saw ${JSON.stringify((second.systemEntries as any[])[2])}`,
    );
    assert((third.systemEntries as any[]).length === 3, "the third generation must add no pi.system entry");
    await harness.close(context);
    scenarios.push({
        name: "section and tool reorders share the last planned entry",
        inputs: {
            sectionsPerTurn: [["a", "b"], ["b", "a"], ["b", "a"]],
            toolsPerTurn: [["x", "y"], ["y", "x"], ["y", "x"]],
            turns: ["u1", "u2", "u3"],
        },
        expected,
        normalizations,
    });
}

// Scenario 4: the minimal patch branch. A changed section value and a removed section are
// patched in place; a changed tool declaration, a removal and an addition ride on the same
// entry. No remove-all/re-add-all pair is emitted.
{
    const requests: any[] = [];
    let installed: any[] = [];
    const { setup, harness, root } = await open(requests, (registry: any) => {
        installed = [
            addSection(registry, "a", () => "1", { tag: false }),
            addSection(registry, "b", () => "2", { tag: false }),
            addTool(registry, tool("x")),
            addTool(registry, tool("y")),
        ];
    });
    await submit(root, "u1");
    const first = await observe(root, requests);
    for (const handle of installed) handle.dispose();
    addSection(setup.registry, "a", () => "one", { tag: false });
    const changed = tool("x");
    changed.description = "x changed";
    addTool(setup.registry, changed);
    addTool(setup.registry, tool("z"));
    await submit(root, "u2");
    const second = await observe(root, requests);
    await submit(root, "u3");
    const third = await observe(root, requests);
    const expected = {
        first,
        second,
        third,
        systemEntryCount: (third.systemEntries as any[]).length,
    };
    assert(expected.systemEntryCount === 2, `expected two pi.system entries, saw ${expected.systemEntryCount}`);
    assert(
        JSON.stringify((second.systemEntries as any[])[1]) ===
            JSON.stringify({
                sections: [["a", "one"], ["b", null]],
                toolsRemoved: ["x", "y"],
                toolsAdded: ["x", "z"],
            }),
        `expected the minimal patch entry, saw ${JSON.stringify((second.systemEntries as any[])[1])}`,
    );
    assert((third.systemEntries as any[]).length === 2, "the third generation must add no pi.system entry");
    await harness.close(context);
    scenarios.push({
        name: "changed and removed sections and tools produce one minimal patch",
        inputs: {
            sectionsPerTurn: [["a", "b"], ["a"]],
            sectionValuesPerTurn: [{ a: "1", b: "2" }, { a: "one" }],
            toolsPerTurn: [["x", "y"], ["x", "z"]],
            turns: ["u1", "u2", "u3"],
        },
        expected,
        normalizations,
    });
}

const sourceEndpoints = [
    "packages/durable/src/harness/prompt.ts:planSystemEntries",
    "packages/durable/src/harness/prompt.ts:planSections",
    "packages/durable/src/harness/prompt.ts:planTools",
    "packages/durable/src/harness/generation.ts:GenerationTask.prepare,request",
    "packages/ai/src/utils/transcript.ts:getCurrentSystemMessage,getCurrentTools",
];
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};
const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline,
    revision,
    ticket,
    family,
    capturedAt,
    environment,
    sourceEndpoints,
    scenarios,
};
const artifactName = "prompt-plan.json";
const artifactContent = `${JSON.stringify(artifact, null, 2)}\n`;
writeFileSync(join(artifactDirectory, artifactName), artifactContent);

const pinnedSources = [script, join(artifactDirectory, "pi.mts")].map((path) => ({
    path: relative(repo, path),
    sha256: createHash("sha256").update(readFileSync(path)).digest("hex"),
}));
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline,
    revision,
    ticket,
    family,
    capturedAt,
    environment,
    generator: {
        path: relative(repo, script),
        sha256: createHash("sha256").update(readFileSync(script)).digest("hex"),
        command:
            "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1151/capture-1151.mts",
        sources: pinnedSources,
    },
    artifacts: [
        {
            path: artifactName,
            family,
            bytes: Buffer.byteLength(artifactContent),
            sha256: createHash("sha256").update(artifactContent).digest("hex"),
            sourceEndpoints,
            scenarios,
        },
    ],
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(artifactDirectory, "manifest.json"), manifestContent);
console.log(`captured ${artifactName} (${scenarios.length} scenarios)`);
console.log(`wrote ${relative(repo, join(artifactDirectory, "manifest.json"))}`);
console.log(`manifest sha256 ${createHash("sha256").update(manifestContent).digest("hex")}`);
