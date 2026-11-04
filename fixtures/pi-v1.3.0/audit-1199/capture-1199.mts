#!/usr/bin/env tsx
/**
 * Capture the `audit-1199` frozen work-tree discovery and config-surface
 * observations for the shared pi-v1.3.0 evidence registry.
 *
 * These scenarios record how the frozen listFiles daemon
 * (`packages/env/daemon/src/list.rs`, walking with the Rust `ignore` crate)
 * discovers work trees and ignore-rule sources: `.git` directories and files
 * (linked worktrees via `gitdir:` + `commondir`), parent `.gitignore` files
 * from the listing root up to the work-tree root, `.git/info/exclude`, the
 * global excludes file resolved from git config (`GIT_CONFIG_GLOBAL`,
 * `$HOME/.gitconfig`, `$XDG_CONFIG_HOME/git/config`, `GIT_CONFIG_SYSTEM` /
 * `/etc/gitconfig`, then the XDG default), and the environment variables the
 * crate deliberately does not consult (`GIT_DIR`, `GIT_WORK_TREE`).
 *
 * Every scenario declares the exact environment its daemon ran with, so the
 * C++ differential side can reproduce it.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1199/capture-1199.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync, chmodSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1199";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi");

if (process.env.TSX_TSCONFIG_PATH === undefined) {
	const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
	const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
		stdio: "inherit",
		env: {
			...process.env,
			NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
			TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json"),
		},
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

let tsxVersion = "unknown";
try {
	tsxVersion = execFileSync(path.join(piCheckout, "node_modules", ".bin", "tsx"), ["--version"], {
		encoding: "utf8",
	}).trim();
} catch {
	// The tsx version is provenance, not a capture precondition.
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

/** The checkout's own pi-env daemon, built from the frozen sources when it is missing. */
function daemonBinary(): string {
	const daemonDirectory = path.join(piCheckout, "packages/env/daemon");
	const binary = path.join(daemonDirectory, "target/debug/pi-env");
	if (existsSync(binary)) return binary;
	execFileSync("cargo", ["build", "--locked"], { cwd: daemonDirectory, stdio: "inherit" });
	if (!existsSync(binary)) throw new Error(`cargo build did not produce ${binary}`);
	return binary;
}

const { BACKGROUND_CONTEXT } = await import("@earendil-works/chord/context");
const { getOrThrow } = await import("@earendil-works/pi-durable/env");
const { Connection, LocalExecutionEnv } = await import("@earendil-works/pi-env");

const context = BACKGROUND_CONTEXT;
const testRoot = mkdtempSync(path.join(tmpdir(), "cch-audit-1199-"));

/** Write `content` at `relative` below `root`, creating parent directories; a trailing `/` creates a directory. */
function write(root: string, relative: string, content: string | Uint8Array = ""): void {
	const target = path.join(root, relative);
	mkdirSync(path.dirname(target), { recursive: true });
	if (relative.endsWith("/")) {
		mkdirSync(target, { recursive: true });
		return;
	}
	writeFileSync(target, content);
}

/** The controlled base environment every scenario daemon runs with. */
const neutralEnv = {
	HOME: path.join(testRoot, "env-home"),
	XDG_CONFIG_HOME: path.join(testRoot, "env-xdg"),
	GIT_CONFIG_GLOBAL: path.join(testRoot, "env-no-such-global"),
	GIT_CONFIG_SYSTEM: path.join(testRoot, "env-no-such-system"),
};
mkdirSync(neutralEnv.HOME, { recursive: true });
mkdirSync(neutralEnv.XDG_CONFIG_HOME, { recursive: true });

function applyEnv(env: Record<string, string>) {
	for (const key of ["HOME", "XDG_CONFIG_HOME", "GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM", "GIT_DIR", "GIT_WORK_TREE"]) {
		if (key in env) process.env[key] = env[key];
		else delete process.env[key];
	}
}

function listed(listing: any) {
	return {
		entries: listing.entries.map((value: any) => ({ path: value.path, kind: value.kind })),
		truncated: listing.truncated,
	};
}

