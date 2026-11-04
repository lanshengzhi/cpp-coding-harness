#!/usr/bin/env tsx
/**
 * Capture the `audit-1198` frozen ignore-rule observations for the shared
 * pi-v1.3.0 evidence registry.
 *
 * The bounded tree listing honors git ignore rules with frozen precedence:
 * the pi-env daemon (`packages/env/daemon/src/list.rs`) walks with the Rust
 * `ignore` crate, so these scenarios record the daemon's own output — the
 * acceptance authority for #1198 — over fixture work trees with a controlled
 * environment (no global excludes leak in: HOME/XDG/GIT_CONFIG_* point at
 * empty fixtures unless a scenario declares otherwise).
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1198/capture-1198.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1198";
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
const testRoot = mkdtempSync(path.join(tmpdir(), "cch-audit-1198-"));

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

/** The controlled environment every scenario daemon runs with (no host git config leaks). */
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

function daemonEnvironment(root: string, env: Record<string, string> = neutralEnv): any {
	applyEnv(env);
	const connection = new Connection({ command: [daemonBinary()] });
	return { env: new LocalExecutionEnv({ cwd: root, connection }), connection };
}

function listed(listing: any) {
	return {
		entries: listing.entries.map((value: any) => ({ path: value.path, kind: value.kind })),
		truncated: listing.truncated,
	};
}

async function list(root: string, files: Record<string, string | Uint8Array>, options: any, env?: Record<string, string>) {
	for (const [relative, content] of Object.entries(files)) write(root, relative, content);
	const { env: execEnv, connection } = daemonEnvironment(root, env);
	try {
		return getOrThrow(await execEnv.listFiles(".", options, context));
	} finally {
		await connection.close();
	}
}

function scenario(name: string, inputs: any, expected: any, normalizations: string[] = []) {
	return { name, inputs, expected, normalizations };
}

const scenarios = [];
const workTreeFiles = { ".git/HEAD": "ref: refs/heads/main\n" };

// Rule syntax matrix. Every fixture is a work tree (`.git/` present), the
// environment is neutral (no global excludes), and `hidden` stays default.
{
	const base = { ...workTreeFiles, "keep.txt": "" };
	let caseIndex = 0;
	const run = async (name: string, gitignore: string, extra: Record<string, string>, options: any = {}) => {
		// One fresh work tree per case: earlier rules must not leak into later
		// listings, and the recorded inputs describe the whole tree.
		const root = path.join(testRoot, "syntax", `case-${caseIndex++}`);
		const files = { ...base, ...extra, ".gitignore": gitignore };
		const listing = await list(root, files, { gitignore: true, ...options });
		scenarios.push(
			scenario(name, { gitignore, files, options: { gitignore: true, ...options } }, listed(listing)),
		);
	};
	await run("comments and blank lines carry no rules", "# nothing\n\n   \nkeep.txt\n", { "drop.txt": "" });
	await run("escaped hash and bang are literal patterns", "\\#weird\n\\!odd\n", { "#weird": "", "!odd": "", "other": "" });
	await run("trailing spaces are stripped unless escaped", "spaced.txt\nescaped\\ .txt\n", { "spaced.txt": "", "escaped .txt": "", "other": "" });
	await run("a negation whitelists a matching name", "*.log\n!keep.log\n", { "drop.log": "", "keep.log": "" });
	await run("a leading slash anchors to the rules directory", "/root.txt\n", { "root.txt": "", "sub/root.txt": "" });
	await run("a trailing slash matches directories only", "build/\n", { "build/x": "", "builder": "" });
	await run("star does not cross a slash", "a*b\n", { "axb": "", "a/x/b": "", "a/xb": "" });
	await run("a star-star prefix matches at any depth", "**/deep.txt\n", { "deep.txt": "", "sub/deep.txt": "", "sub/mid/deep.txt": "" });
	await run("a question mark matches one non-slash character", "f?o.txt\n", { "foo.txt": "", "fo.txt": "", "f/o.txt": "" });
	await run("character classes and ranges match one character", "c[a]t.txt\nc[a-z]t2.txt\n", { "cat.txt": "", "cat2.txt": "", "ct.txt": "" });
	await run("a negated class matches outside the class", "n[!o].txt\n", { "nix.txt": "", "no.txt": "" });
	await run("a middle star-star crosses directories", "mid/**/end.txt\n", { "mid/end.txt": "", "mid/a/end.txt": "", "mid/a/b/end.txt": "", "midx/a/end.txt": "" });
	await run("a trailing star-star matches everything inside only", "inside/**\n", { "inside/x": "", "inside/a/b": "", "outsidex": "" });
	await run("brace alternation is a globset extension over git", "alt.{txt,log}\n", { "alt.txt": "", "alt.log": "", "alt.md": "" });
	await run("rules are case sensitive on Linux bytes", "*.LOG\n", { "x.log": "", "x.LOG": "" });
	await run("the last matching rule in one file wins", "*.log\n!keep.log\ndrop.log\n", { "keep.log": "", "drop.log": "" });
}

