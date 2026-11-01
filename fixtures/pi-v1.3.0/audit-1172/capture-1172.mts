#!/usr/bin/env tsx
/**
 * Frozen public Harness observations for #1172: durable codemode output
 * budgets, spill files and environment-saved script images.
 *
 * Every scenario drives the public Harness and the public execution
 * environment: a faux model issues one `codemode` call, the durable codemode
 * extension runs it, and the recorded tool result is the observation. The
 * `output.txt` spill and image files are read back and recorded, so a later
 * C++ test compares the visible result, the details and the file bytes against
 * this capture rather than against the C++ formatter.
 *
 * The script refuses to write evidence unless the private checkout is the
 * exact baseline revision and its tracked source is clean, then re-executes
 * itself under that checkout's tsx so the recorded runtime belongs to the
 * install that ran the capture.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1172-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1172-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1172/capture-1172.mts
 */
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const script = fileURLToPath(import.meta.url);
const repository = path.resolve(path.dirname(script), "../../..");
const family = "audit-1172";
const ticket = "1172";
const baseline = process.env.PI_BASELINE ?? "pi-v1.3.0";
const checkout = process.env.PI_CHECKOUT;
if (checkout === undefined || checkout === "") {
    console.error("PI_CHECKOUT must name the private frozen pi v1.3.0 checkout");
    process.exit(1);
}
const tsx = path.join(checkout, "node_modules/.bin/tsx");
if (!existsSync(tsx)) {
    console.error(`the checkout has no tsx: ${tsx}`);
    process.exit(1);
}

function run(command: string, arguments_: string[], cwd = repository): string {
    return execFileSync(command, arguments_, { cwd, encoding: "utf8" }).trim();
}

function sha256(content: Buffer | string): string {
    return createHash("sha256").update(content).digest("hex");
}

if (process.env.AUDIT_1172_TSX === undefined) {
    execFileSync(tsx, [script, ...process.argv.slice(2)], {
        cwd: repository,
        env: {
            ...process.env,
            AUDIT_1172_TSX: "1",
            NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
            TSX_TSCONFIG_PATH: path.join(checkout, "tsconfig.json"),
        },
        stdio: "inherit",
    });
    process.exit(0);
}

const registryPath = path.join(repository, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { revision?: string; bundles?: Record<string, unknown> }>;
};
const revision = registry.baselines[baseline]?.revision;
if (revision === undefined || !/^[0-9a-f]{40}$/.test(revision)) {
    console.error(`baseline ${baseline} does not resolve to a full revision`);
    process.exit(1);
}
if (registry.baselines[baseline]?.bundles?.[family] !== undefined) {
    console.error(`${family} is already registered; a deliberate recapture needs an explicit registry replacement`);
    process.exit(1);
}
const head = run("git", ["rev-parse", "HEAD"], checkout);
if (head !== revision) {
    console.error(`checkout revision ${head} is not ${revision}`);
    process.exit(1);
}
const dirty = run("git", ["status", "--porcelain"], checkout);
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

const pi = await import("../audit-1056/pi.mts");
const support = await import("../audit-1056/support.mts");
const { openChat, closeChat, entriesOf, answer, toolCall, toolResults } = support;
const { MemoryStorage, NodeExecutionEnv, createCodemodeExtension, CodemodeSandbox } = pi;
const png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a8kUAAAAASUVORK5CYII=";
const outputDirectory = /\/tmp\/pi-codemode-[A-Za-z0-9]+/g;
const normalizations = [
    "the wall time is recorded as Wall time <seconds>",
    "the run's temporary output directory is recorded as <output-dir>",
    "every run of 100 or more newlines is recorded as <newlines:N>",
];
const normalize = (text: string) =>
    text
        .replace(/Wall time [\d.]+ seconds/, "Wall time <seconds>")
        .replace(outputDirectory, "<output-dir>")
        .replace(/\n{100,}/g, (run) => `<newlines:${run.length}>`);
const callsView = (calls) =>
    calls.map((call) => ({
        id: call.id,
        name: call.name,
        args: call.args,
        status: call.status,
        durationPresent: typeof call.durationMs === "number",
    }));