async function capture(
	name: string,
	files: Record<string, string | Uint8Array>,
	options: any,
	env: Record<string, string>,
	inputs: any,
	normalizations: string[] = [],
	listPath = ".",
) {
	const root = path.join(testRoot, "case-" + scenarios.length);
	for (const [relative, content] of Object.entries(files)) write(root, relative, content);
	applyEnv(env);
	const connection = new Connection({ command: [daemonBinary()] });
	try {
		const execEnv = new LocalExecutionEnv({ cwd: root, connection });
		const listing = getOrThrow(await execEnv.listFiles(listPath, options, context));
		// The recorded inputs describe the whole tree the daemon listed, so a
		// test can rebuild it exactly.
		scenarios.push({ name, inputs: { ...inputs, files }, expected: listed(listing), normalizations });
	} finally {
		await connection.close();
	}
}

const scenarios: any[] = [];
const HEAD = "ref: refs/heads/main\n";
const gitDir = { ".git/HEAD": HEAD };

// Work-tree detection: a root outside any work tree applies no repository
// rules, even with ignore files present; the same tree with `.git` applies
// them (the discriminating case pairs with the audit-1198 syntax cases).
await capture(
	"a root outside any work tree applies no repository rules",
	{ ".gitignore": "drop.txt\n", "drop.txt": "", "keep.txt": "" },
	{ gitignore: true },
	neutralEnv,
	{ files: { ".gitignore": "drop.txt\n" }, hasDotGit: false, options: { gitignore: true } },
);
await capture(
	"a git directory enables the repository rules",
	{ ...gitDir, ".gitignore": "drop.txt\n", "drop.txt": "", "keep.txt": "" },
	{ gitignore: true },
	neutralEnv,
	{ files: { ".gitignore": "drop.txt\n" }, hasDotGit: true, options: { gitignore: true } },
);

// Parent .gitignore files from the listing root up to the work-tree root.
// The listing root is a subdirectory of the inner work tree; the inner rules
// apply, while the outer repository's rules stop at the inner work-tree root.
await capture(
	"parent gitignores apply up to the work-tree root and no further",
	{
		".git/HEAD": HEAD,
		".gitignore": "from-outer.txt\nleak.txt\n",
		"from-outer.txt": "",
		"wt/.git/HEAD": HEAD,
		"wt/.gitignore": "from-wt.txt\n",
		"wt/from-wt.txt": "",
		"wt/sub/from-wt.txt": "",
		"wt/sub/leak.txt": "",
		"wt/sub/keep.txt": "",
	},
	{ gitignore: true },
	neutralEnv,
	{
		files: {
			".gitignore": "from-outer.txt\nleak.txt\n (the outer repository also has .git)",
			"wt/.gitignore": "from-wt.txt\n",
		},
		listingRoot: "wt/sub",
		options: { gitignore: true },
	},
	["the fixture is listed from its wt/sub directory; entry paths stay relative to that root"],
	"wt/sub",
);

// `.git` as a file (linked worktree): the gitdir file and its commondir point
// at the real git directory; the exclude file is read from the commondir.
{
	const root = path.join(testRoot, "case-linked");
	const mainGit = path.join(root, "main/.git");
	write(mainGit, "HEAD", HEAD);
	write(mainGit, "info/exclude", "excluded-by-common.txt\n");
	write(mainGit, "worktrees/linked/commondir", "../..\n");
	const linkedGit = path.join(mainGit, "worktrees/linked");
	const files: Record<string, string> = {
		"linked/.git": `gitdir: ${linkedGit}\n`,
		"linked/.gitignore": "dropped.txt\n",
		"linked/dropped.txt": "",
		"linked/excluded-by-common.txt": "",
		"linked/keep.txt": "",
	};
	for (const [relative, content] of Object.entries(files)) write(root, relative, content);
	applyEnv(neutralEnv);
	const connection = new Connection({ command: [daemonBinary()] });
	try {
		const execEnv = new LocalExecutionEnv({ cwd: root, connection });
		const listing = getOrThrow(await execEnv.listFiles("linked", { gitignore: true }, context));
		scenarios.push({
			name: "a git file worktree reads info-exclude from the commondir",
			inputs: {
				files,
				gitdir: "<absolute, written into linked/.git>",
				commondir: "<gitdir>/commondir -> ../..",
				exclude: "excluded-by-common.txt\n",
				options: { gitignore: true },
			},
			expected: listed(listing),
			normalizations: ["the .git gitdir path is absolute; the exclude file is read through the commondir"],
		});
	} finally {
		await connection.close();
	}
}

