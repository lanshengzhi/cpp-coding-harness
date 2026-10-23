#!/usr/bin/env tsx
/**
 * Capture the `audit-831` dock-footer frames against frozen `pi-v1.3.0` (#831).
 *
 * The ticket reports that the product's two footer rows can leave the composed
 * frame while the dock grows or shrinks (autocomplete open, multi-line editor,
 * submit, resize). The only dock in the product is the fullscreen one, so this
 * capture drives the frozen `createChatViewport` assembly through the real
 * frozen toolkit (`TuiAltScreen` + the frozen `VirtualTerminal`) at the
 * reporter's 50x30 size and records, per frame, every rendered row and the
 * last two non-blank rows the ticket measures.
 *
 * The editor region is a plain `Text` because the interactive runtime is not
 * executable in isolation; its height is the growth axis: the autocomplete
 * menu and the pasted lines both render inside the product's editor component,
 * so the dock sees one taller editor block either way. No Pike output is used.
 *
 * This bundle is UNREGISTERED: it is not named in `baselines.json`; the parent
 * registers it.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout at 71023aec> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-831/capture-831.mts
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
const family = "audit-831";
const baseline = "pi-v1.3.0";
const revision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi"));
const relativeScript = path.relative(repositoryRoot, scriptPath);

if (process.env.TSX_TSCONFIG_PATH === undefined) {
    const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
    const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
        stdio: "inherit",
        env: { ...process.env, TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json") },
    });
    if (result.error) throw new Error(`cannot re-exec ${tsx}: ${result.error}`);
    process.exit(result.status ?? 1);
}

const runGit = (args: string[]) => {
    const result = spawnSync("git", ["-C", piCheckout, ...args], { encoding: "utf8" });
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error(`git ${args.join(" ")} failed: ${result.stderr}`);
    return result.stdout.trim();
};
if (runGit(["rev-parse", "HEAD"]) !== revision) {
    throw new Error(`pi checkout must be at ${revision} (baseline ${baseline}), found ${runGit(["rev-parse", "HEAD"])}`);
}
if (runGit(["status", "--porcelain"])) throw new Error("pi capture requires an unmodified source checkout");
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
    throw new Error("audit capture requires Linux x86-64/glibc");
}
if (process.env.PI_BASELINE !== undefined && process.env.PI_BASELINE !== baseline) {
    throw new Error(`this family targets ${baseline}, got PI_BASELINE=${process.env.PI_BASELINE}`);
}
const requirePi = createRequire(path.join(piCheckout, "package.json"));
let tsxVersion = "unknown";
try {
    tsxVersion = requirePi("tsx/package.json").version;
} catch {
    // Provenance only; not a capture precondition.
}

const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
const coding = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/coding-agent", relative)).href;
const { createChatViewport } = await import(coding("src/modes/interactive/chat-viewport.ts"));
const { Container } = await import(source("src/tui.ts"));
const { Text } = await import(source("src/components/text.ts"));
const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

const MENU_ROWS = 6;
const NINE_LINES = [
    "/",
    "line two",
    "line three",
    "line four",
    "line five",
    "line six",
    "line seven",
    "line eight",
    "line nine",
];
const menuBlock = (text: string) =>
    [text, ...Array.from({ length: MENU_ROWS }, (_, index) => `menu-item-${index + 1}`)].join("\n");
const queued = (count: number) =>
    Array.from({ length: count }, (_, index) => `Steering: queued ${index}`).join("\n");

interface DockInputs {
    readonly columns: number;
    readonly rows: number;
    readonly document: string[];
    readonly pending?: string;
    readonly status?: string;
    readonly editor: string;
    readonly footer: string[];
}

interface StepAction {
    readonly name: string;
    readonly editor?: string;
    readonly pending?: string;
    readonly resize?: { columns: number; rows: number };
}

class Viewport {
    readonly terminal: any;
    readonly tui: any;
    readonly transcript: any;
    readonly documentText: any;
    readonly editorText: any;
    readonly pendingText: any;

    constructor(inputs: DockInputs) {
        this.terminal = new VirtualTerminal(inputs.columns, inputs.rows);
        const document = new Container();
        this.documentText = new Text(inputs.document.join("\n"), 0, 0);
        document.addChild(this.documentText);
        this.editorText = new Text(inputs.editor, 0, 0);
        this.pendingText = new Text(inputs.pending ?? "", 0, 0);
        const viewport = createChatViewport({
            document,
            pendingMessages: this.pendingText,
            status: new Text(inputs.status ?? "", 0, 0),
            editor: this.editorText,
            footer: new Text(inputs.footer.join("\n"), 0, 0),
        });
        this.transcript = viewport.transcript;
        this.tui = new TuiAltScreen(this.terminal);
        this.tui.setLayoutRoot(viewport.root);
        this.tui.start();
    }

    async apply(action: StepAction) {
        if (action.resize !== undefined) this.terminal.resize(action.resize.columns, action.resize.rows);
        if (action.editor !== undefined) this.editorText.setText(action.editor);
        if (action.pending !== undefined) this.pendingText.setText(action.pending);
        if (action.editor !== undefined || action.pending !== undefined) this.tui.requestRender();
        await this.terminal.waitForRender();
        const screen: string[] = (this.terminal.getViewport() as string[]).map((line) => line.trimEnd());
        const nonBlank = screen.filter((line) => line.trim() !== "");
        const lastTwoRows = nonBlank.slice(-2);
        return {
            action: action.name,
            rows: this.terminal.rows,
            columns: this.terminal.columns,
            screen,
            lastTwoRows,
            footerRowsVisible:
                lastTwoRows.length === 2 &&
                lastTwoRows[0].includes("cpp-harness") &&
                lastTwoRows[1].includes("/128k (auto)"),
            transcript: {
                scrollTop: this.transcript.scrollTop,
                viewportHeight: this.transcript.viewportHeight,
                isFollowingEnd: this.transcript.isFollowingEnd,
            },
        };
    }

    stop() {
        this.tui.stop();
    }
}

const scenario = async (name: string, inputs: DockInputs, actions: StepAction[]) => {
    const viewport = new Viewport(inputs);
    const steps = [];
    for (const action of actions) steps.push(await viewport.apply(action));
    viewport.stop();
    return {
        name,
        dimensions: { columns: inputs.columns, rows: inputs.rows },
        inputs: {
            document: inputs.document,
            pending: inputs.pending ?? "",
            status: inputs.status ?? "",
            editor: inputs.editor,
            footer: inputs.footer,
            steps: actions,
        },
        steps,
    };
};

/// The product's footer at 50x30: the workspace path row over the context and
/// model stats row.
const productFooter = [
    "/tmp/cpp-harness-catch2-2625741-301292044582848...",
    "0.0%/128k (auto)                        fake-model",
];
const productDocument = Array.from({ length: 40 }, (_, index) => `transcript row ${index + 1}`);
const fitInputs: DockInputs = {
    columns: 50,
    rows: 30,
    document: productDocument,
    status: "── Working ──",
    editor: "prompt",
    footer: productFooter,
};
const overBudgetInputs: DockInputs = {
    ...fitInputs,
    pending: queued(20),
};

const scenarios = [
    // The ticket's four steps with a dock that fits: growth alone never costs
    // a footer row in the frozen product.
    await scenario("dock-fits-autocomplete-and-multiline", fitInputs, [
        { name: "baseline" },
        { name: "autocomplete-open", editor: menuBlock("/") },
        { name: "nine-line-paste", editor: NINE_LINES.join("\n") },
        { name: "submit", editor: "" },
        { name: "resize-60x30", resize: { columns: 60, rows: 30 } },
    ]),
    // Twenty queued messages make the dock's intrinsic height exceed the frame
    // once the editor block grows; this is the frame the ticket reports.
    await scenario("dock-over-budget-editor-growth", overBudgetInputs, [
        { name: "baseline" },
        { name: "autocomplete-open", editor: menuBlock("/") },
        { name: "nine-line-paste", editor: NINE_LINES.join("\n") },
        { name: "autocomplete-over-nine-lines", editor: menuBlock(NINE_LINES.join("\n")) },
        { name: "submit", editor: "" },
        { name: "resize-60x30", resize: { columns: 60, rows: 30 } },
    ]),
];

const replacements: Array<[string, string]> = [[path.resolve(piCheckout).split(path.sep).join("/"), "<pi-checkout>"]];
const normalize = (value: unknown): unknown => {
    let text = JSON.stringify(value);
    for (const [from, to] of replacements) text = text.split(from).join(to);
    return JSON.parse(text);
};

const artifact = normalize({
    schema: "pike/pi-v1.3.0-audit-scenarios/1",
    baseline,
    revision,
    family,
    capturedAt: new Date().toISOString(),
    sourceEndpoints: [
        "packages/coding-agent/src/modes/interactive/chat-viewport.ts:createChatViewport",
        "packages/tui/src/components/stack.ts:VStack,distribute",
        "packages/tui/src/layout.ts:layoutComponent,updateClips",
        "packages/tui/test/virtual-terminal.ts:VirtualTerminal",
    ],
    scenarios,
});
const artifactBytes = `${JSON.stringify(artifact, null, 2)}\n`;
const artifactName = "dock-footer.json";
writeFileSync(path.join(familyDir, artifactName), artifactBytes);
const sha256 = (bytes: string) => createHash("sha256").update(bytes).digest("hex");

const manifest = normalize({
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline,
    revision,
    ticket: "831",
    family,
    capturedAt: artifact.capturedAt,
    environment: {
        platform: process.platform,
        arch: process.arch,
        libc: "glibc",
        glibcVersion: glibc,
        node: process.version,
        tsx: tsxVersion,
    },
    generator: {
        path: relativeScript.split(path.sep).join("/"),
        sha256: sha256(readFileSync(scriptPath)),
        command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baseline} <pi-checkout>/node_modules/.bin/tsx ${relativeScript.split(path.sep).join("/")}`,
        sources: [
            {
                path: relativeScript.split(path.sep).join("/"),
                sha256: sha256(readFileSync(scriptPath)),
            },
        ],
    },
    artifacts: [
        {
            path: artifactName,
            family,
            bytes: Buffer.byteLength(artifactBytes),
            sha256: sha256(artifactBytes),
            sourceEndpoints: artifact.sourceEndpoints,
            scenarioCount: scenarios.length,
        },
    ],
});
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(
    `${family}: captured ${scenarios.length} scenarios ` +
        `(${scenarios.flatMap((entry) => entry.steps).length} frames) into ${artifactName}`,
);
