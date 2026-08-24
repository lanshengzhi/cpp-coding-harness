#!/usr/bin/env tsx
/**
 * System Prompt message-level differential-golden capture (issue #422, P26,
 * the System Prompt golden of the pi-coding-agent gate, ADR 0036).
 *
 * The committed snapshots under `fixtures/pi-coding-agent/prompts/` pin the
 * FROZEN pi `buildSystemPrompt` output (baseline 83114817) at MESSAGE level —
 * a `system` message with one text content block, the same message/content-
 * block projection the session/value suites use — for the three scripted
 * scenarios the C++ `SystemPromptBuilderTest` already pins as raw text
 * (default branch, custom branch, empty-tools edge). Each snapshot carries the
 * `identityDelta`: the identity line and the documentation block in both
 * forms (pi vs the C++ binary's own "pike"), which is exactly the delta the
 * C++ System Prompt carries (ADR 0036 G4 / #392: structure byte-identical,
 * identity lines swapped). The custom branch carries no identity regions, so
 * its delta is empty and the two branches are pinned byte-identical.
 *
 * The C++ comparator (`tests/coding_agent/SystemPromptGoldenTest.cpp`) builds
 * the C++ prompt through `SystemPromptBuilder` with the identical inputs,
 * swaps the delta regions into pi's message, and byte-compares — so the
 * golden pins "structure byte-identical, identity lines swapped" as a
 * differential check, not just a C++-side byte pin.
 *
 * The pi docs paths are scrubbed to `/pi/*` via `PI_PACKAGE_DIR=/pi` (the
 * same scrubbing rule the rest of the bundle applies), so the captured
 * message is deterministic across checkouts; the C++ side builds against the
 * scrubbed `/pike/*` paths. The script captures, then re-captures to a temp
 * directory and byte-compares, so a nondeterministic capture fails loudly.
 *
 * Usage (from anywhere):
 *   ../pi/node_modules/.bin/tsx fixtures/pi-coding-agent/capture/system-prompt-snapshots.mts
 * `PI_CHECKOUT` overrides the pi source checkout (default: sibling `../pi`).
 * The pi checkout MUST sit at the frozen parity baseline commit and declare
 * the pinned artifact version; the script refuses to run otherwise. The
 * frozen checkout needs a resolvable `node_modules` for `buildSystemPrompt`'s
 * transitive import of `ignore` (via `skills.ts`); a frozen worktree can
 * symlink the sibling checkout's like the session sidecar does.
 */

import { execFileSync } from "node:child_process";
import {
	existsSync,
	linkSync,
	mkdirSync,
	readFileSync,
	rmSync,
	writeFileSync,
} from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// Deterministic environment before any module that reads the home directory
// or terminal capabilities runs (same pins as the pi-ai/pi-tui sidecars).
process.env.HOME = "/home/tester";
process.env.USERPROFILE = "/home/tester";
delete process.env.TERM;
delete process.env.TERM_PROGRAM;
delete process.env.TERM_PROGRAM_VERSION;
delete process.env.KITTY_WINDOW_ID;
delete process.env.WEZTERM_PANE;
delete process.env.TMUX;

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const fixtureDir = path.resolve(scriptDir, "..");
const promptsDir = path.join(fixtureDir, "prompts");
const repoRoot = path.resolve(fixtureDir, "../..");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repoRoot, "../pi");
const FROZEN_COMMIT = "83114817c68f5413e4d7ba6d7003ddc511cd31d2";
const PINNED_PACKAGE = "@earendil-works/pi-coding-agent";
const PINNED_VERSION = "0.83.0";

// ── Frozen-checkout guard (same as the parent/session sidecars) ─────────────

const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], {
	encoding: "utf8",
}).trim();
if (head !== FROZEN_COMMIT) {
	throw new Error(
		`pi checkout must be at the frozen baseline ${FROZEN_COMMIT}, found ${head}`,
	);
}