// Nested repositories: the inner repository's rules apply inside it and the
// outer repository's rules stop at the inner work-tree boundary.
await capture(
	"a nested repository applies its own rules and blocks the outer ones",
	{
		...gitDir,
		".gitignore": "root-only.txt\nshared.txt\n",
		"root-only.txt": "",
		"shared.txt": "",
		"inner/.git/HEAD": HEAD,
		"inner/.gitignore": "!shared.txt\ninner-only.txt\n",
		"inner/shared.txt": "",
		"inner/inner-only.txt": "",
		"inner/root-only.txt": "",
		"inner/keep.txt": "",
	},
	{ gitignore: true },
	neutralEnv,
	{
		files: {
			".gitignore": "root-only.txt\nshared.txt\n",
			"inner/.gitignore": "!shared.txt\ninner-only.txt\n",
		},
		options: { gitignore: true },
	},
);

// The ignore crate does not consult GIT_DIR / GIT_WORK_TREE: with no `.git`
// in the tree, repository rules stay off even when the environment names a
// real repository.
{
	const repo = path.join(testRoot, "env-repo");
	write(repo, ".git/HEAD", HEAD);
	await capture(
		"GIT_DIR and GIT_WORK_TREE do not enable repository rules",
		{ ".gitignore": "drop.txt\n", "drop.txt": "", "keep.txt": "" },
		{ gitignore: true },
		{ ...neutralEnv, GIT_DIR: path.join(repo, ".git"), GIT_WORK_TREE: repo },
		{
			files: { ".gitignore": "drop.txt\n" },
			environment: { GIT_DIR: "<a real repository>", GIT_WORK_TREE: "<its work tree>" },
			options: { gitignore: true },
		},
		["the frozen walker keys on .git in the tree; the environment variables are not consulted"],
	);
}

// Bare repositories: no `.git` inside a bare layout, no rules apply even
// though the directory looks repository-shaped.
await capture(
	"a bare repository layout applies no repository rules",
	{
		".gitignore": "drop.txt\n",
		"HEAD": HEAD,
		"config": "[core]\n\tbare = true\n",
		"objects/info": "",
		"drop.txt": "",
		"keep.txt": "",
	},
	{ gitignore: true },
	neutralEnv,
	{ files: { ".gitignore": "drop.txt\n", "config": "[core] bare = true" }, options: { gitignore: true } },
);

// A `.git` symlink to a directory is a repository marker (metadata follows
// the link), and the exclude file is read through it.
{
	const root = path.join(testRoot, "case-gitlink");
	const realGit = path.join(root, "real.git");
	write(realGit, "HEAD", HEAD);
	write(realGit, "info/exclude", "excluded-by-link.txt\n");
	write(root, ".gitignore", "dropped.txt\n");
	write(root, "dropped.txt", "");
	write(root, "excluded-by-link.txt", "");
	write(root, "keep.txt", "");
	execFileSync("ln", ["-s", "real.git", path.join(root, ".git")]);
	applyEnv(neutralEnv);
	const connection = new Connection({ command: [daemonBinary()] });
	try {
		const execEnv = new LocalExecutionEnv({ cwd: root, connection });
		const listing = getOrThrow(await execEnv.listFiles(".", { gitignore: true }, context));
		scenarios.push({
			name: "a git symlink to a directory is a repository marker",
			inputs: {
				files: { ".gitignore": "dropped.txt\n", "dropped.txt": "", "excluded-by-link.txt": "", "keep.txt": "" },
				gitSymlink: "real.git",
				exclude: "excluded-by-link.txt\n",
				options: { gitignore: true },
			},
			expected: listed(listing),
			normalizations: [],
		});
	} finally {
		await connection.close();
	}
}

// Unreadable ignore files degrade to no rules rather than an error.
{
	const root = path.join(testRoot, "case-unreadable");
	write(root, ".git/HEAD", HEAD);
	const ignore = path.join(root, ".gitignore");
	writeFileSync(ignore, "drop.txt\n");
	chmodSync(ignore, 0o000);
	write(root, "drop.txt", "");
	write(root, "keep.txt", "");
	applyEnv(neutralEnv);
	const connection = new Connection({ command: [daemonBinary()] });
	try {
		const execEnv = new LocalExecutionEnv({ cwd: root, connection });
		const listing = getOrThrow(await execEnv.listFiles(".", { gitignore: true }, context));
		scenarios.push({
			name: "an unreadable gitignore degrades to no rules",
			inputs: { files: { ".gitignore": "drop.txt\n (mode 000)", "drop.txt": "", "keep.txt": "" }, options: { gitignore: true } },
			expected: listed(listing),
			normalizations: ["the ignore file exists but cannot be opened; the walker applies no rules from it"],
		});
	} finally {
		await connection.close();
		chmodSync(ignore, 0o644);
	}
}

