#!/usr/bin/env tsx
/**
 * Session- and value-suite differential-golden capture (issue #421, the
 * session-family differential goldens of the pi-coding-agent gate, ADR 0036).
 *
 * The committed snapshots under `fixtures/pi-coding-agent/sessions/` pin the
 * FROZEN pi behavior (message/content-block level) for the five session
 * families: session lifecycle, resume, compaction, model switching, and the
 * session-family flows. This sidecar drives the frozen `packages/coding-agent`
 * sources (via `session-scenarios.mts`, spawned with a capture tsconfig that
 * maps the `@earendil-works/*` workspace packages to the frozen `packages/`
 * sources) and regenerates the committed snapshots deterministically: it
 * captures, then re-captures to a temp directory and byte-compares, so a
 * nondeterministic capture fails loudly.
 *
 * The C++ comparator (`tests/coding_agent/runtime/SessionSuiteGoldenTest.cpp`)
 * drives the same scenarios through the C++ session runtime and byte-compares
 * against these committed snapshots through the same canonical projection.
 *
 * Usage (from anywhere):
 *   ../pi/node_modules/.bin/tsx fixtures/pi-coding-agent/capture/session-snapshots.mts
 * `PI_CHECKOUT` overrides the pi source checkout (default: sibling `../pi`).
 * The pi checkout MUST sit at the frozen parity baseline commit and declare
 * the pinned artifact version; the script refuses to run otherwise.
 *
 * The frozen checkout needs the same dev prerequisites pi's own test suite
 * needs to run: a `node_modules` (the script symlinks the sibling checkout's
 * when absent) and the generated `packages/ai/src/providers/data/` directory
 * (gitignored, produced by pi's `npm run hydrate:model-data`; the script
 * provisions it from the sibling checkout when absent — the model data never
 * enters a captured value, the faux scenarios are driven by
 * `registerFauxProvider` with in-memory registries).
 */

import { execFileSync } from "node:child_process";
import {
	cpSync,
	existsSync,
	linkSync,
	mkdirSync,
	readFileSync,

	rmSync,
	writeFileSync,
} from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

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
const sessionsDir = path.join(fixtureDir, "sessions");
const repoRoot = path.resolve(fixtureDir, "../..");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repoRoot, "../pi");
const FROZEN_COMMIT = "83114817c68f5413e4d7ba6d7003ddc511cd31d2";
const PINNED_PACKAGE = "@earendil-works/pi-coding-agent";
const PINNED_VERSION = "0.83.0";

// ── Frozen-checkout guard (same as the parent sidecar) ─────────────────────

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

// ── Frozen-checkout dev prerequisites ───────────────────────────────────────

// 1. node_modules: a frozen worktree has none; symlink the sibling checkout's
//    (the canonical local pi source) so third-party deps resolve. The capture
//    tsconfig below maps the `@earendil-works/*` workspace packages to the
//    frozen sources ahead of that node_modules, so nothing from the sibling
//    checkout's packages can leak in.
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

// 2. Generated model data: `packages/ai/src/providers/data/` is gitignored and
//    produced by pi's own `hydrate:model-data`; the coding-agent sources import
//    it at module load. The faux scenarios never read it, so provisioning from
//    the sibling checkout (or a minimal stub for the one missing file) is
//    deterministic and value-neutral.
const dataDir = path.join(piCheckout, "packages/ai/src/providers/data");
if (!existsSync(dataDir)) {
	const siblingDataDir = path.join(
		path.dirname(piCheckout), "pi/packages/ai/src/providers/data",
	);
	if (existsSync(siblingDataDir)) {
		mkdirSync(path.dirname(dataDir), { recursive: true });
		cpSync(siblingDataDir, dataDir, { recursive: true });
		console.log(`provisioned ${dataDir} from the sibling checkout`);
	} else {
		mkdirSync(dataDir, { recursive: true });
	}
}
// baseten.json is absent even from hydrated sibling checkouts (generated from
// models.dev network data at hydrate time); the module only needs it to load.
const basetenJson = path.join(dataDir, "baseten.json");
if (!existsSync(basetenJson)) {
	writeFileSync(basetenJson, "{}\n");
}

// ── Capture tsconfig: map the workspace packages to the frozen sources ──────

const tsconfigPath = path.join(piCheckout, "tsconfig.capture.json");
writeFileSync(
	tsconfigPath,
	JSON.stringify(
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
				baseUrl: ".",
				paths: {
					"@earendil-works/pi-ai": ["./packages/ai/src/index.ts"],
					"@earendil-works/pi-ai/*": ["./packages/ai/src/*.ts"],
					"@earendil-works/pi-agent-core": ["./packages/agent/src/index.ts"],
					"@earendil-works/pi-tui": ["./packages/tui/src/index.ts"],
					"@earendil-works/pi-client": ["./packages/client/src/index.ts"],
					"@earendil-works/pi-protocol": ["./packages/protocol/src/index.ts"],
				},
			},
		},
		null,
		2,
	) + "\n",
);

// ── Run the scenario driver: capture, then byte-verify determinism ──────────

const scenarioScript = path.join(scriptDir, "session-scenarios.mts");
const tsxBin = path.join(piCheckout, "node_modules/.bin/tsx");
const captureEnv = {
	...process.env,
	HOME: "/home/tester",
	USERPROFILE: "/home/tester",
	PI_CHECKOUT: piCheckout,
	CCH_SESSION_FIXTURE_DIR: sessionsDir,
};

// Capture pass: write the committed snapshots.
execFileSync(tsxBin, ["--tsconfig", tsconfigPath, scenarioScript], {
	stdio: "inherit",
	env: captureEnv,
	cwd: repoRoot,
});

// Verify pass: re-capture to a temp directory and byte-compare. A
// nondeterministic capture fails loudly here.
const verifyDir = path.join(fixtureDir, ".verify");
rmSync(verifyDir, { recursive: true, force: true });
mkdirSync(verifyDir, { recursive: true });
const verifyTarget = path.join(verifyDir, "sessions");
mkdirSync(verifyTarget, { recursive: true });
execFileSync(
	tsxBin,
	["--tsconfig", tsconfigPath, scenarioScript],
	{
		stdio: "inherit",
		env: { ...captureEnv, CCH_SESSION_FIXTURE_DIR: verifyTarget },
		cwd: repoRoot,
	},
);
const expected = ["session-lifecycle", "session-resume", "session-compaction", "session-model-switch", "session-family"];
for (const name of expected) {
	const committed = readFileSync(path.join(sessionsDir, `${name}.json`), "utf8");
	const fresh = readFileSync(path.join(verifyTarget, `${name}.json`), "utf8");
	if (committed !== fresh) {
		throw new Error(
			`nondeterministic session capture: ${name}.json differs between ` +
				`capture passes`,
		);
	}
}
rmSync(verifyDir, { recursive: true, force: true });

console.log(
	"wrote fixtures/pi-coding-agent/sessions/session-*.json " +
		"(capture + byte-verify pass both green)",
);