// Precedence between rule sources and directories.
{
	const root = path.join(testRoot, "precedence");
	// Nearest .gitignore beats the parent file, in both directions.
	{
		const files = {
			...workTreeFiles,
			".gitignore": "*.log\n!keep.log\n",
			"sub/.gitignore": "keep.log\n",
			"sub/drop.log": "",
			"sub/keep.log": "",
			"top.log": "",
			"keep.log": "",
		};
		const listing = await list(path.join(root, "nearest"), files, { gitignore: true });
		scenarios.push(
			scenario("the nearest gitignore overrides the parent in either direction", { files, options: { gitignore: true } }, listed(listing)),
		);
	}
	// .gitignore beats .git/info/exclude; exclude beats the global file.
	{
		const files = {
			...workTreeFiles,
			".gitignore": "from-ignore\nfrom-both\n",
			".git/info/exclude": "from-exclude\nfrom-both\n",
			"from-ignore": "",
			"from-exclude": "",
			"from-both": "",
		};
		const home = path.join(testRoot, "prec-xdg");
		write(home, "git/ignore", "from-global\nfrom-global-extra\n");
		const listing = await list(
			path.join(root, "sources"),
			{ ...files, "from-global": "", "from-global-extra": "" },
			{ gitignore: true },
			{ ...neutralEnv, XDG_CONFIG_HOME: home, GIT_CONFIG_GLOBAL: path.join(testRoot, "prec-none") },
		);
		const allSources = { ...files, "from-global": "", "from-global-extra": "" };
		scenarios.push(
			scenario(
				"gitignore beats info-exclude and the global file",
				{
					files: allSources,
					globalIgnore: "from-global\nfrom-global-extra\n",
					globalSource: "$XDG_CONFIG_HOME/git/ignore (the default path)",
					options: { gitignore: true },
				},
				listed(listing),
			),
		);
	}
	// A whitelist in .gitignore keeps matching through exclude and global.
	{
		const files = {
			...workTreeFiles,
			".gitignore": "*.log\n!keep.log\n",
			".git/info/exclude": "keep.log\n",
			"keep.log": "",
			"drop.log": "",
		};
		const listing = await list(path.join(root, "whitelist"), files, { gitignore: true });
		scenarios.push(
			scenario("a whitelist in gitignore suppresses the exclude file", { files, options: { gitignore: true } }, listed(listing)),
		);
	}
	// An ignored directory prunes its subtree; a negation below it cannot revive.
	{
		const files = {
			...workTreeFiles,
			".gitignore": "build/\n!build/keep.txt\n",
			"build/keep.txt": "",
			"build/drop.txt": "",
			"src/keep.txt": "",
		};
		const listing = await list(path.join(root, "prune"), files, { gitignore: true });
		scenarios.push(
			scenario(
				"an ignored directory prunes its subtree and a negation below cannot revive it",
				{ files, options: { gitignore: true } },
				listed(listing),
			),
		);
	}
}