// Global excludes through the git config surface. The ignore crate resolves
// `core.excludesFile` with a line scan of, in order: GIT_CONFIG_GLOBAL,
// $HOME/.gitconfig, $XDG_CONFIG_HOME/git/config, GIT_CONFIG_SYSTEM (or
// /etc/gitconfig); the first config that names a value wins, `~` expands
// against $HOME, a quoted value is unquoted by the scan, and a missing file
// falls through to the next source.
{
	const configFile = (dir: string, value: string) => write(dir, "gitconfig", `[core]\n\texcludesFile = ${value}\n`);
	const envBase = path.join(testRoot, "cfg-env");
	const home = path.join(testRoot, "cfg-home");
	const xdg = path.join(testRoot, "cfg-xdg");
	const sys = path.join(testRoot, "cfg-sys");
	const globalFile = path.join(testRoot, "cfg-global.txt");
	const homeFile = path.join(testRoot, "cfg-home.txt");
	const xdgFile = path.join(testRoot, "cfg-xdg.txt");
	const sysFile = path.join(testRoot, "cfg-sys.txt");
	for (const [file, rule] of [
		[globalFile, "from-global-config.txt\n"],
		[homeFile, "from-home-config.txt\n"],
		[xdgFile, "from-xdg-config.txt\n"],
		[sysFile, "from-system-config.txt\n"],
	] as const) {
		write(path.dirname(file), path.basename(file), rule);
	}
	for (const dir of [envBase, home, xdg, sys, path.join(testRoot, "cfg-empty-home"), path.join(xdg, "git")])
		mkdirSync(dir, { recursive: true });
	// Every source names a different excludes file: GIT_CONFIG_GLOBAL wins.
	configFile(envBase, globalFile);
	write(home, ".gitconfig", `[core]\n\texcludesFile = ${homeFile}\n`);
	write(xdg, "git/config", `[core]\n\texcludesFile = ${xdgFile}\n`);
	configFile(sys, sysFile);
	await capture(
		"GIT_CONFIG_GLOBAL wins over home xdg and system config",
		{
			...gitDir,
			"from-global-config.txt": "",
			"from-home-config.txt": "",
			"from-xdg-config.txt": "",
			"from-system-config.txt": "",
			"keep.txt": "",
		},
		{ gitignore: true },
		{
			HOME: home,
			XDG_CONFIG_HOME: xdg,
			GIT_CONFIG_GLOBAL: path.join(envBase, "gitconfig"),
			GIT_CONFIG_SYSTEM: path.join(sys, "gitconfig"),
		},
		{
			files: ["from-global-config.txt", "from-home-config.txt", "from-xdg-config.txt", "from-system-config.txt"],
			config: {
				"GIT_CONFIG_GLOBAL": globalFile,
				"$HOME/.gitconfig": homeFile,
				"$XDG_CONFIG_HOME/git/config": xdgFile,
				"GIT_CONFIG_SYSTEM": sysFile,
			},
			options: { gitignore: true },
		},
	);
	// $HOME/.gitconfig beats XDG, which beats the system config.
	await capture(
		"home gitconfig wins over xdg and system config",
		{
			...gitDir,
			"from-home-config.txt": "",
			"from-xdg-config.txt": "",
			"from-system-config.txt": "",
			"keep.txt": "",
		},
		{ gitignore: true },
		{
			HOME: home,
			XDG_CONFIG_HOME: xdg,
			GIT_CONFIG_GLOBAL: path.join(testRoot, "cfg-missing"),
			GIT_CONFIG_SYSTEM: path.join(sys, "gitconfig"),
		},
		{
			files: ["from-home-config.txt", "from-xdg-config.txt", "from-system-config.txt"],
			config: { "$HOME/.gitconfig": homeFile, "$XDG_CONFIG_HOME/git/config": xdgFile, "GIT_CONFIG_SYSTEM": sysFile },
			options: { gitignore: true },
		},
	);
	// The system config applies when no global source names a value.
	await capture(
		"the system config names the excludes file last",
		{
			...gitDir,
			"from-system-config.txt": "",
			"keep.txt": "",
		},
		{ gitignore: true },
		{
			HOME: path.join(testRoot, "cfg-empty-home"),
			XDG_CONFIG_HOME: xdg,
			GIT_CONFIG_GLOBAL: path.join(testRoot, "cfg-missing"),
			GIT_CONFIG_SYSTEM: path.join(sys, "gitconfig"),
		},
		{
			files: ["from-system-config.txt"],
			config: { "GIT_CONFIG_SYSTEM": sysFile },
			options: { gitignore: true },
		},
	);
	// A quoted value with `~` expands against $HOME.
	{
		const tildeFile = path.join(home, "my-excludes.txt");
		writeFileSync(tildeFile, "from-tilde.txt\n");
		write(envBase, "gitconfig-quoted", `[core]\n\texcludesFile = "~/my-excludes.txt"\n`);
		await capture(
			"a quoted excludesFile with tilde expands against home",
			{ ...gitDir, "from-tilde.txt": "", "keep.txt": "" },
			{ gitignore: true },
			{
				HOME: home,
				XDG_CONFIG_HOME: xdg,
				GIT_CONFIG_GLOBAL: path.join(envBase, "gitconfig-quoted"),
				GIT_CONFIG_SYSTEM: path.join(testRoot, "cfg-missing"),
			},
			{
				files: ["from-tilde.txt"],
				config: { "GIT_CONFIG_GLOBAL": 'excludesFile = "~/my-excludes.txt" (quoted)' },
				options: { gitignore: true },
			},
		);
	}
	// No config names a value: the default is $XDG_CONFIG_HOME/git/ignore.
	{
		const defaultXdg = path.join(testRoot, "cfg-default-xdg");
		write(defaultXdg, "git/ignore", "from-xdg-default.txt\n");
		await capture(
			"the xdg default ignore file applies without any config",
			{ ...gitDir, "from-xdg-default.txt": "", "keep.txt": "" },
			{ gitignore: true },
			{
				HOME: path.join(testRoot, "cfg-empty-home"),
				XDG_CONFIG_HOME: defaultXdg,
				GIT_CONFIG_GLOBAL: path.join(testRoot, "cfg-missing"),
				GIT_CONFIG_SYSTEM: path.join(testRoot, "cfg-missing"),
			},
			{ files: ["from-xdg-default.txt"], config: "none names core.excludesFile", options: { gitignore: true } },
		);
	}
}