function view(result) {
    const textParts = result.content.filter((block) => block.type === "text").map((block) => normalize(block.text));
    return {
        isError: result.isError,
        content: result.content.map((block) =>
            block.type === "text"
                ? { type: "text", text: normalize(block.text) }
                : { type: "image", mimeType: block.mimeType, data: block.data },
        ),
        text: textParts.join(""),
        details: {
            calls: callsView(result.details?.calls ?? []),
            fullOutputPath: result.details?.fullOutputPath ? normalize(result.details.fullOutputPath) : null,
        },
    };
}

async function runScenario(source: string, options: { readonly env?: boolean; readonly maxTokens?: number } = {}) {
    const workspace = mkdtempSync(path.join(tmpdir(), "cch-1172-frozen-"));
    const chat = await openChat(
        new MemoryStorage(),
        options.env === true ? { env: () => new NodeExecutionEnv({ cwd: workspace }) } : {},
    );
    chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
    const sourceText =
        options.maxTokens === undefined ? source : `// @options: {"max_output_tokens": ${options.maxTokens}}\n${source}`;
    chat.faux.setResponses([toolCall("codemode", { code: sourceText }), answer("done")]);
    await (await chat.root.submit({ type: "input", content: "output" }, pi.BACKGROUND_CONTEXT)).wait(
        pi.BACKGROUND_CONTEXT,
    );
    const result = toolResults(await entriesOf(chat.root))[0];
    const observed = view(result);
    let spill = null;
    if (result.details?.fullOutputPath) {
        const bytes = readFileSync(result.details.fullOutputPath);
        spill = { text: normalize(bytes.toString("utf8")), bytes: bytes.length, sha256: sha256(bytes) };
    }
    const images = [];
    const seen = new Set();
    const rawText = result.content
        .filter((block) => block.type === "text")
        .map((block) => block.text)
        .join("");
    for (const match of rawText.matchAll(/\[Image saved to (.+?) \(/g)) {
        const found = match[1];
        if (seen.has(found)) continue;
        seen.add(found);
        const bytes = readFileSync(found);
        images.push({ name: path.basename(found), bytes: bytes.length, sha256: sha256(bytes) });
    }
    await closeChat(chat);
    rmSync(workspace, { recursive: true, force: true });
    return { expected: { ...observed, spill, images } };
}

const scenarios = [];
async function scenario(name: string, inputs: Record<string, unknown>, source: string, options = {}) {
    const observed = await runScenario(source, options);
    const text = observed.expected.text;
    if (String(inputs.source).includes("repeat(99900)")) {
        if (!text.includes("Warning: truncated output")) throw new Error(`${name}: frozen pi did not truncate`);
        if (observed.expected.details.fullOutputPath === null) throw new Error(`${name}: no complete-output path`);
        if (observed.expected.spill === null) throw new Error(`${name}: no complete-output file`);
    }
    if (String(inputs.source).includes("repeat(99899)")) {
        if (text.includes("Warning: truncated output")) throw new Error(`${name}: frozen pi truncated at the limit`);
        if (observed.expected.details.fullOutputPath !== null) throw new Error(`${name}: wrote a spill at the limit`);
    }
    scenarios.push({ name, inputs, ...observed, normalizations });
}

await scenario(
    "token budget truncates output and writes the complete text",
    { source: "text('x'.repeat(120)); return 'done'", maxTokens: 20, environment: true },
    "text('x'.repeat(120)); return 'done'",
    { env: true, maxTokens: 20 },
);
await scenario(
    "text exactly at the token budget is not truncated",
    { source: "text('y'.repeat(80))", maxTokens: 20, environment: false },
    "text('y'.repeat(80))",
    { maxTokens: 20 },
);
await scenario(
    "a script image is saved through the execution environment",
    { source: `image('data:image/png;base64,${png}'); return 'ok'`, environment: true },
    `image('data:image/png;base64,${png}'); return 'ok'`,
    { env: true },
);
await scenario(
    "no execution environment keeps the frozen unsupported labels",
    { source: `text('z'.repeat(120)); image('data:image/png;base64,${png}')`, maxTokens: 20, environment: false },
    `text('z'.repeat(120)); image('data:image/png;base64,${png}')`,
    { maxTokens: 20 },
);
await scenario(
    "a failed script keeps partial output and spills it",
    { source: "text('f'.repeat(100)); throw new Error('boom')", maxTokens: 10, environment: true },
    "text('f'.repeat(100)); throw new Error('boom')",
    { env: true, maxTokens: 10 },
);
await scenario(
    "unicode truncation keeps whole BMP characters",
    { source: "text('é'.repeat(60))", maxTokens: 10, environment: true },
    "text('é'.repeat(60))",
    { env: true, maxTokens: 10 },
);
await scenario(
    "ordered text console images and values keep production order",
    {
        source: `text('alpha'); image('data:image/png;base64,${png}'); console.log('console'); return 'omega'`,
        environment: true,
    },
    `text('alpha'); image('data:image/png;base64,${png}'); console.log('console'); return 'omega'`,
    { env: true },
);
await scenario(
    "one character over the token budget truncates",
    { source: "text('y'.repeat(81))", maxTokens: 20, environment: true },
    "text('y'.repeat(81))",
    { env: true, maxTokens: 20 },
);
await scenario(
    "the line budget exactly at its limit is not truncated",
    { source: "text('\\n'.repeat(99899))", maxTokens: 25000, environment: false },
    "text('\\n'.repeat(99899))",
    { maxTokens: 25000 },
);
await scenario(
    "one line over the line budget truncates and spills",
    { source: "text('\\n'.repeat(99900))", maxTokens: 25000, environment: true },
    "text('\\n'.repeat(99900))",
    { env: true, maxTokens: 25000 },
);
await scenario(
    "a token budget larger than the text still clamps the tail to the whole line window",
    { source: "text('\\n'.repeat(99900))", maxTokens: 300000, environment: true },
    "text('\\n'.repeat(99900))",
    { env: true, maxTokens: 300000 },
);

const capturedAt = new Date().toISOString();
const environment = {
    platform: "linux",
    arch: "x64",
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: run(tsx, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim(),
};
const sourceEndpoints = ["packages/durable/src/codemode/output.ts:formatResult", "packages/durable/src/codemode/codemode.ts:CodemodeCall"];
const bundle = path.join(repository, "fixtures/pi-v1.3.0", family);
mkdirSync(bundle, { recursive: true });
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
const artifactName = "codemode-output.json";
const artifactContent = `${JSON.stringify(artifact, null, 2)}\n`;
writeFileSync(path.join(bundle, artifactName), artifactContent, "utf8");

const generatorPath = "fixtures/pi-v1.3.0/audit-1172/capture-1172.mts";
const sourceFiles = [
    generatorPath,
    "fixtures/pi-v1.3.0/audit-1056/pi.mts",
    "fixtures/pi-v1.3.0/audit-1056/support.mts",
];
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline,
    revision,
    ticket,
    family,
    capturedAt,
    environment,
    generator: {
        path: generatorPath,
        sha256: sha256(readFileSync(path.join(repository, generatorPath))),
        command:
            `PI_CHECKOUT=${checkout} PI_BASELINE=${baseline} ` +
            `${path.join(checkout, "node_modules/.bin/tsx")} ${generatorPath}`,
        sources: sourceFiles.map((source) => ({ path: source, sha256: sha256(readFileSync(path.join(repository, source))) })),
    },
    artifacts: [
        {
            path: artifactName,
            family,
            bytes: Buffer.byteLength(artifactContent),
            sha256: sha256(artifactContent),
            sourceEndpoints,
            scenarios,
        },
    ],
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(path.join(bundle, "manifest.json"), manifestContent, "utf8");
console.log(`${artifactName}: ${scenarios.length} scenario(s), ${Buffer.byteLength(artifactContent)} bytes`);
console.log(`manifest.json: ${Buffer.byteLength(manifestContent)} bytes, sha256 ${sha256(manifestContent)}`);
