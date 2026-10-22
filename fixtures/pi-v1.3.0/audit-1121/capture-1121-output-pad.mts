#!/usr/bin/env tsx
/**
 * Capture the `audit-1121` output-pad family against the frozen `pi-v1.3.0` baseline (#1121, B63).
 *
 * HONEST SCOPE: this is a COMPONENT-LEVEL capture of pi's three output-pad-owning chat blocks.
 * The frozen `ToolExecutionComponent`, `BashExecutionComponent` and `CustomMessageComponent` are
 * constructed directly with an explicit `outputPad` of 1 and 0 and rendered at widths 32, 60
 * and 100; the narrow render wraps each block's content while the wide render does not. No model turn, no gateway,
 * no network and no interactive session are involved.
 *
 * What it freezes is the property pike's product layer had to reproduce: pi's `outputPad` reaches
 * the tool block's box padding, the bash block's header/body/status text padding and the custom
 * block's box padding. A user-message-only implementation passes the user block and fails every
 * block captured here.
 *
 * The bundle stays UNREGISTERED: the parent or serial merger applies the registry patch through
 * `scripts/evidence/audit_evidence.py register`.
 *
 * Usage (from the repository root):
 *   NODE_OPTIONS=--conditions=source PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1121/capture-1121-output-pad.mts
 */

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1121";
const artifactName = "output-pad.json";
const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi"));
const baselineName = process.env.PI_BASELINE ?? "pi-v1.3.0";
const baselineRevision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const widths = [32, 60, 100];
const toolCommand =
    "echo wrap-tool audit payload with enough words to continue onto another terminal row and test width-dependent wrapping";
const bashCommand =
    "echo wrap-bash audit command with enough words to continue onto another terminal row and test width-dependent wrapping";
const bashOutput =
    "wrap-bash output carries additional words so the body wraps at the narrow width and keeps measuring differently at every tested terminal width";
const customContent =
    "wrap-custom content carries additional words so this custom message wraps at the narrow width and keeps measuring differently at every tested terminal width";

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
if (spawnSync("git", ["-C", piCheckout, "status", "--porcelain"], { encoding: "utf8" }).stdout.trim()) {
    throw new Error("pi capture requires an unmodified source checkout");
}

const { ToolExecutionComponent } = await import(
    pathToFileURL(
        path.join(piCheckout, "packages/coding-agent/src/modes/interactive/components/tool-execution.ts"),
    ).href
);
const { BashExecutionComponent } = await import(
    pathToFileURL(
        path.join(piCheckout, "packages/coding-agent/src/modes/interactive/components/bash-execution.ts"),
    ).href
);
const { CustomMessageComponent } = await import(
    pathToFileURL(
        path.join(piCheckout, "packages/coding-agent/src/modes/interactive/components/custom-message.ts"),
    ).href
);
const { initTheme } = await import(
    pathToFileURL(path.join(piCheckout, "packages/coding-agent/src/modes/interactive/theme/theme.ts")).href
);
initTheme(undefined, false);

/// The minimum TUI surface pi's blocks touch: the loader needs an interval
/// pair and a render request; nothing here reads a model or the network.
function tuiStub(width: number): any {
    return {
        terminal: {
            get columns() {
                return width;
            },
            get rows() {
                return 24;
            },
        },
        addInterval: (_callback: () => void, _milliseconds: number) => ({ dispose: () => {} }),
        removeInterval: () => {},
        requestRender: () => {},
    };
}