const listingEndpoints = [
	"packages/env/daemon/src/list.rs:list,Options",
	"packages/env/daemon/src/frame.rs:MAX_PAYLOAD",
];

const artifacts = [
	{
		path: "discovery.json",
		scenarios,
		sourceEndpoints: listingEndpoints,
	},
];

const manifestArtifacts = [];
const artifactBytes = new Map<string, Buffer>();
try {
	for (const artifact of artifacts) {
		const document = {
			schema: "pike/pi-v1.3.0-audit-evidence/1",
			baseline: baselineName,
			revision: baseline.revision,
			capturedAt,
			environment,
			sourceEndpoints: artifact.sourceEndpoints,
			scenarios: artifact.scenarios,
		};
		const bytes = Buffer.from(`${JSON.stringify(document, null, 2)}\n`);
		artifactBytes.set(artifact.path, bytes);
		manifestArtifacts.push({
			path: artifact.path,
			family,
			bytes: bytes.length,
			sha256: sha256(bytes),
			sourceEndpoints: artifact.sourceEndpoints,
			scenarios: artifact.scenarios,
		});
	}
} finally {
	rmSync(testRoot, { recursive: true, force: true });
}

const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1199",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(readFileSync(scriptPath)),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(readFileSync(scriptPath)) }],
	},
	artifacts: manifestArtifacts,
};

// Write only after every guard and scenario succeeded.
mkdirSync(familyDir, { recursive: true });
for (const [relative, bytes] of artifactBytes) writeFileSync(path.join(familyDir, relative), bytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);

console.log(
	`captured ${family} (${manifestArtifacts.length} artifacts, ${scenarios.length} scenarios) for ${baselineName} (${baseline.revision})`,
);
