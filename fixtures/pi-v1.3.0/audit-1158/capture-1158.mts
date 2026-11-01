#!/usr/bin/env tsx
/**
 * Capture the `audit-1158` bounded EnvIO observations for the shared pi-v1.3.0
 * evidence registry.
 *
 * The frozen environment exposes three surfaces this ticket aligns:
 *
 * - `BinaryReader.read(offset, length)` returns up to `length` bytes from
 *   `offset` and fewer only at end of file; an offset at or past the end reads
 *   an empty result instead of failing (`packages/durable/src/env/index.ts:99`).
 * - `DirReader.next(maxEntries)` pages one directory, continuing where the
 *   previous call stopped and reporting `done` (`env/index.ts:162-168`).
 * - `FileSystem.listFiles` walks a whole tree depth first with the frozen
 *   `maxEntries` / `maxBytes` bounds and `hidden` / `exclude` filters, and
 *   reports `truncated` (`env/index.ts:177-208`, implemented by the pi-env
 *   daemon at `packages/env/daemon/src/list.rs`).
 *
 * `NodeExecutionEnv` implements the first two directly. `listFiles` is the
 * optional daemon-backed member (`packages/env/src/list-files.ts`), so this
 * capture starts the checkout's own daemon; it is built from the frozen
 * `packages/env/daemon` sources with `cargo build --locked` when the binary is
 * missing. Every call disables `gitignore`, whose result depends on the host's
 * git configuration and which this runtime does not claim.
 *
 * The capture refuses a checkout whose HEAD is not the registered baseline
 * revision and a modified source tree, and it refuses to run once the family is
 * registered to another baseline.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1158/capture-1158.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1158";
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
const { NodeExecutionEnv } = await import("@earendil-works/pi-durable/env/node");
const { getOrThrow } = await import("@earendil-works/pi-durable/env");
const { Connection, LocalExecutionEnv } = await import("@earendil-works/pi-env");

const context = BACKGROUND_CONTEXT;
const testRoot = mkdtempSync(path.join(tmpdir(), "cch-audit-1158-"));
const utf8 = new TextDecoder("utf-8", { fatal: true });

/** Write `content` at `relative` below `root`, creating parent directories. */
function write(root: string, relative: string, content = ""): void {
	const target = path.join(root, relative);
	mkdirSync(path.dirname(target), { recursive: true });
	writeFileSync(target, content);
}

function page(path_: string, kind: string, size: number) {
	return { name: path_, kind, size };
}

function entry(path_: string, kind: string) {
	return { path: path_, kind };
}

const scenarios = {};

/** `DirReader` paging: continuation state and the honest `done` mark. */
scenarios.directoryPages = await (async () => {
	const root = path.join(testRoot, "pages");
	for (const name of ["a", "b", "c"]) write(root, name);
	const env = new NodeExecutionEnv({ cwd: root });
	const reader = getOrThrow(await env.openDirReader(".", context));
	const pages: any[] = [];
	try {
		for (let call = 0; call < 4; call++) {
			const result = getOrThrow(await reader.next(2, context));
			pages.push({
				entries: result.entries.map((info: any) => page(info.name, info.kind, info.size)),
				done: result.done,
			});
			if (result.done) break;
		}
	} finally {
		await reader.close(context);
	}
	const names = pages.flatMap((value: any) => value.entries.map((info: any) => info.name)).sort();
	return {
		name: "directory pages continue where the previous page stopped",
		inputs: { files: ["a", "b", "c"], pageSize: 2 },
		expected: {
			names,
			pageSizes: pages.map((value: any) => value.entries.length),
			done: pages.map((value: any) => value.done),
		},
		normalizations: [
			"entries inside one page are in the order the file system returns them and are compared as a set",
		],
	};
})();

/** `listFiles`: a plain tree, depth first with parents before their entries. */
function daemonEnvironment(root: string): any {
	const connection = new Connection({ command: [daemonBinary()] });
	return { env: new LocalExecutionEnv({ cwd: root, connection }), connection };
}

function listed(listing: any) {
	return {
		entries: listing.entries.map((value: any) => entry(value.path, value.kind)),
		truncated: listing.truncated,
	};
}

