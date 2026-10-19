#!/usr/bin/env tsx
/**
 * Capture the `audit-1050` complete Linux TUI toolkit family against frozen `pi-v1.3.0` (#1050).
 *
 * The observations are public-call/scenario results from the frozen checkout: package-entry
 * formatting/helper calls, public components, `TuiMainScreen`/`TuiAltScreen`,
 * `StdinBuffer`, `detectCapabilities` and the `ProcessTerminal` host boundary. Historical
 * `pi-v1.0.4` scenario families (hardware cursor, input, selection) are re-run against the new
 * baseline instead of relabelling their old observations. This is inventory evidence, not a C++
 * capability-alignment claim: the ledger records implementation and verification separately.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1050/capture-1050.mts
 */

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1050";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi"));

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

const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.schema !== "pike/pi-v1.3.0-audit-baselines/1") {
    throw new Error(`unexpected audit baseline registry schema: ${registryPath}`);
}
const baselineName = process.env.PI_BASELINE ?? registry.default;
const baseline = registry.baselines[baselineName];
if (!baseline) {
    throw new Error(
        `unknown baseline ${JSON.stringify(baselineName)}; registered baselines: ` +
            Object.keys(registry.baselines).sort().join(", "),
    );
}
const head = runGit(piCheckout, ["rev-parse", "HEAD"]);
if (head !== baseline.revision) {
    throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baselineName}), found ${head}`);
}
if (runGit(piCheckout, ["status", "--porcelain"])) {
    throw new Error("pi capture requires an unmodified source checkout");
}
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
    throw new Error("audit capture requires Linux x86-64/glibc");
}
for (const [name, entry] of Object.entries<any>(registry.baselines)) {
    if (name !== baselineName && entry.bundles && family in entry.bundles) {
        throw new Error(`refusing to capture ${family}: it already belongs to baseline ${JSON.stringify(name)}`);
    }
}

const requirePi = createRequire(path.join(piCheckout, "package.json"));
let tsxVersion = "unknown";
try {
    tsxVersion = requirePi("tsx/package.json").version;
} catch {
    // The tsx version is provenance, not a capture precondition; keep the sentinel explicit.
}

const pinnedVariables = [
    "TERM_PROGRAM",
    "TERM_PROGRAM_VERSION",
    "COLORTERM",
    "KITTY_WINDOW_ID",
    "GHOSTTY_RESOURCES_DIR",
    "WEZTERM_PANE",
    "ITERM_SESSION_ID",
    "TMUX",
    "TERMUX_VERSION",
    "PI_HYPERLINKS",
    "PI_IMAGE_PROTOCOL",
    "PI_TRUE_COLOR",
    "PI_PROGRAM_STATUS",
    "PI_TUI_WRITE_LOG",
    "PI_HARDWARE_CURSOR",
];
for (const name of pinnedVariables) delete process.env[name];
process.env.TERM = "xterm-256color";
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: glibc,
    node: process.version,
    tsx: tsxVersion,
    TERM: process.env.TERM,
    pinnedVariables,
};
const capturedAt = new Date().toISOString();

const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");
const scenarios = await import(pathToFileURL(path.join(familyDir, "scenarios-1050.mts")).href);
scenarios.pinScenarioEnvironment();

const historicalCapture = path.join(repositoryRoot, "fixtures/pi-tui/capture");
const { captureHardwareCursor } = await import(pathToFileURL(path.join(historicalCapture, "hardware-cursor-scenarios.mts")).href);
const { captureInputScenarios } = await import(pathToFileURL(path.join(historicalCapture, "input-scenarios.mts")).href);
const { captureSelection } = await import(pathToFileURL(path.join(historicalCapture, "selection-scenarios.mts")).href);

const hardwareCursor = await captureHardwareCursor(piCheckout);
const inputStandalone = await captureInputScenarios(piCheckout);
const selectionMulticlick = await captureSelection(piCheckout);
const cursorComposition = await scenarios.captureCursorComposition(piCheckout);
const paddingAndSelectionReset = await scenarios.capturePaddingAndSelectionReset(piCheckout);
const overlayRouting = await scenarios.captureOverlayRouting(piCheckout);
const keysAndTerminalReplies = await scenarios.captureKeysAndTerminalReplies(piCheckout);
const programStatusFormat = await scenarios.captureProgramStatusFormatting(piCheckout);
const terminalCapabilities = await scenarios.captureTerminalCapabilities(piCheckout);

const programStatusDetection = captureProgramStatusDetection();

function captureProgramStatusDetection() {
    const childPath = path.join(familyDir, "program-status-child.mts");
    const result = spawnSync(path.join(piCheckout, "node_modules", ".bin", "tsx"), [childPath], {
        cwd: repositoryRoot,
        encoding: "utf8",
        env: {
            ...process.env,
            PI_CHECKOUT: piCheckout,
            TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json"),
        },
    });
    if (result.error) throw result.error;
    if (result.status !== 0) {
        throw new Error(`program-status child failed (${result.status}): ${result.stderr}`);
    }
    const parsed = JSON.parse(result.stdout);
    if (!Array.isArray(parsed.cases) || parsed.cases.length !== 4) {
        throw new Error(`program-status child returned an unexpected shape: ${result.stdout}`);
    }
    const scenarios = parsed.cases.map((entry: any) => ({
        name: entry.name,
        dimensions: { columns: 80, rows: 24 },
        inputs: {
            processTerminal: "public start()/setProgramStatus()/stop()",
            piProgramStatusOverride: entry.name.startsWith("override-")
                ? entry.name === "override-enabled"
                    ? "1"
                    : "0"
                : null,
            replyOrder: entry.name === "default-device-attributes-before-reply" ? "DA-before-reply" : "reply-before-DA",
            states: ["working:app=pi", "blocked:kind=permission:msg=Allow?"],
        },
        expected: {
            queryWrite: entry.queryWrite,
            programStatusWrites: entry.programStatusWrites,
            stopWrites: entry.stopWrites,
            inputs: entry.inputs,
        },
    }));
    return {
        sourceEndpoints: [
            "packages/tui/src/terminal.ts:ProcessTerminal.start,ProcessTerminal.stop,ProcessTerminal.setProgramStatus",
            "packages/tui/src/program-status.ts:formatProgramStatus,isProgramStatusReply,PROGRAM_STATUS_QUERY",
            "packages/tui/test/terminal.test.ts:program status (OSC 7501)",
        ],
        scenarios,
    };
}

const replacements: Array<[string, string]> = [
    [path.resolve(piCheckout).split(path.sep).join("/"), "<pi-checkout>"],
];
const normalize = (value: unknown): unknown => {
    const withNulls = normalizeUndefined(value);
    const text = JSON.stringify(withNulls);
    let normalized = text;
    for (const [from, to] of replacements) normalized = normalized.split(from).join(to);
    return JSON.parse(normalized);
};

function normalizeUndefined(value: unknown): unknown {
    if (Array.isArray(value)) return value.map(normalizeUndefined);
    if (value && typeof value === "object") {
        const result: Record<string, unknown> = {};
        for (const [key, entry] of Object.entries(value)) {
            result[key] = entry === undefined ? null : normalizeUndefined(entry);
        }
        return result;
    }
    return value;
}

const artifactSpecs = [
    { name: "hardware-cursor.json", captured: hardwareCursor },
    { name: "input-standalone.json", captured: inputStandalone },
    { name: "selection-multiclick.json", captured: selectionMulticlick },
    { name: "cursor-composition.json", captured: cursorComposition },
    { name: "padding-and-selection-reset.json", captured: paddingAndSelectionReset },
    { name: "overlay-routing.json", captured: overlayRouting },
    { name: "keys-and-terminal-replies.json", captured: keysAndTerminalReplies },
    { name: "program-status-format.json", captured: programStatusFormat },
    { name: "program-status-detection.json", captured: programStatusDetection },
    { name: "terminal-capabilities.json", captured: terminalCapabilities },
];

const artifactRecords: any[] = [];
for (const spec of artifactSpecs) {
    const artifactScenarios = normalize(spec.captured.scenarios);
    const sourceEndpoints = normalize(spec.captured.sourceEndpoints);
    const artifact = {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: baselineName,
        revision: baseline.revision,
        capturedAt,
        environment,
        sourceEndpoints,
        scenarios: artifactScenarios,
    };
    const bytes = Buffer.from(`${JSON.stringify(artifact, null, 2)}\n`);
    writeFileSync(path.join(familyDir, spec.name), bytes);
    artifactRecords.push({
        path: spec.name,
        family,
        bytes: bytes.length,
        sha256: sha256(bytes),
        sourceEndpoints,
        scenarios: artifactScenarios,
    });
    console.log(`  ${spec.name}: ${(artifactScenarios as unknown[]).length} scenarios, ${bytes.length} bytes`);
}

const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const generatorSources = [
    scriptPath,
    path.join(familyDir, "scenarios-1050.mts"),
    path.join(familyDir, "program-status-child.mts"),
    path.join(historicalCapture, "hardware-cursor-scenarios.mts"),
    path.join(historicalCapture, "input-scenarios.mts"),
    path.join(historicalCapture, "selection-scenarios.mts"),
].map((source) => ({
    path: path.relative(repositoryRoot, source).split(path.sep).join("/"),
    sha256: sha256(readFileSync(source)),
}));
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: baselineName,
    revision: baseline.revision,
    ticket: "1050",
    family,
    capturedAt,
    environment,
    generator: {
        path: generatorPath,
        sha256: sha256(readFileSync(scriptPath)),
        command:
            `PI_CHECKOUT=/tmp/cch-1050-pi-frozen PI_BASELINE=${baselineName} ` +
            `/tmp/cch-1050-pi-frozen/node_modules/.bin/tsx ${generatorPath}`,
        sources: generatorSources,
    },
    artifacts: artifactRecords,
};
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(`captured ${family} for ${baselineName} (${baseline.revision})`);

function runGit(cwd: string, args: string[]): string {
    const result = spawnSync("git", args, {
        cwd,
        encoding: "utf8",
        env: { ...process.env, GIT_CONFIG_NOSYSTEM: "1", GIT_CONFIG_GLOBAL: "/dev/null" },
    });
    if (result.error) throw result.error;
    if (result.status !== 0) {
        throw new Error(`git ${args.join(" ")} failed in ${cwd}: ${result.stderr}`);
    }
    return result.stdout.trim();
}
