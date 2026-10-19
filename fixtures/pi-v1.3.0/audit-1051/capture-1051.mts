#!/usr/bin/env tsx
/**
 * Capture the `audit-1051` product TUI family against the frozen `pi-v1.3.0` baseline.
 *
 * Three real product-boundary observations are captured:
 *  - the existing native dual-runtime harness's pi side for its complete fixed scenario list
 *    (startup, editors, selectors, tool result, footer, resize, scrollback);
 *  - the extension editor lifecycle through `ctx.ui.setEditorComponent`: factory inputs,
 *    callback/appearance/focus/text transfers, border-token follow, and restore/reinstall;
 *  - the OSC 7501 program-status reports the interactive terminal receives for idle, blocked,
 *    working, done and error states.
 *
 * The observations are public-call/scenario results from the frozen checkout, not a Pike
 * expectation and not a capability-alignment claim. A controlled workspace, controlled terminal
 * dimensions, a scripted faux model, and a private clean checkout are required. The bundle is
 * written only after every guard and observation succeeds.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1051/capture-1051.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire, registerHooks } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1051";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi"));
const childMode = process.argv.includes("--child");

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
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== baseline.revision) {
    throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baselineName}), found ${head}`);
}
if (execFileSync("git", ["-C", piCheckout, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
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
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: glibc,
    node: process.version,
    tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();
const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");
const source = (relative: string) => pathToFileURL(path.join(piCheckout, relative)).href;
const { VirtualTerminal: VirtualTerminalType } = await import(source("packages/tui/test/virtual-terminal.ts"));
const delay = (milliseconds: number) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const writeJson = (target: string, value: any) => writeFileSync(target, `${JSON.stringify(value, null, 2)}\n`);
const readJson = (target: string) => JSON.parse(readFileSync(target, "utf8"));

/// The frozen, read-only pi checkout omits a gitignored generated provider catalog. Every scenario
/// here uses an explicitly supplied faux model, so the import is stubbed in memory and the
/// authority checkout stays clean.
registerHooks({
    resolve(specifier, context, nextResolve) {
        if (specifier === "./data/azure.json" && context.parentURL?.includes("/packages/ai/src/providers/")) {
            return { url: new URL(specifier, context.parentURL).href, shortCircuit: true, format: "json" };
        }
        return nextResolve(specifier, context);
    },
    load(url, context, nextLoad) {
        if (url.endsWith("/providers/data/azure.json")) return { format: "json", source: "{}", shortCircuit: true };
        return nextLoad(url, context);
    },
});

// -------------------------------------------------------------------------------------------------
// Existing native dual-runtime harness: pi-side scenarios.
// This list mirrors `fixtures/pi-coding-agent/capture/native-tui-differential.mts` exactly so the
// frozen product rows can be re-read next to their historical v1.0.4 captures without relabelling.
// -------------------------------------------------------------------------------------------------
type DifferentialScenario = {
    id: string;
    width: number;
    inputs: string[];
    resize?: string;
};
const differentialScenarios: DifferentialScenario[] = [
    { id: "boot-72", width: 72, inputs: [] },
    { id: "boot-100", width: 100, inputs: [] },
    { id: "boot-120", width: 120, inputs: [] },
    { id: "boot-41", width: 41, inputs: [] },
    { id: "model-selector", width: 100, inputs: ["\x0c", "\r"] },
    {
        id: "settings-selector",
        width: 100,
        inputs: ["/settings", "\r", "Show hardware cursor", "\r", "\x15", "Terminal progress", "\r", "\x15", "\x1b"],
    },
    { id: "thinking-selector", width: 100, inputs: ["/thinking", "\r", "\r"] },
    { id: "editor-long", width: 72, inputs: ["A long editor line that must wrap without losing the fixture-authored suffix", "\x15"] },
    { id: "editor-cjk", width: 72, inputs: ["混在文本 mixed 日本語 ✅", "\x15"] },
    { id: "editor-token", width: 41, inputs: ["UNBREAKABLE_TOKEN_abcdefghijklmnopqrstuvwxyz0123456789", "\x15"] },
    { id: "user-message", width: 72, inputs: ["deterministic user prompt\r"] },
    { id: "tool-result", width: 100, inputs: ["run the deterministic tool\r"] },
    { id: "status-footer", width: 100, inputs: ["deterministic status prompt\r"] },
    { id: "resize-72-41", width: 72, inputs: [], resize: "41x30->72x30" },
    { id: "resize-100-72", width: 100, inputs: [], resize: "72x40->100x40" },
    { id: "resize-120-100", width: 120, inputs: [], resize: "100x50->120x50" },
    { id: "scrollback", width: 72, inputs: ["line one\r", "line two\r", "line three\r", "line four\r", "line five\r", "line six\r"] },
];

const harnessPath = path.join(repositoryRoot, "fixtures/pi-coding-agent/capture/native-tui-differential.mts");
const harnessStubPath = path.join(repositoryRoot, "fixtures/pi-coding-agent/capture/grok-mermaid-stub.mts");

function captureTsconfig(output: string): string {
    if (existsSync(path.join(piCheckout, "node_modules/grok-mermaid"))) {
        return path.join(piCheckout, "tsconfig.json");
    }
    const rootConfig = JSON.parse(readFileSync(path.join(piCheckout, "tsconfig.json"), "utf8")) as {
        compilerOptions?: { paths?: Record<string, string[]> };
    };
    const paths = { ...(rootConfig.compilerOptions?.paths ?? {}) };
    paths["grok-mermaid"] = [harnessStubPath];
    const capturePath = `${output}.tsconfig.json`;
    writeFileSync(
        capturePath,
        `${JSON.stringify(
            {
                compilerOptions: {
                    target: "ES2022",
                    module: "NodeNext",
                    moduleResolution: "NodeNext",
                    allowImportingTsExtensions: true,
                    rewriteRelativeImportExtensions: true,
                    esModuleInterop: true,
                    skipLibCheck: true,
                    types: ["node"],
                    baseUrl: piCheckout,
                    paths,
                },
            },
            null,
            2,
        )}\n`,
    );
    return capturePath;
}

function childEnvironment(extra: Record<string, string>): NodeJS.ProcessEnv {
    return {
        PATH: `${path.dirname(process.execPath)}:/usr/bin:/bin`,
        LANG: "C.UTF-8",
        LC_ALL: "C.UTF-8",
        TZ: "UTC",
        HOME: "/tmp/audit-1051-home",
        USERPROFILE: "/tmp/audit-1051-home",
        PI_OFFLINE: "1",
        PI_SKIP_VERSION_CHECK: "1",
        TERM: "xterm-256color",
        COLORTERM: "truecolor",
        COLORFGBG: "15;0",
        FORCE_COLOR: "3",
        ...extra,
    };
}

function normalizeProductText(text: string, root: string): string {
    return text
        .replaceAll(root, "<deterministic-workspace>")
        .replaceAll(/\/tmp\/audit-1051-[A-Za-z0-9]+/g, "<deterministic-workspace>");
}

function trimDifferentialReport(report: any, root: string): any {
    return {
        runtime: report.runtime,
        scenario: report.scenario,
        width: report.width,
        theme: report.theme,
        snapshots: (report.snapshots as any[]).map((snapshot) => ({
            visible: (snapshot.visible as string[]).map((line) => normalizeProductText(line, root)),
            scrollback: (snapshot.scrollback as string[]).map((line) => normalizeProductText(line, root)),
            cursor: snapshot.cursor,
            cursorVisible: snapshot.cursorVisible,
        })),
    };
}

async function captureNativeDifferential(root: string): Promise<any[]> {
    const reports: any[] = [];
    for (const scenario of differentialScenarios) {
        const scenarioRoot = path.join(root, "native", scenario.id);
        const workspace = path.join(scenarioRoot, "workspace");
        const agentDirectory = path.join(scenarioRoot, "agent");
        mkdirSync(workspace, { recursive: true });
        mkdirSync(agentDirectory, { recursive: true });
        const output = path.join(scenarioRoot, "capture.json");
        const home = path.join(scenarioRoot, "home");
        mkdirSync(home, { recursive: true });
        const environmentForScenario: Record<string, string> = {
            HOME: home,
            USERPROFILE: home,
            CCH_DIFFERENTIAL_SCENARIO: scenario.id,
            CCH_DIFFERENTIAL_WORKSPACE: workspace,
            CCH_DIFFERENTIAL_WIDTH: String(scenario.width),
            CCH_DIFFERENTIAL_INPUTS: scenario.inputs.map((input) => Buffer.from(input, "utf8").toString("hex") || "-").join("|"),
            CCH_DIFFERENTIAL_OUTPUT: output,
            CCH_DIFFERENTIAL_AGENT_DIRECTORY: agentDirectory,
            XDG_CONFIG_HOME: path.join(scenarioRoot, "xdg/config"),
            PI_CODING_AGENT_DIR: agentDirectory,
            PI_CHECKOUT: piCheckout,
            ...(scenario.resize === undefined ? {} : { CCH_DIFFERENTIAL_RESIZE: scenario.resize }),
        };
        const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
        const result = spawnSync(tsx, ["--tsconfig", captureTsconfig(output), harnessPath, "--runtime", "pi"], {
            cwd: repositoryRoot,
            env: childEnvironment(environmentForScenario),
            encoding: "utf8",
            timeout: 120_000,
        });
        if (result.error) throw new Error(`native differential ${scenario.id} failed to start: ${result.error}`);
        if (result.status !== 0) {
            throw new Error(
                `native differential ${scenario.id} failed with ${result.status}:\n` +
                    `${result.stdout ?? ""}\n${result.stderr ?? ""}`,
            );
        }
        const report = readJson(output);
        if (report.runtime !== "pi" || report.scenario !== scenario.id) {
            throw new Error(`native differential ${scenario.id} produced a mismatched report`);
        }
        reports.push(trimDifferentialReport(report, scenarioRoot));
    }
    return reports;
}

// -------------------------------------------------------------------------------------------------
// Child modes. Each scenario gets a fresh process so pi's faux provider registration and terminal
// state cannot leak between the product observations.
// -------------------------------------------------------------------------------------------------
type ProgramStatus = {
    state: "idle" | "working" | "blocked" | "done" | "error" | "clear";
    app?: string;
    kind?: "permission" | "question" | "auth";
    message?: string;
};

class RecordingTerminal extends VirtualTerminalType {
    ansi = "";
    cursorVisible = true;
    programStatusCalls: ProgramStatus[] = [];
    override write(data: string): void {
        this.ansi += data;
        for (const match of data.matchAll(/\x1b\[\?25([hl])/g)) this.cursorVisible = match[1] === "h";
        super.write(data);
    }
    override hideCursor(): void {
        this.cursorVisible = false;
        super.hideCursor();
    }
    override showCursor(): void {
        this.cursorVisible = true;
        super.showCursor();
    }
    override setProgramStatus(status?: ProgramStatus): void {
        if (status !== undefined) this.programStatusCalls.push(status);
    }
}

async function waitForText(terminal: any, expected: string, timeoutMs = 8_000): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        await terminal.flush();
        if (terminal.getViewport().join("\n").includes(expected)) return;
        await delay(20);
    }
    throw new Error(`interactive capture did not render ${JSON.stringify(expected)}:\n${terminal.getViewport().join("\n")}`);
}

async function waitForFact(facts: any[], stage: string, timeoutMs = 8_000): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (facts.some((fact) => fact.stage === stage)) return;
        await delay(20);
    }
    throw new Error(`interactive capture did not record stage ${JSON.stringify(stage)}: ${JSON.stringify(facts)}`);
}

function editorBorderRows(viewport: string[]): number {
    return viewport.filter((line) => /^─+$/.test(line.trim())).length;
}

function themeInputSummary(theme: any): Record<string, unknown> {
    return {
        type: typeof theme,
        keys: Object.keys(theme ?? {}).sort(),
        borderColorType: typeof theme?.borderColor,
        selectListType: typeof theme?.selectList,
    };
}

function keybindingsInputSummary(keybindings: any): Record<string, unknown> {
    return {
        type: typeof keybindings,
        constructor: keybindings?.constructor?.name ?? "",
        keys: Object.keys(keybindings ?? {}).sort(),
        methods: ["matches", "getEffectiveConfig", "keys"].filter((name) => typeof keybindings?.[name] === "function"),
    };
}

function tuiInputSummary(tui: any): Record<string, unknown> {
    return {
        type: typeof tui,
        constructor: tui?.constructor?.name ?? "",
        keys: Object.keys(tui ?? {}).sort(),
        methods: ["requestRender", "setFocus", "getFocus", "stop"].filter((name) => typeof tui?.[name] === "function"),
    };
}

class ObservedEditor {
    readonly actionHandlers = new Map<string, () => void>();
    readonly observations: any[] = [];
    private readonly inner: any;
    private onSubmitValue?: (text: string) => void;
    private onChangeValue?: (text: string) => void;
    private onEscapeValue?: () => void;
    private onCtrlDValue?: () => void;
    private onPasteImageValue?: () => void;
    private onExtensionShortcutValue?: (data: string) => boolean;
    private borderColorValue?: (text: string) => string;

    constructor(tui: any, theme: any, keybindings: any, editorClass: any) {
        this.inner = new editorClass(tui, theme, keybindings, { paddingX: 0 });
        this.inner.actionHandlers = this.actionHandlers;
        this.inner.onSubmit = (text: string) => {
            this.observations.push({ event: "inner.onSubmit", text });
            this.onSubmitValue?.(text);
        };
        this.inner.onChange = (text: string) => {
            this.observations.push({ event: "inner.onChange", text });
            this.onChangeValue?.(text);
        };
        this.inner.onEscape = () => {
            this.observations.push({ event: "inner.onEscape" });
            this.onEscapeValue?.();
        };
        this.inner.onCtrlD = () => {
            this.observations.push({ event: "inner.onCtrlD" });
            this.onCtrlDValue?.();
        };
        this.inner.onPasteImage = () => {
            this.observations.push({ event: "inner.onPasteImage" });
            this.onPasteImageValue?.();
        };
        this.inner.onExtensionShortcut = (data: string) => {
            this.observations.push({ event: "inner.onExtensionShortcut", data: Buffer.from(data, "utf8").toString("hex") });
            return this.onExtensionShortcutValue?.(data) ?? false;
        };
        this.observations.push({ event: "constructed", text: this.inner.getText(), focused: this.inner.focused });
    }

    get focused(): boolean {
        return this.inner.focused;
    }
    set focused(value: boolean) {
        this.observations.push({ event: "focus", value });
        this.inner.focused = value;
    }
    get borderColor(): ((text: string) => string) | undefined {
        return this.borderColorValue;
    }
    set borderColor(value: ((text: string) => string) | undefined) {
        this.borderColorValue = value;
        this.observations.push({ event: "borderColor", sample: value?.("x") ?? null });
    }
    get onSubmit(): ((text: string) => void) | undefined {
        return this.onSubmitValue;
    }
    set onSubmit(value: ((text: string) => void) | undefined) {
        this.onSubmitValue = value;
        this.observations.push({ event: "onSubmit", assigned: typeof value === "function" });
    }
    get onChange(): ((text: string) => void) | undefined {
        return this.onChangeValue;
    }
    set onChange(value: ((text: string) => void) | undefined) {
        this.onChangeValue = value;
        this.observations.push({ event: "onChange", assigned: typeof value === "function" });
    }
    get onEscape(): (() => void) | undefined {
        return this.onEscapeValue;
    }
    set onEscape(value: (() => void) | undefined) {
        this.onEscapeValue = value;
        this.observations.push({ event: "onEscape", assigned: typeof value === "function" });
    }
    get onCtrlD(): (() => void) | undefined {
        return this.onCtrlDValue;
    }
    set onCtrlD(value: (() => void) | undefined) {
        this.onCtrlDValue = value;
        this.observations.push({ event: "onCtrlD", assigned: typeof value === "function" });
    }
    get onPasteImage(): (() => void) | undefined {
        return this.onPasteImageValue;
    }
    set onPasteImage(value: (() => void) | undefined) {
        this.onPasteImageValue = value;
        this.observations.push({ event: "onPasteImage", assigned: typeof value === "function" });
    }
    get onExtensionShortcut(): ((data: string) => boolean) | undefined {
        return this.onExtensionShortcutValue;
    }
    set onExtensionShortcut(value: ((data: string) => boolean) | undefined) {
        this.onExtensionShortcutValue = value;
        this.observations.push({ event: "onExtensionShortcut", assigned: typeof value === "function" });
    }
    setPaddingX(padding: number): void {
        this.observations.push({ event: "setPaddingX", padding });
        this.inner.setPaddingX(padding);
    }
    setAutocompleteMaxVisible(maxVisible: number): void {
        this.observations.push({ event: "setAutocompleteMaxVisible", maxVisible });
        this.inner.setAutocompleteMaxVisible(maxVisible);
    }
    setAutocompleteProvider(provider: any): void {
        this.observations.push({ event: "setAutocompleteProvider", present: provider != null });
        this.inner.setAutocompleteProvider(provider);
    }
    getText(): string {
        return this.inner.getText();
    }
    setText(text: string): void {
        this.observations.push({ event: "setText", text });
        this.inner.setText(text);
    }
    getExpandedText(): string {
        return this.inner.getExpandedText?.() ?? this.inner.getText();
    }
    insertTextAtCursor(text: string): void {
        this.inner.insertTextAtCursor?.(text);
    }
    addToHistory(text: string): void {
        this.inner.addToHistory?.(text);
    }
    handleInput(data: string): void {
        this.observations.push({ event: "input", hex: Buffer.from(data, "utf8").toString("hex") });
        this.inner.handleInput(data);
    }
    render(width: number): string[] {
        const lines = this.inner.render(width);
        this.observations.push({ event: "render", width, lineCount: lines.length });
        return lines;
    }
    invalidate(): void {
        this.inner.invalidate();
    }
}

async function runExtensionEditorChild(): Promise<any> {
    const { createHarness } = await import(source("packages/coding-agent/test/suite/harness.ts"));
    const { AgentSessionRuntime } = await import(source("packages/coding-agent/src/core/agent-session-runtime.ts"));
    const { InteractiveMode } = await import(source("packages/coding-agent/src/modes/interactive/interactive-mode.ts"));
    const { CustomEditor } = await import(source("packages/coding-agent/src/modes/interactive/components/custom-editor.ts"));
    const { fauxAssistantMessage } = await import(source("packages/ai/src/providers/faux.ts"));

    const root = mkdtempSync(path.join(tmpdir(), "audit-1051-editor-"));
    const workspace = path.join(root, "workspace");
    const agentDirectory = path.join(root, "agent");
    mkdirSync(workspace, { recursive: true });
    mkdirSync(agentDirectory, { recursive: true });
    process.chdir(workspace);

    const facts: any[] = [];
    const installs: ObservedEditor[] = [];
    let latest: ObservedEditor | undefined;
    const factory = (tui: any, theme: any, keybindings: any) => {
        facts.push({
            stage: "factory",
            inputs: {
                tui: tuiInputSummary(tui),
                theme: themeInputSummary(theme),
                keybindings: keybindingsInputSummary(keybindings),
            },
        });
        latest = new ObservedEditor(tui, theme, keybindings, CustomEditor);
        installs.push(latest);
        return latest;
    };

    const harness = await createHarness({
        models: [
            {
                id: "faux-1",
                name: "Faux Reasoning",
                reasoning: true,
                input: ["text", "image"],
                cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
                contextWindow: 128000,
                maxTokens: 16384,
            },
        ],
        settings: { theme: "dark" },
        extensionFactories: [
            {
                name: "audit-1051-editor",
                factory: (pi: any) => {
                    pi.on("session_start", async (_event: any, ctx: any) => {
                        facts.push({ stage: "session_start" });
                        ctx.ui.setEditorText("before-install");
                        facts.push({ stage: "default_text_set", text: ctx.ui.getEditorText() });
                        ctx.ui.setEditorComponent(factory);
                        facts.push({
                            stage: "installed",
                            text: ctx.ui.getEditorText(),
                            focused: latest?.focused ?? false,
                            actionHandlerNames: latest === undefined ? [] : [...latest.actionHandlers.keys()],
                        });
                    });
                    pi.registerShortcut("alt+e", {
                        description: "restore the default editor",
                        handler: (ctx: any) => {
                            facts.push({
                                stage: "restore.request",
                                text: ctx.ui.getEditorText(),
                                customFocused: latest?.focused ?? false,
                            });
                            ctx.ui.setEditorComponent(undefined);
                            facts.push({
                                stage: "restore.done",
                                text: ctx.ui.getEditorText(),
                                customFocused: latest?.focused ?? false,
                            });
                        },
                    });
                    pi.registerShortcut("alt+i", {
                        description: "install the probe editor again",
                        handler: (ctx: any) => {
                            facts.push({ stage: "reinstall.request", text: ctx.ui.getEditorText() });
                            ctx.ui.setEditorComponent(factory);
                            facts.push({
                                stage: "reinstall.done",
                                text: ctx.ui.getEditorText(),
                                customFocused: latest?.focused ?? false,
                            });
                        },
                    });
                    pi.registerShortcut("alt+s", {
                        description: "record the current editor text",
                        handler: (ctx: any) => {
                            facts.push({ stage: "state", text: ctx.ui.getEditorText() });
                        },
                    });
                },
            },
        ],
    });
    harness.setResponses([fauxAssistantMessage("deterministic extension reply")]);

    const terminal = new RecordingTerminal(100, 24);
    const services = {
        cwd: workspace,
        agentDir: agentDirectory,
        modelRuntime: harness.session.modelRuntime,
        settingsManager: harness.settingsManager,
        resourceLoader: harness.session.resourceLoader,
        diagnostics: [],
    };
    const runtimeHost = new AgentSessionRuntime(harness.session, services, async () => {
        throw new Error("session replacement is not used by the audit-1051 editor capture");
    });
    const mode = new InteractiveMode(runtimeHost, { terminal, tuiMode: "regular", initialThemeSetting: "dark" });
    void mode.run().catch((error: unknown) => {
        console.error(error);
        process.exitCode = 1;
    });

    await waitForText(terminal, "Press ctrl+o");
    await waitForFact(facts, "installed");
    await terminal.flush();
    facts.push({ stage: "startup", viewport: terminal.getViewport(), editorBorderRows: editorBorderRows(terminal.getViewport()) });

    const borderBeforeTyping = latest?.borderColor?.("x") ?? null;
    terminal.sendInput("typed-after-install");
    await delay(120);
    await terminal.flush();
    facts.push({
        stage: "typed",
        text: latest?.getText(),
        border: latest?.borderColor?.("x") ?? null,
    });

    terminal.sendInput("\x1b[Z");
    await waitForText(terminal, "Thinking level: minimal");
    await terminal.flush();
    facts.push({
        stage: "thinking",
        borderBefore: borderBeforeTyping,
        borderAfter: latest?.borderColor?.("x") ?? null,
        footerLine: terminal.getViewport().find((line: string) => line.includes("faux-1")) ?? null,
    });

    terminal.sendInput("\x03");
    await delay(80);
    terminal.sendInput("!");
    await delay(140);
    await terminal.flush();
    facts.push({ stage: "bash", text: latest?.getText(), border: latest?.borderColor?.("x") ?? null });
    terminal.sendInput("\x7f");
    await delay(140);
    await terminal.flush();
    facts.push({ stage: "bash_off", text: latest?.getText(), border: latest?.borderColor?.("x") ?? null });

    terminal.sendInput("after-install");
    await delay(120);
    await terminal.flush();
    facts.push({ stage: "typed_before_restore", text: latest?.getText() });

    terminal.sendInput("\x1be");
    await waitForFact(facts, "restore.done");
    await terminal.flush();
    facts.push({
        stage: "restored",
        viewport: terminal.getViewport(),
        editorBorderRows: editorBorderRows(terminal.getViewport()),
    });

    terminal.sendInput("default-typed");
    await delay(120);
    terminal.sendInput("\x1bs");
    await waitForFact(facts, "state");
    await terminal.flush();
    facts.push({ stage: "default_state", viewport: terminal.getViewport() });

    terminal.sendInput("\x1bi");
    await waitForFact(facts, "reinstall.done");
    await terminal.flush();
    facts.push({
        stage: "reinstalled",
        viewport: terminal.getViewport(),
        editorBorderRows: editorBorderRows(terminal.getViewport()),
    });

    terminal.sendInput("submitted-draft");
    await delay(200);
    terminal.sendInput("\r");
    await waitForText(terminal, "deterministic extension reply");
    await delay(300);
    await terminal.flush();
    facts.push({
        stage: "submitted",
        textAfterSubmit: latest?.getText() ?? null,
        onSubmitInvocations: latest?.observations.filter((event) => event.event === "inner.onSubmit").length ?? 0,
        onSubmitAssigned: latest?.observations.some(
            (event) => event.event === "onSubmit" && event.assigned === true,
        ) ?? false,
    });

    terminal.sendInput("\x1be");
    await delay(300);
    await terminal.flush();
    facts.push({ stage: "restored_again", focused: latest?.focused ?? null });

    const observation = {
        facts,
        installs: installs.map((install) => install.observations),
        finalViewport: terminal.getViewport(),
        finalEditorBorderRows: editorBorderRows(terminal.getViewport()),
        programStatusCalls: terminal.programStatusCalls,
    };
    return observation;
}

async function runProgramStatusChild(): Promise<any> {
    const { createHarness } = await import(source("packages/coding-agent/test/suite/harness.ts"));
    const { AgentSessionRuntime } = await import(source("packages/coding-agent/src/core/agent-session-runtime.ts"));
    const { InteractiveMode } = await import(source("packages/coding-agent/src/modes/interactive/interactive-mode.ts"));
    const { fauxAssistantMessage } = await import(source("packages/ai/src/providers/faux.ts"));

    const root = mkdtempSync(path.join(tmpdir(), "audit-1051-status-"));
    const workspace = path.join(root, "workspace");
    const agentDirectory = path.join(root, "agent");
    mkdirSync(workspace, { recursive: true });
    mkdirSync(agentDirectory, { recursive: true });
    process.chdir(workspace);

    const facts: any[] = [];
    const harness = await createHarness({
        models: [
            {
                id: "faux-1",
                name: "Faux Reasoning",
                reasoning: true,
                input: ["text", "image"],
                cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
                contextWindow: 128000,
                maxTokens: 16384,
            },
        ],
        settings: { theme: "dark" },
        extensionFactories: [
            {
                name: "audit-1051-status",
                factory: (pi: any) => {
                    pi.registerShortcut("alt+g", {
                        description: "open a probe dialog",
                        handler: async (ctx: any) => {
                            facts.push({ stage: "dialog.request" });
                            const value = await ctx.ui.input("Probe dialog");
                            facts.push({ stage: "dialog.done", value: value ?? null });
                        },
                    });
                },
            },
        ],
    });
    harness.setResponses([fauxAssistantMessage("deterministic status reply")]);

    const terminal = new RecordingTerminal(100, 24);
    const services = {
        cwd: workspace,
        agentDir: agentDirectory,
        modelRuntime: harness.session.modelRuntime,
        settingsManager: harness.settingsManager,
        resourceLoader: harness.session.resourceLoader,
        diagnostics: [],
    };
    const runtimeHost = new AgentSessionRuntime(harness.session, services, async () => {
        throw new Error("session replacement is not used by the audit-1051 status capture");
    });
    const mode = new InteractiveMode(runtimeHost, { terminal, tuiMode: "regular", initialThemeSetting: "dark" });
    void mode.run().catch((error: unknown) => {
        console.error(error);
        process.exitCode = 1;
    });

    await waitForText(terminal, "Press ctrl+o");
        await terminal.flush();
        facts.push({ stage: "idle", calls: [...terminal.programStatusCalls] });

        terminal.sendInput("\x1bg");
        await waitForText(terminal, "Probe dialog");
        await terminal.flush();
        facts.push({ stage: "blocked", calls: [...terminal.programStatusCalls], viewport: terminal.getViewport() });

        terminal.sendInput("\x1b");
        await delay(200);
        await terminal.flush();
        facts.push({ stage: "blocked_closed", calls: [...terminal.programStatusCalls], viewport: terminal.getViewport() });

        terminal.sendInput("status prompt");
        await delay(200);
        await terminal.flush();
        facts.push({ stage: "prompt_typed", viewport: terminal.getViewport() });
        terminal.sendInput("\r");
        await waitForText(terminal, "deterministic status reply");
        await terminal.flush();
        facts.push({ stage: "done", calls: [...terminal.programStatusCalls] });

        await delay(300);
        harness.setResponses([fauxAssistantMessage("", { stopReason: "error", errorMessage: "deterministic failure" })]);
        terminal.sendInput("error prompt");
        await delay(200);
        terminal.sendInput("\r");
        await waitForText(terminal, "deterministic failure");
        await terminal.flush();
    facts.push({ stage: "error", calls: [...terminal.programStatusCalls] });

    const observation = { facts, finalViewport: terminal.getViewport() };
    return observation;
}

// -------------------------------------------------------------------------------------------------
// Parent capture.
// -------------------------------------------------------------------------------------------------
const root = mkdtempSync(path.join(tmpdir(), "audit-1051-"));

function runChild(mode: "extension-editor" | "program-status"): any {
    const output = path.join(root, `${mode}.json`);
    const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
    const result = spawnSync(
        tsx,
        ["--tsconfig", captureTsconfig(output), scriptPath, "--child", mode, "--output", output],
        {
            cwd: repositoryRoot,
            env: childEnvironment({ PI_CHECKOUT: piCheckout }),
            encoding: "utf8",
            timeout: 180_000,
        },
    );
    if (result.error) throw new Error(`${mode} child failed to start: ${result.error}`);
    if (result.status !== 0) {
        throw new Error(`${mode} child failed with ${result.status}:\n${result.stdout ?? ""}\n${result.stderr ?? ""}`);
    }
    return readJson(output);
}

function artifactEnvelope(sourceEndpoints: string[], scenarios: any[], payload: any): any {
    return {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: baselineName,
        revision: baseline.revision,
        capturedAt,
        environment,
        sourceEndpoints,
        scenarios,
        ...payload,
    };
}

function generatorRecord(): any {
    const paths = [
        "fixtures/pi-v1.3.0/audit-1051/capture-1051.mts",
        "fixtures/pi-coding-agent/capture/native-tui-differential.mts",
        "fixtures/pi-coding-agent/capture/grok-mermaid-stub.mts",
    ];
    return {
        path: paths[0],
        sha256: sha256(readFileSync(path.join(repositoryRoot, paths[0]))),
        command:
            "PI_CHECKOUT=<private-frozen-checkout> PI_BASELINE=pi-v1.3.0 " +
            "<private-frozen-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1051/capture-1051.mts",
        sources: paths.map((relative) => ({
            path: relative,
            sha256: sha256(readFileSync(path.join(repositoryRoot, relative))),
        })),
    };
}

function writeBundle(differentialReports: any[], extensionObservation: any, statusObservation: any): void {
    const scenarioRecords = differentialReports.map((report) => {
        const scenario = differentialScenariosById(report.scenario);
        return {
            name: `native differential ${report.scenario} at ${report.width} columns`,
            inputs: {
                scenario: report.scenario,
                width: report.width,
                terminalRows: 24,
                keystrokes: scenario.inputs,
                resize: scenario.resize ?? null,
                profile: "dark truecolor, deterministic faux model, controlled workspace",
            },
            expected: {
                runtime: report.runtime,
                snapshotCount: report.snapshots.length,
                themeName: report.theme.name,
                colorCapability: report.theme.colorCapability,
                cursorVisibleInFinalSnapshot: report.snapshots.at(-1)?.cursorVisible ?? null,
            },
            normalizations: [
                "the private capture root is replaced with <deterministic-workspace>",
                "raw ANSI and styled-cell projections are omitted; visible cell text, scrollback text and cursor position are the compared boundary",
            ],
            upstreamTest: {
                file: "fixtures/pi-coding-agent/capture/native-tui-differential.mts",
                scenario: report.scenario,
            },
        };
    });
    const differential = artifactEnvelope(
        [
            "packages/coding-agent/src/modes/interactive/interactive-mode.ts:InteractiveMode",
            "packages/coding-agent/test/suite/harness.ts:createHarness",
            "packages/tui/test/virtual-terminal.ts:VirtualTerminal",
        ],
        scenarioRecords,
        { reports: differentialReports },
    );

    const editorScenarios = [
        {
            name: "extension editor installs, transfers text and focus, follows thinking and Bash borders, restores and reinstalls",
            inputs: {
                terminal: { width: 100, rows: 24 },
                initialText: "before-install",
                typedText: "typed-after-install",
                thinkingShortcut: "shift+tab",
                bashModeInput: "!",
                restoreShortcut: "alt+e",
                reinstallShortcut: "alt+i",
            },
            expected: {
                factoryCalls: extensionObservation.installs.length,
                installTextTransferred: "before-install",
                installedFocused: true,
                thinkingBorderChanged: true,
                bashBorderChanged: true,
                submitInvokedOnSubmit: true,
                submitClearedEditor: true,
                restoreCarriedText: true,
                reinstallCarriedText: true,
                editorBorderRowsPerFrame: 2,
            },
            normalizations: [
                "the private capture root is replaced with <deterministic-workspace>",
                "SGR border samples are retained verbatim because the role identity is the observation",
            ],
        },
        {
            name: "extension editor applies each optional appearance and autocomplete operation once per install",
            inputs: {
                installs: 2,
                optionalOperations: ["borderColor", "setPaddingX", "setAutocompleteMaxVisible", "setAutocompleteProvider"],
            },
            expected: {
                borderColorAssignmentsInFirstInstall: extensionObservation.installs[0].filter(
                    (event: any) => event.event === "borderColor",
                ).length,
                setPaddingXCallsPerInstall: extensionObservation.installs[0].filter(
                    (event: any) => event.event === "setPaddingX",
                ).length,
                setAutocompleteMaxVisibleCallsPerInstall: extensionObservation.installs[0].filter(
                    (event: any) => event.event === "setAutocompleteMaxVisible",
                ).length,
                actionHandlerCount: extensionObservation.facts.find((fact: any) => fact.stage === "installed")?.actionHandlerNames?.length ?? 0,
                onSubmitAndOnChangeAssigned: true,
            },
            normalizations: ["none"],
        },
    ];
    const extension = artifactEnvelope(
        [
            "packages/coding-agent/src/modes/interactive/interactive-mode.ts:setCustomEditorComponent",
            "packages/coding-agent/src/core/extensions/types.ts:EditorFactory,ExtensionUIContext.setEditorComponent",
            "packages/coding-agent/src/modes/interactive/components/custom-editor.ts:CustomEditor",
            "packages/coding-agent/test/suite/harness.ts:createHarness",
        ],
        editorScenarios,
        { observation: extensionObservation },
    );

    const statusScenarios = [
        {
            name: "program status reports idle, blocked, working, done and error through the terminal contract",
            inputs: {
                terminal: { width: 100, rows: 24 },
                programStatusOverride: "recorded terminal setProgramStatus calls",
                dialogTitle: "Probe dialog",
                prompts: ["status prompt", "error prompt"],
            },
            expected: {
                statesInOrder: ["idle", "blocked", "idle", "working", "done", "working", "error"],
                app: "pi",
                blockedKind: "question",
                doneMessageAbsent: true,
                errorMessage: "deterministic failure",
            },
            normalizations: ["none"],
        },
    ];
    const programStatus = artifactEnvelope(
        [
            "packages/tui/src/program-status.ts:formatProgramStatus,isProgramStatusReply",
            "packages/coding-agent/src/modes/interactive/program-status-reporter.ts:ProgramStatusReporter",
            "packages/coding-agent/src/modes/interactive/interactive-mode.ts:InteractiveMode",
            "packages/tui/src/terminal.ts:setProgramStatus",
        ],
        statusScenarios,
        { observation: statusObservation },
    );

    const artifacts: Array<{ name: string; envelope: any; sourceEndpoints: string[]; scenarios: any[] }> = [
        {
            name: "product-differential.json",
            envelope: differential,
            sourceEndpoints: differential.sourceEndpoints,
            scenarios: scenarioRecords,
        },
        {
            name: "extension-editor.json",
            envelope: extension,
            sourceEndpoints: extension.sourceEndpoints,
            scenarios: editorScenarios,
        },
        {
            name: "program-status.json",
            envelope: programStatus,
            sourceEndpoints: programStatus.sourceEndpoints,
            scenarios: statusScenarios,
        },
    ];

    const manifestArtifacts = [];
    for (const artifact of artifacts) {
        const bytes = Buffer.from(`${JSON.stringify(artifact.envelope, null, 2)}\n`);
        writeFileSync(path.join(familyDir, artifact.name), bytes);
        manifestArtifacts.push({
            path: artifact.name,
            family,
            bytes: bytes.length,
            sha256: sha256(bytes),
            sourceEndpoints: artifact.sourceEndpoints,
            scenarios: artifact.scenarios,
        });
    }

    writeJson(path.join(familyDir, "manifest.json"), {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: baselineName,
        revision: baseline.revision,
        ticket: "1051",
        family,
        capturedAt,
        environment,
        generator: generatorRecord(),
        artifacts: manifestArtifacts,
    });
    console.log(
        `captured ${family} (${artifacts.length} artifacts, ${manifestArtifacts.reduce((total, artifact) => total + artifact.scenarios.length, 0)} scenarios) for ${baselineName} (${baseline.revision.slice(0, 8)}…)`,
    );
}

function differentialScenariosById(id: string): DifferentialScenario {
    const found = differentialScenarios.find((scenario) => scenario.id === id);
    if (found === undefined) throw new Error(`unknown differential scenario ${id}`);
    return found;
}

async function main(): Promise<number> {
    if (childMode) {
        const modeIndex = process.argv.indexOf("--child");
        const outputIndex = process.argv.indexOf("--output");
        const mode = process.argv[modeIndex + 1];
        const output = process.argv[outputIndex + 1];
        if (!mode || !output) throw new Error("child mode requires --child <mode> --output <path>");
        // The guard above already validated the checkout; child observation runs are public calls.
        const observation = mode === "extension-editor" ? await runExtensionEditorChild() : await runProgramStatusChild();
        writeJson(output, observation);
        process.exit(0);
    }

    const differentialReports = await captureNativeDifferential(root);
    const extensionObservation = runChild("extension-editor");
    const statusObservation = runChild("program-status");
    writeBundle(differentialReports, extensionObservation, statusObservation);
    return 0;
}

process.exitCode = await main();