scenarios.depthFirst = await (async () => {
	const root = path.join(testRoot, "tree");
	for (const name of ["b.txt", "a/z.txt", "a/b/c.txt", ".hidden/x", "A.txt"]) write(root, name);
	mkdirSync(path.join(root, "empty"), { recursive: true });
	const { env, connection } = daemonEnvironment(root);
	try {
		const listing = getOrThrow(await env.listFiles(".", { gitignore: false }, context));
		const hiddenOff = getOrThrow(await env.listFiles(".", { gitignore: false, hidden: false }, context));
		return {
			name: "listFiles lists files and directories depth first in name order",
			inputs: {
				files: ["b.txt", "a/z.txt", "a/b/c.txt", ".hidden/x", "A.txt"],
				directories: ["empty"],
				options: { gitignore: false },
			},
			expected: { listing: listed(listing), hiddenOff: listed(hiddenOff) },
			normalizations: ["temporary directory path omitted"],
		};
	} finally {
		await connection.close();
	}
})();

/** `listFiles`: the entry and byte bounds, with `truncated` at the first entry beyond a bound. */
scenarios.bounds = await (async () => {
	const root = path.join(testRoot, "bounds");
	for (const name of ["a", "b", "c"]) write(root, name);
	const { env, connection } = daemonEnvironment(root);
	try {
		const cases: any[] = [];
		for (const options of [
			{ maxEntries: 2 },
			{ maxEntries: 3 },
			{ maxEntries: 0 },
			{ maxBytes: 6 },
			{ maxBytes: 5 },
		]) {
			const listing = getOrThrow(await env.listFiles(".", { gitignore: false, ...options }, context));
			cases.push({ options, listing: listed(listing) });
		}
		return {
			name: "listFiles stops at its entry and byte bounds and reports truncated",
			inputs: { files: ["a", "b", "c"], gitignore: false },
			expected: { cases },
			normalizations: ["each entry counts its UTF-8 path plus two bytes"],
		};
	} finally {
		await connection.close();
	}
})();

/** `listFiles`: exclusion rules — hidden names, excluded names with their subtree, and `.git`. */
scenarios.exclusions = await (async () => {
	const root = path.join(testRoot, "exclusions");
	for (const name of [
		"keep.txt",
		"out/x.js",
		"sub/keep.txt",
		"node_modules/m/i.js",
		".hidden/x.txt",
		".git/HEAD",
	])
		write(root, name);
	const { env, connection } = daemonEnvironment(root);
	try {
		const excluded = getOrThrow(
			await env.listFiles(".", { gitignore: false, exclude: ["node_modules"] }, context),
		);
		const hiddenOff = getOrThrow(
			await env.listFiles(".", { gitignore: false, hidden: false, exclude: ["node_modules"] }, context),
		);
		return {
			name: "listFiles applies hidden and excluded names and never lists .git",
			inputs: {
				files: ["keep.txt", "out/x.js", "sub/keep.txt", "node_modules/m/i.js", ".hidden/x.txt", ".git/HEAD"],
				exclude: ["node_modules"],
				gitignore: false,
			},
			expected: { excluded: listed(excluded), hiddenOff: listed(hiddenOff) },
			normalizations: ["temporary directory path omitted"],
		};
	} finally {
		await connection.close();
	}
})();

/** `BinaryReader.read`: positioned reads stop early only at end of file. */
scenarios.positionedReads = await (async () => {
	const root = path.join(testRoot, "reads");
	write(root, "digits.txt", "0123456789abcdef");
	const env = new NodeExecutionEnv({ cwd: root });
	const reader = getOrThrow(await env.openBinaryReader("digits.txt", undefined, context));
	try {
		const reads = [
			{ offset: 0, length: 4 },
			{ offset: 4, length: 4 },
			{ offset: 10, length: 8 },
			{ offset: 4, length: 8192 },
			{ offset: 10, length: 0 },
		];
		const results: any[] = [];
		for (const read of reads) {
			results.push({ ...read, text: utf8.decode(getOrThrow(await reader.read(read.offset, read.length, context))) });
		}
		return {
			name: "positioned reads return up to the requested length and stop early only at end of file",
			inputs: { file: "digits.txt", content: "0123456789abcdef", reads },
			expected: { results },
			normalizations: [],
		};
	} finally {
		await reader.close(context);
	}
})();