const codingAgentPackage = JSON.parse(
	readFileSync(path.join(piCheckout, "packages/coding-agent/package.json"), "utf8"),
) as { name: string; version: string };
if (
	codingAgentPackage.name !== PINNED_PACKAGE ||
	codingAgentPackage.version !== PINNED_VERSION
) {
	throw new Error(
		`frozen checkout must declare ${PINNED_PACKAGE}@${PINNED_VERSION}, ` +
			`found ${codingAgentPackage.name}@${codingAgentPackage.version}`,
	);
}

// The frozen checkout needs a resolvable node_modules for `ignore` (imported
// transitively by `skills.ts` at module load). A frozen worktree has none;
// symlink the sibling checkout's like the session sidecar does.
const frozenNodeModules = path.join(piCheckout, "node_modules");
if (!existsSync(frozenNodeModules)) {
	const siblingNodeModules = path.join(path.dirname(piCheckout), "pi/node_modules");
	if (!existsSync(siblingNodeModules)) {
		throw new Error(
			`frozen checkout has no node_modules and no sibling checkout to link ` +
				`(${siblingNodeModules}); run the pi dev install inside the frozen ` +
				`checkout first (npm install)`,
		);
	}
	linkSync(siblingNodeModules, frozenNodeModules, "dir");
	console.log(`linked ${frozenNodeModules} -> ${siblingNodeModules}`);
}

// ── Pinned scenario inputs (must match the C++ SystemPromptBuilderTest / ────
// ── SystemPromptGoldenTest inputs byte-for-byte) ───────────────────────────

const SNIPPETS: Record<string, string> = {
	read: "Read file contents",
	bash: "Execute bash commands (ls, grep, find, etc.)",
	edit: "Make precise file edits with exact text replacement, including " +
		"multiple disjoint edits in one call",
	write: "Create or overwrite files",
};

const GUIDELINES = [
	"Use read to examine files instead of cat or sed.",
	"Inspect PI_* environment variables for current model and session details.",
	"Use edit for precise changes (edits[].oldText must match exactly)",
	"When changing multiple separate locations in one file, use one edit " +
		"call with multiple entries in edits[] instead of multiple edit calls",
	"Each edits[].oldText is matched against the original file, not after " +
		"earlier edits are applied. Do not emit overlapping or nested edits. " +
		"Merge nearby changes into one edit.",
	"Keep edits[].oldText as small as possible while still being unique " +
		"in the file. Do not pad with large unchanged regions.",
	"Use write only for new files or complete rewrites.",
];

const SKILL = {
	name: "my-skill",
	description: "Do things.",
	filePath: "/home/user/.agents/skills/my-skill/SKILL.md",
	baseDir: "/home/user/.agents/skills/my-skill",
	disableModelInvocation: false,
};

// ── Capture ────────────────────────────────────────────────────────────────

// PI_PACKAGE_DIR scrubs pi's resolved docs paths to the deterministic
// `/pi/*` form (the bundle's path-scrubbing rule), so the captured message is
// checkout-independent; the C++ side builds against the `/pike/*` paths.
process.env.PI_PACKAGE_DIR = "/pi";

const { buildSystemPrompt } = (await import(
	pathToFileURL(
		path.join(piCheckout, "packages/coding-agent/src/core/system-prompt.ts"),
	).href,
)) as { buildSystemPrompt: (options: unknown) => string };

/// The Pike identity forms of a region: the "pi" identity word becomes the
/// C++ binary's own "pike", word-bounded so embedded "pi" substrings (e.g.
/// "topics", "pipelines") are untouched, and the scrubbed `/pi/*` docs paths
/// become `/pike/*` through the same word-boundary rule.
function pikeify(text: string): string {
	return text.replace(/\bPi\b/g, "pike").replace(/\bpi\b/g, "pike");
}

const DOCS_END_MARKER = "tui.md for TUI API details)";