/// The visible row text of a rendered block, with the SGR styling removed so
/// the comparison is the geometry (the leading pad) rather than the palette.
function visibleRows(lines: string[]): string[] {
    return lines.map((line) => line.replace(/\x1b\[[0-9;]*m/g, ""));
}

function renderTool(outputPad: number, width: number): string[] {
    const component = new ToolExecutionComponent(
        "audit-tool",
        "call-audit-1121",
        { command: toolCommand },
        { outputPad },
        undefined,
        tuiStub(width),
        "/tmp",
    );
    return visibleRows(component.render(width));
}

function renderBash(outputPad: number, width: number): string[] {
    const component = new BashExecutionComponent(bashCommand, tuiStub(width), false, outputPad);
    component.appendOutput(`${bashOutput}\n`);
    component.setComplete(0, false);
    return visibleRows(component.render(width));
}

function renderCustom(outputPad: number, width: number): string[] {
    const message: any = {
        role: "custom",
        customType: "audit",
        content: customContent,
        display: true,
        timestamp: 0,
    };
    const component = new CustomMessageComponent(message, undefined, undefined, outputPad);
    return visibleRows(component.render(width));
}

function renderAtWidths(render: (outputPad: number, width: number) => string[]) {
    return Object.fromEntries(
        widths.map((width) => {
            const atOne = render(1, width);
            const atZero = render(0, width);
            return [String(width), { atOne, atZero, lineCountAtOne: atOne.length, lineCountAtZero: atZero.length }];
        }),
    );
}

const toolAtWidths = renderAtWidths(renderTool);
const bashAtWidths = renderAtWidths(renderBash);
const customAtWidths = renderAtWidths(renderCustom);

for (const [name, rendered] of [
    ["tool", toolAtWidths],
    ["bash", bashAtWidths],
    ["custom", customAtWidths],
] as const) {
    if (rendered["32"].atOne.length <= rendered["100"].atOne.length ||
        rendered["60"].atOne.length <= rendered["100"].atOne.length) {
        throw new Error(`${name} block did not wrap more at narrow widths than at 100 columns`);
    }
}

/// The content row that carries the block's own text, and the number of
/// leading spaces before it. The pad is the property under test: it is the
/// difference between the pad-1 and pad-0 rows.
function contentRow(rows: string[], needle: string): string {
    const found = rows.find((row) => row.includes(needle));
    if (found === undefined) throw new Error(`no rendered row carries ${needle}`);
    return found;
}

function rowsBetween(rows: string[], first: string, last: string): string[] {
    const firstIndex = rows.findIndex((row) => row.includes(first));
    const lastIndex = rows.findIndex((row, index) => index >= firstIndex && row.includes(last));
    if (firstIndex < 0 || lastIndex < firstIndex) throw new Error(`no rows run from ${first} through ${last}`);
    return rows.slice(firstIndex, lastIndex + 1);
}

const capturedAt = new Date().toISOString();
const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    recordKind: "component-probe",
    baseline: baselineName,
    revision: baselineRevision,
    capturedAt,
    environment: {
        platform: process.platform,
        arch: process.arch,
        libc: "glibc",
        glibcVersion: (process.report.getReport() as any).header.glibcVersionRuntime,
        node: process.version,
        tsx: "4.22.1",
    },
    sourceEndpoints: [
        "packages/coding-agent/src/modes/interactive/components/tool-execution.ts:ToolExecutionComponent.setOutputPad,updateDisplay",
        "packages/coding-agent/src/modes/interactive/components/bash-execution.ts:BashExecutionComponent.setOutputPad,updateDisplay",
        "packages/coding-agent/src/modes/interactive/components/custom-message.ts:CustomMessageComponent.setOutputPad,rebuild",
        "packages/tui/src/components/box.ts:Box.setPaddingX,render",
        "packages/tui/src/components/text.ts:Text.setPaddingX,render",
    ],
    probe:
        "the frozen tool-execution, bash-execution and custom-message components are constructed " +
        "directly with outputPad 1 and 0 and rendered at 32, 60 and 100 columns; the observed rows " +
        "are the frozen pi geometry, with SGR styling stripped so content wrapping and padding are visible",
    scenarios: [
        {
            name: "output-pad-reaches-the-tool-block-box",
            inputs: {
                component: "ToolExecutionComponent",
                tool: "audit-tool",
                toolCallId: "call-audit-1121",
                arguments: { command: toolCommand },
                outputPad: [1, 0],
                widths,
            },
            expected: {
                atWidths: toolAtWidths,
                contentRowAtOne: contentRow(toolAtWidths["60"].atOne, "audit-tool"),
                contentRowAtZero: contentRow(toolAtWidths["60"].atZero, "audit-tool"),
                contentRowAtOneLeadingSpaces: contentRow(toolAtWidths["60"].atOne, "audit-tool").search(/\S/),
                contentRowAtZeroLeadingSpaces: contentRow(toolAtWidths["60"].atZero, "audit-tool").search(/\S/),
                wrappedCommandRowsAtNarrowWidth: rowsBetween(toolAtWidths["32"].atOne, "wrap-tool", "width-dependent wrapping"),
            },
            normalizations: ["SGR styling is stripped; the compared bytes are the visible cell text"],
        },
        {
            name: "output-pad-reaches-the-bash-block-text",
            inputs: {
                component: "BashExecutionComponent",
                command: bashCommand,
                output: bashOutput,
                exitCode: 0,
                outputPad: [1, 0],
                widths,
            },
            expected: {
                atWidths: bashAtWidths,
                headerRowAtOne: contentRow(bashAtWidths["60"].atOne, "$ echo wrap-bash"),
                headerRowAtZero: contentRow(bashAtWidths["60"].atZero, "$ echo wrap-bash"),
                headerRowAtOneLeadingSpaces: contentRow(bashAtWidths["60"].atOne, "$ echo wrap-bash").search(/\S/),
                headerRowAtZeroLeadingSpaces: contentRow(bashAtWidths["60"].atZero, "$ echo wrap-bash").search(/\S/),
                wrappedOutputRowsAtNarrowWidth: rowsBetween(bashAtWidths["32"].atOne, "wrap-bash output", "every tested terminal width"),
            },
            normalizations: ["SGR styling is stripped; the compared bytes are the visible cell text"],
        },
        {
            name: "output-pad-reaches-the-custom-block-box",
            inputs: {
                component: "CustomMessageComponent",
                customType: "audit",
                content: customContent,
                outputPad: [1, 0],
                widths,
            },
            expected: {
                atWidths: customAtWidths,
                labelRowAtOne: contentRow(customAtWidths["60"].atOne, "[audit]"),
                labelRowAtZero: contentRow(customAtWidths["60"].atZero, "[audit]"),
                labelRowAtOneLeadingSpaces: contentRow(customAtWidths["60"].atOne, "[audit]").search(/\S/),
                labelRowAtZeroLeadingSpaces: contentRow(customAtWidths["60"].atZero, "[audit]").search(/\S/),
                wrappedContentRowsAtNarrowWidth: rowsBetween(customAtWidths["32"].atOne, "wrap-custom", "terminal width"),
            },
            normalizations: ["SGR styling is stripped; the compared bytes are the visible cell text"],
        },
    ],
};

writeFileSync(path.join(familyDir, artifactName), `${JSON.stringify(artifact, null, 2)}\n`, "utf8");
process.stdout.write(
    `captured ${family}/${artifactName} at ${capturedAt} (tool/bash/custom at outputPad 1 and 0; widths ${widths.join(", ")})\n`,
);
process.exit(0);