// Interaction with the listing options.
{
	const root = path.join(testRoot, "interaction");
	// A whitelisted hidden name survives hidden:false; plain hidden names drop.
	{
		const files = {
			...workTreeFiles,
			".gitignore": "!.secret-keep\n",
			".secret-keep": "",
			".secret-drop": "",
			"plain": "",
		};
		const listing = await list(path.join(root, "hidden-whitelist"), files, { gitignore: true, hidden: false });
		scenarios.push(
			scenario("hidden false drops dot names unless a rule whitelists them", { files, options: { gitignore: true, hidden: false } }, listed(listing)),
		);
	}
	// gitignore:false disables the rules but keeps hidden filtering and the
	// .git exclusion.
	{
		const files = {
			...workTreeFiles,
			".gitignore": "drop.txt\n",
			".hidden/x": "",
			"drop.txt": "",
			"keep.txt": "",
		};
		const off = await list(path.join(root, "gitignore-off"), files, { gitignore: false });
		const on = await list(path.join(root, "gitignore-off"), files, { gitignore: true });
		scenarios.push(
			scenario(
				"gitignore false disables the rules while hidden filtering stays",
				{ files, optionsOff: { gitignore: false }, optionsOn: { gitignore: true } },
				{ off: listed(off), on: listed(on) },
			),
		);
	}
	// Symlinks list as links (dangling ones too); a FIFO is skipped.
	{
		const dir = path.join(root, "kinds");
		const files = { ...workTreeFiles, "target.txt": "", "dir/inner.txt": "" };
		for (const [relative, content] of Object.entries(files)) write(dir, relative, content);
		write(dir, ".gitignore", "");
		const { execFileSync: run } = await import("node:child_process");
		run("ln", ["-s", "target.txt", path.join(dir, "link")]);
		run("ln", ["-s", "missing", path.join(dir, "dangling")]);
		run("mkfifo", [path.join(dir, "pipe")]);
		const { env: execEnv, connection } = daemonEnvironment(dir);
		try {
			const listing = getOrThrow(await execEnv.listFiles(".", { gitignore: true }, context));
			scenarios.push(
				scenario(
					"symlinks list as links and a fifo is skipped",
					{
						files: { "target.txt": "", "dir/inner.txt": "", ".gitignore": "" },
						links: ["link -> target.txt", "dangling -> missing"],
						fifo: "pipe",
						options: { gitignore: true },
					},
					listed(listing),
				),
			);
		} finally {
			await connection.close();
		}
	}
	// A non-UTF-8 name lists with replacement characters.
	{
		const dir = path.join(root, "nonutf8");
		for (const [relative, content] of Object.entries({ ...workTreeFiles, "ok.txt": "" })) write(dir, relative, content);
		write(dir, ".gitignore", "");
		writeFileSync(Buffer.concat([Buffer.from(dir + path.sep), Buffer.from([0x62, 0x61, 0x64, 0xff, 0x2e, 0x74, 0x78, 0x74])]), "");
		const { env: execEnv, connection } = daemonEnvironment(dir);
		try {
			const listing = getOrThrow(await execEnv.listFiles(".", { gitignore: true }, context));
			scenarios.push(
				scenario(
					"a non-utf8 name lists with replacement characters",
					{ files: { "ok.txt": "", ".gitignore": "", "<bytes 62 61 64 ff 2e 74 78 74>": "" }, options: { gitignore: true } },
					listed(listing),
					["the payload path is the lossy UTF-8 rendering (U+FFFD for byte 0xff), not the raw name"],
				),
			);
		} finally {
			await connection.close();
		}
	}
}

// Bounds: zero, exact and over, for the entry and byte limits and the timeout.
{
	const root = path.join(testRoot, "bounds");
	const three = { ...workTreeFiles, "a": "", "b": "", "c": "" };
	const exactBytes = 2 + 1 + 2 + 1 + 2 + 1; // f a NUL f b NUL f c NUL = 9
	const cases: any[] = [];
	for (const options of [
		{ maxEntries: 0 },
		{ maxEntries: 2 },
		{ maxEntries: 3 },
		{ maxEntries: 4 },
		{ maxBytes: 0 },
		{ maxBytes: 9 },
		{ maxBytes: 10 },
		{ timeoutMs: 0 },
		{ timeoutMs: 60000 },
	]) {
		const listing = await list(path.join(root, "three"), three, { gitignore: false, ...options });
		cases.push({ options, listing: listed(listing) });
	}
	scenarios.push(
		scenario(
			"zero exact and over bounds report truncated exactly",
			{ files: ["a", "b", "c"], gitignore: false, exactBytes },
			{ cases },
			["each entry costs its UTF-8 path plus two bytes; a timeout of zero expires before the first entry"],
		),
	);
}

// Shape: wide directory, deep nesting, byte-order sibling sort, empty directory.
{
	const root = path.join(testRoot, "shape");
	const files: Record<string, string> = { ...workTreeFiles };
	for (let i = 0; i < 30; i++) files[`w${String(i).padStart(2, "0")}`] = "";
	files["deep/a/b/c/leaf.txt"] = "";
	files["B.txt"] = "";
	files["a.txt"] = "";
	files["empty/"] = "";
	files["deep/z/last.txt"] = "";
	const listing = await list(path.join(root, "tree"), files, { gitignore: false });
	scenarios.push(
		scenario(
			"depth-first byte-order traversal with an empty directory",
			{
				files: [
					...Array.from({ length: 30 }, (_, i) => `w${String(i).padStart(2, "0")}`),
					"deep/a/b/c/leaf.txt",
					"B.txt",
					"a.txt",
					"empty/",
					"deep/z/last.txt",
				],
				options: { gitignore: false },
			},
			listed(listing),
			["siblings sort by raw name bytes: B.txt before a.txt before w00"],
		),
	);
}

const listingEndpoints = [
	"packages/env/daemon/src/list.rs:list,Options",
	"packages/durable/src/env/index.ts:ListFilesOptions,FileListEntry,FileListing",
];

const artifacts = [
	{
		path: "ignore-rules.json",
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
	ticket: "1198",
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