/// Extracts the default-branch identity regions from a built prompt, or
/// null for the custom branch (which carries no identity line/docs block).
function extractIdentityDelta(prompt: string) {
	const identityStart = prompt.indexOf(
		"You are an expert coding assistant operating inside pi,",
	);
	const docsStart = prompt.indexOf("Pi documentation (read only");
	if (identityStart !== 0 || docsStart < 0) {
		// Custom branch: no identity regions (pi truthy customPrompt).
		return null;
	}
	const identityLinePi = prompt.slice(0, prompt.indexOf("\n"));
	const docsEnd = prompt.indexOf(DOCS_END_MARKER) + DOCS_END_MARKER.length;
	const docsBlockPi = prompt.slice(docsStart, docsEnd);
	return {
		identityLine: { pi: identityLinePi, pike: pikeify(identityLinePi) },
		docsBlock: { pi: docsBlockPi, pike: pikeify(docsBlockPi) },
	};
}

function buildDefaultOptions() {
	return {
		cwd: "/tmp/workspace",
		selectedTools: ["read", "bash", "edit", "write"],
		toolSnippets: SNIPPETS,
		promptGuidelines: GUIDELINES,
		skills: [SKILL],
	};
}

interface Scenario {
	name: string;
	options: {
		cwd: string;
		customPrompt?: string;
		appendSystemPrompt?: string;
		contextFiles?: Array<{ path: string; content: string }>;
		selectedTools?: string[];
		toolSnippets?: Record<string, string>;
		promptGuidelines?: string[];
		skills?: unknown[];
	};
}

/// The three scripted scenarios (must match the C++ SystemPromptGoldenTest
/// inputs byte-for-byte): the default branch with the four fixed tools and a
/// skill, the custom branch with append + context files, and the default
/// branch with an explicitly empty tool set.
function scenarios(): Scenario[] {
	return [
		{
			name: "default",
			options: buildDefaultOptions(),
		},
		{
			name: "custom",
			options: {
				cwd: "/tmp/workspace",
				customPrompt: "You are a custom assistant.",
				appendSystemPrompt: "Custom append section.",
				contextFiles: [
					{
						path: "AGENTS.md",
						content: "Project-specific instructions and guidelines:\n\nBe careful.",
					},
				],
				skills: [SKILL],
			},
		},
		{
			name: "empty-tools",
			options: {
				cwd: "/tmp/workspace",
				selectedTools: [],
				toolSnippets: {},
				promptGuidelines: GUIDELINES,
				skills: [],
			},
		},
	];
}

function captureScenario(scenario: Scenario): unknown {
	const prompt = buildSystemPrompt(scenario.options);
	const identityDelta = extractIdentityDelta(prompt);
	return {
		meta: {
			baseline: FROZEN_COMMIT,
			artifact: `${PINNED_PACKAGE}@${PINNED_VERSION}`,
			family: `system-prompt-message-${scenario.name}`,
		},
		message: {
			role: "system",
			content: [{ type: "text", text: prompt }],
		},
		identityDelta: identityDelta ?? {},
	};
}

function captureTo(directory: string): void {
	mkdirSync(directory, { recursive: true });
	for (const scenario of scenarios()) {
		writeFileSync(
			path.join(directory, `system-prompt-${scenario.name}-message.json`),
			JSON.stringify(captureScenario(scenario), null, 2) + "\n",
		);
	}
}

// Capture pass: write the committed snapshots.
captureTo(promptsDir);

// Verify pass: re-capture to a temp directory and byte-compare. A
// nondeterministic capture fails loudly here.
const verifyDir = path.join(fixtureDir, ".verify");
rmSync(verifyDir, { recursive: true, force: true });
mkdirSync(verifyDir, { recursive: true });
const verifyPrompts = path.join(verifyDir, "prompts");
mkdirSync(verifyPrompts, { recursive: true });
captureTo(verifyPrompts);
for (const scenario of scenarios()) {
	const committed = readFileSync(
		path.join(promptsDir, `system-prompt-${scenario.name}-message.json`),
		"utf8",
	);
	const fresh = readFileSync(
		path.join(verifyPrompts, `system-prompt-${scenario.name}-message.json`),
		"utf8",
	);
	if (committed !== fresh) {
		throw new Error(
			`nondeterministic system-prompt capture: ${scenario.name} differs ` +
				`between capture passes`,
		);
	}
}
rmSync(verifyDir, { recursive: true, force: true });

console.log(
	"wrote fixtures/pi-coding-agent/prompts/system-prompt-*-message.json " +
		"(capture + byte-verify pass both green)",
);