/** `BinaryReader.read`: an offset at or past the end is an empty result, not an error. */
scenarios.pastEnd = await (async () => {
	const root = path.join(testRoot, "past-end");
	write(root, "digits.txt", "0123456789abcdef");
	write(root, "empty.txt", "");
	const env = new NodeExecutionEnv({ cwd: root });
	const results: any[] = [];
	for (const [file, reads] of [
		["digits.txt", [{ offset: 16, length: 4 }, { offset: 17, length: 4 }, { offset: 4096, length: 1024 }]],
		["empty.txt", [{ offset: 0, length: 8 }, { offset: 1, length: 8 }]],
	] as const) {
		const reader = getOrThrow(await env.openBinaryReader(file, undefined, context));
		try {
			for (const read of reads) {
				const bytes = getOrThrow(await reader.read(read.offset, read.length, context));
				results.push({ file, ...read, bytes: bytes.length, text: utf8.decode(bytes) });
			}
		} finally {
			await reader.close(context);
		}
	}
	return {
		name: "a read at or past end of file returns the frozen empty result instead of an error",
		inputs: { files: ["digits.txt", "empty.txt"], digits: "0123456789abcdef" },
		expected: { results },
		normalizations: [],
	};
})();

/** `BinaryReader.read`: a range longer than one read chunk crosses internal reads unchanged. */
scenarios.crossChunk = await (async () => {
	const root = path.join(testRoot, "cross-chunk");
	const pattern = "0123456789abcdef";
	const content = pattern.repeat(Math.ceil((3 * 1024 * 1024) / pattern.length)).slice(0, 3 * 1024 * 1024);
	write(root, "large.txt", content);
	const env = new NodeExecutionEnv({ cwd: root });
	const reader = getOrThrow(await env.openBinaryReader("large.txt", undefined, context));
	try {
		const reads = [
			{ offset: 1048576 - 7, length: 64 },
			{ offset: 2 * 1048576, length: 1048576 },
		];
		const results: any[] = [];
		for (const read of reads) {
			const bytes = getOrThrow(await reader.read(read.offset, read.length, context));
			results.push({
				...read,
				bytes: bytes.length,
				sha256: createHash("sha256").update(bytes).digest("hex"),
				head: utf8.decode(bytes.subarray(0, 8)),
				tail: utf8.decode(bytes.subarray(Math.max(0, bytes.length - 8))),
			});
		}
		return {
			name: "a positioned read longer than one internal chunk crosses reads unchanged",
			inputs: { file: "large.txt", bytes: content.length, reads },
			expected: { results },
			normalizations: ["the 3 MiB pattern file is described by length and digest, not stored in the artifact"],
		};
	} finally {
		await reader.close(context);
	}
})();

const positionReads = [
	scenarios.positionedReads,
	scenarios.pastEnd,
	scenarios.crossChunk,
];
const listingScenarios = [
	scenarios.directoryPages,
	scenarios.depthFirst,
	scenarios.bounds,
	scenarios.exclusions,
];

const listingEndpoints = [
	"packages/durable/src/env/index.ts:DirReader.next",
	"packages/durable/src/env/index.ts:ListFilesOptions,FileListEntry,FileListing",
	"packages/durable/src/env/node.ts:NodeDirReader.next",
	"packages/env/daemon/src/list.rs:list,Options",
];
const readEndpoints = [
	"packages/durable/src/env/index.ts:BinaryReader.read",
	"packages/durable/src/env/node.ts:NodeBinaryReader.read",
];

const artifacts = [
	{
		path: "listing.json",
		scenarios: listingScenarios,
		sourceEndpoints: listingEndpoints,
	},
	{
		path: "positioned-read.json",
		scenarios: positionReads,
		sourceEndpoints: readEndpoints,
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
	ticket: "1158",
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

// The listing artifact carries the daemon's own build output; say where it came from.
console.log(
	`captured ${family} (${manifestArtifacts.length} artifacts, ` +
		`${listingScenarios.length + positionReads.length} scenarios) for ${baselineName} (${baseline.revision})`,
);
